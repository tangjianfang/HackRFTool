// 真机接收链定量诊断（#55c 临时工具）：收 2 秒 IQ → 时域统计/频谱峰/
// DC 分量/离线 FM 解调音频质量。定位"扫到一堆台、点开全是噪音"。
#define NOMINMAX
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "dsp/analyzer.hpp"
#include "dsp/fft.hpp"
#include "dsp/fm.hpp"
#include "radio/hackrf.hpp"

namespace {

struct Cap {
    std::vector<std::int8_t> iq;
    std::atomic<std::size_t> got{0};
    std::size_t cap = 0;
};

void cap_trampoline(const std::int8_t* iq, std::size_t bytes, void* ctx) {
    auto* c = static_cast<Cap*>(ctx);
    const std::size_t n = std::min(bytes, c->cap - c->got.load());
    if (n > 0) {
        std::memcpy(c->iq.data() + c->got.load(), iq, n);
        c->got.fetch_add(n);
    }
}

struct AudioCap {
    std::vector<float> l;
    static void cb(const float* li, const float*, std::size_t n, void* ctx) {
        auto* a = static_cast<AudioCap*>(ctx);
        a->l.insert(a->l.end(), li, li + n);
    }
};

double goertzel_db(const std::vector<float>& x, double fs, double f) {
    const double w = 2.0 * hackrftool::dsp::kPi * f / fs;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (const float v : x) {
        const double s0 = v + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 10.0 * std::log10(std::max(p / double(x.size() * x.size()), 1e-20));
}

} // namespace

static int run_analysis(const std::int8_t* cap_iq, std::size_t got, double mhz);

int main(int argc, char** argv) {
    // 文件模式：rx_check file <path.cs8> —— 离线分析落盘 IQ（按 2 Msps 假设）
    if (argc > 2 && std::strcmp(argv[1], "file") == 0) {
        const double fmhz = argc > 3 ? std::atof(argv[3]) : 98.0;
        std::FILE* f = std::fopen(argv[2], "rb");
        if (f == nullptr) {
            std::printf("open file fail\n");
            return 1;
        }
        Cap cap;
        cap.cap = 8 << 20;
        cap.iq.assign(cap.cap, 0);
        const std::size_t got = std::fread(cap.iq.data(), 1, cap.cap, f);
        std::fclose(f);
        std::printf("file %s: %zu bytes, center %.1f MHz (assume 2 Msps)\n",
                    argv[2], got, fmhz);
        // SWAP=1：交换 I/Q（镜像判定 A/B——交换后峰翻边且导频可锁=采集链镜像）
        if (std::getenv("SWAP") != nullptr)
            for (std::size_t k = 0; k < got / 2; ++k)
                std::swap(cap.iq[k * 2], cap.iq[k * 2 + 1]);
        return run_analysis(cap.iq.data(), got, fmhz);
    }
    const double mhz = argc > 1 ? std::atof(argv[1]) : 98.0;
    const unsigned lna = argc > 2 ? unsigned(std::atoi(argv[2])) : 40;
    const unsigned vga = argc > 3 ? unsigned(std::atoi(argv[3])) : 32;
    const bool amp = argc > 4 ? std::atoi(argv[4]) != 0 : true;
    hackrftool::radio::HackRadio radio;
    std::string err;
    if (!radio.open(&err)) {
        std::printf("open fail: %s\n", err.c_str());
        return 1;
    }
    hackrftool::radio::RadioConfig cfg;
    cfg.center_hz = mhz * 1e6;
    cfg.sample_rate_hz = 2e6;
    cfg.lna_gain_db = lna;
    cfg.vga_gain_db = vga;
    cfg.amp = amp;
    if (!radio.apply(cfg, &err)) {
        std::printf("apply fail: %s\n", err.c_str());
        return 1;
    }
    Cap cap;
    cap.cap = 2 * 2000000;   // 2 秒（字节）
    cap.iq.assign(cap.cap, 0);
    // 主程序复现路径：20Msps 配置 → 开流 → 流中切 2Msps
    hackrftool::radio::RadioConfig cfg20 = cfg;
    cfg20.sample_rate_hz = 20e6;
    if (!radio.apply(cfg20, &err)) {
        std::printf("apply20 fail: %s\n", err.c_str());
        return 1;
    }
    if (!radio.start_rx(&cap_trampoline, &cap, &err)) {
        std::printf("start_rx fail: %s\n", err.c_str());
        return 1;
    }
    if (!radio.apply(cfg, &err)) std::printf("mid-stream switch: %s\n", err.c_str());
    // 修复时序复现：停流→配置→重开（reconfigure_rx 等价路径）
    radio.stop_rx();
    if (!radio.apply(cfg, &err)) std::printf("re-apply fail: %s\n", err.c_str());
    if (!radio.start_rx(&cap_trampoline, &cap, &err))
        std::printf("re-start fail: %s\n", err.c_str());
    std::printf("receiving 2s @ %.1f MHz LNA%u VGA%u amp=%d ...\n", mhz, lna, vga,
                int(amp));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    radio.stop_rx();
    return run_analysis(cap.iq.data(), cap.got.load(), mhz);
}

// 时域/频谱/离线解调三段定量分析（live 与 file 模式共用）
static int run_analysis(const std::int8_t* cap_iq, std::size_t got, double mhz) {
    const std::size_t n = got / 2;
    std::printf("captured %zu samples\n", n);
    if (n < 100000) {
        std::printf("FAIL: insufficient data (device wedge?)\n");
        return 1;
    }

    // ---- 时域统计（饱和检测）----
    long sat_i = 0, sat_q = 0;
    double sum_i = 0, sum_q = 0, sumsq = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const int i = cap_iq[k * 2], q = cap_iq[k * 2 + 1];
        if (i >= 126 || i <= -126) ++sat_i;
        if (q >= 126 || q <= -126) ++sat_q;
        sum_i += i;
        sum_q += q;
        sumsq += double(i) * i + double(q) * q;
    }
    const double rms = std::sqrt(sumsq / double(2 * n));
    std::printf("time-domain: rms=%.1f  satI=%.2f%% satQ=%.2f%%  DC(I)=%.2f DC(Q)=%.2f\n",
                rms, 100.0 * sat_i / double(n), 100.0 * sat_q / double(n),
                sum_i / double(n), sum_q / double(n));

    // ---- 频谱峰（analyzer：512 FFT，裁边后取前 8 峰）----
    hackrftool::dsp::SpectrumAnalyzer an(512, 64, 256);
    an.feed(cap_iq, got);
    const auto f = an.snapshot();
    if (!f.db.empty()) {
        std::vector<std::pair<float, int>> pk;
        for (int i = 0; i < int(f.db.size()); ++i) {
            bool is_pk = (i == 0 || f.db[i] > f.db[i - 1]) &&
                         (i == int(f.db.size()) - 1 || f.db[i] >= f.db[i + 1]);
            if (is_pk) pk.emplace_back(f.db[i], i);
        }
        std::sort(pk.begin(), pk.end(),
                  [](auto& a, auto& b) { return a.first > b.first; });
        std::printf("spectrum peaks (bin 0=%.1fk .. %d=%.1fk MHz span):\n",
                    (mhz - 1.0), int(f.db.size()) - 1, mhz + 1.0);
        for (int k = 0; k < 8 && k < int(pk.size()); ++k) {
            const double off = -1.0 + 2.0 * pk[k].second / double(f.db.size() - 1);
            std::printf("  peak %d: %.1f dB @ %+.3f MHz (%.1f MHz) bin=%d\n", k,
                        pk[k].first, off, mhz + off, pk[k].second);
        }
    }

    // ---- 离线 FM 解调音频质量 ----
    const bool force_mono = std::getenv("MONO") != nullptr;
    AudioCap ac;
    hackrftool::dsp::FmReceiver rx(2e6, 300.0, force_mono);
    rx.set_audio_callback(&AudioCap::cb, &ac);
    rx.feed(cap_iq, got);
    if (ac.l.size() > 8000) {
        const std::size_t sk = std::min<std::size_t>(4800, ac.l.size() / 4);
        const std::vector<float> mid(ac.l.begin() + sk, ac.l.end());
        double sum = 0, mx = 0;
        for (const float v : mid) {
            sum += double(v) * v;
            mx = std::max(mx, std::abs(double(v)));
        }
        const double arms = std::sqrt(sum / mid.size());
        const double n_floor = goertzel_db(mid, 48e3, 8000.0);   // 无话音能量区
        const double b_300 = goertzel_db(mid, 48e3, 300.0);      // 话音基带
        const double b_1k = goertzel_db(mid, 48e3, 1000.0);
        const double b_3k = goertzel_db(mid, 48e3, 3000.0);
        std::printf("audio[%s]: rms=%.3f peak=%.3f  tone_db: 300Hz=%.1f 1k=%.1f "
                    "3k=%.1f 8k(noise)=%.1f  (1k-8k gap=%.1f dB)\n",
                    force_mono ? "mono" : "auto", arms, mx, b_300, b_1k, b_3k, n_floor, b_1k - n_floor);
    }
    std::printf("pilot=%.3f stereo=%d\n", ac.l.empty() ? 0.0f : 0.0f, 0);
    return 0;
}
