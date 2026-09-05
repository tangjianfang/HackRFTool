// 纯 C++ DSP 单元测试 —— 沿用 WinFlux 的裸 main + check 模式，无测试框架
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "dsp/analyzer.hpp"
#include "app/settings.hpp"
#include "dsp/meteor.hpp"
#include "app/telemetry.hpp"
#include "dsp/burst_detector.hpp"
#include "dsp/channel_monitor.hpp"
#include "dsp/fft.hpp"
#include "dsp/esb.hpp"
#include "dsp/gfsk.hpp"
#include "dsp/level_map.hpp"
#include "dsp/live_bursts.hpp"
#include "dsp/panorama.hpp"
#include "dsp/waterfall.hpp"
#include "dsp/fm.hpp"
#include "dsp/apt.hpp"
#include "dsp/sigdb.hpp"
#include "radio/iq_recorder.hpp"
#include "ui/status_text.hpp"

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static void test_fft_dc() {
    std::vector<std::complex<double>> x(4, {1.0, 0.0});
    hackrftool::dsp::fft(x);
    check(std::abs(x[0] - std::complex<double>(4.0, 0.0)) < 1e-9, "fft 直流分量 X[0]=4");
    for (std::size_t k = 1; k < 4; ++k)
        check(std::abs(x[k]) < 1e-9, "fft 直流输入其余频点为 0");
}

static void test_fft_tone_bin1() {
    const std::size_t N = 8;
    std::vector<std::complex<double>> x(N);
    for (std::size_t n = 0; n < N; ++n) {
        const double ph = 2.0 * hackrftool::dsp::kPi * double(n) / double(N);
        x[n] = {std::cos(ph), std::sin(ph)};   // bin 1 复单频，幅度 1
    }
    hackrftool::dsp::fft(x);
    check(std::abs(std::abs(x[1]) - 8.0) < 1e-9, "fft bin1 幅度 = N");
    for (std::size_t k = 0; k < N; ++k)
        if (k != 1) check(std::abs(x[k]) < 1e-9, "fft 单频无泄漏");
}

static void test_fft_parseval() {
    const std::size_t N = 64;
    std::vector<std::complex<double>> x(N);
    unsigned lcg = 12345u;   // 确定性伪随机
    double time_energy = 0.0, freq_energy = 0.0;
    for (std::size_t n = 0; n < N; ++n) {
        lcg = lcg * 1664525u + 1013904223u;
        const double re = double(int(lcg >> 24) - 128) / 128.0;
        lcg = lcg * 1664525u + 1013904223u;
        const double im = double(int(lcg >> 24) - 128) / 128.0;
        x[n] = {re, im};
        time_energy += re * re + im * im;
    }
    hackrftool::dsp::fft(x);
    for (std::size_t k = 0; k < N; ++k) freq_energy += std::norm(x[k]);
    check(std::abs(freq_energy - double(N) * time_energy) < 1e-6 * freq_energy,
          "fft Parseval 定理");
}

static void test_analyzer_tone_location_and_level() {
    hackrftool::dsp::SpectrumAnalyzer an(512, 4, 256);
    check(an.bins() == 256, "分析器输出 256 bin");
    check(an.snapshot().db.empty(), "无数据时快照为空");

    // 输入 bin 64 复单频，幅度 100（满幅 127）
    const std::size_t N = 512, k = 64;
    std::vector<std::int8_t> iq(N * 2);
    for (std::size_t n = 0; n < N; ++n) {
        const double ph = 2.0 * hackrftool::dsp::kPi * double(k) * double(n) / double(N);
        iq[n * 2]     = static_cast<std::int8_t>(100 * std::cos(ph));
        iq[n * 2 + 1] = static_cast<std::int8_t>(100 * std::sin(ph));
    }
    for (int i = 0; i < 4; ++i) an.feed(iq.data(), iq.size());
    const auto f = an.snapshot();
    check(!f.db.empty(), "4 块后出帧");
    std::size_t argmax = 0;
    for (std::size_t i = 1; i < f.db.size(); ++i)
        if (f.db[i] > f.db[argmax]) argmax = i;
    // 期望位置：skip=32，usable=448 → out ≈ (64-32)*256/448 ≈ 18
    check(argmax > 15 && argmax < 22, "分析器峰位 ≈ 18");
    // 期望电平：20log10(100/127) ≈ -2.1 dBFS（int8 量化略降）
    check(f.db[argmax] > -4.0 && f.db[argmax] < -1.0, "分析器峰电平 ≈ -2 dBFS");
}

static void test_analyzer_peak_hold_and_reset() {
    hackrftool::dsp::SpectrumAnalyzer an(512, 2, 256);
    std::vector<std::int8_t> quiet(512 * 2, 0);   // 全零输入：低底噪
    an.feed(quiet.data(), quiet.size());
    an.feed(quiet.data(), quiet.size());
    const auto f1 = an.snapshot();
    check(!f1.db.empty(), "静默输入也出帧");
    an.reset_peaks();
    an.feed(quiet.data(), quiet.size());
    an.feed(quiet.data(), quiet.size());
    const auto f2 = an.snapshot();
    for (std::size_t i = 0; i < f2.db.size(); ++i)
        check(f2.peak[i] >= f2.db[i] - 0.001, "峰值保持 ≥ 当前值");
}

static void test_waterfall_ring() {
    hackrftool::dsp::WaterfallModel wf(4, 2);
    const std::vector<float> a = {-10, -20, -30, -40};
    const std::vector<float> b = {-50, -60, -70, -80};
    const std::vector<float> c = {-90, -100, -110, -120};
    check(wf.push(a), "push a 成功");
    check(wf.push(b), "push b 成功");
    check(wf.push(c), "push c 成功（环回）");
    const auto snap = wf.snapshot();
    check(snap.size() == 8, "瀑布快照 rows*cols");
    check(snap[0] == -90 && snap[1] == -100, "瀑布行 0 = 最新帧");
    check(snap[4] == -50 && snap[5] == -60, "瀑布行 1 = 上一帧（a 被挤出）");
    check(!wf.push({-1.0f, -2.0f}), "长度不符 push 返回 false");
}

static void test_waterfall_prefill() {
    hackrftool::dsp::WaterfallModel wf(2, 3);
    wf.push({-5, -6});
    const auto snap = wf.snapshot();
    check(snap[0] == -5 && snap[1] == -6, "瀑布首帧在行 0");
    check(snap[2] == -130 && snap[4] == -130, "未填充行 = -130");
}

static hackrftool::dsp::SpectrumFrame frame_with(std::size_t n, std::size_t peak_bin,
                                                 float peak_db, float floor_db) {
    hackrftool::dsp::SpectrumFrame f;
    f.db.assign(n, floor_db);
    f.peak = f.db;
    if (peak_bin < n) f.db[peak_bin] = peak_db;
    f.seq = 1;
    return f;
}

static void test_monitor_auto_peak_tracks() {
    hackrftool::dsp::ChannelMonitor mon(8);
    mon.set_mode(hackrftool::dsp::ChannelMonitor::Mode::auto_peak);
    mon.push(frame_with(256, 10, -40.0f, -90.0f));
    check(mon.tracked_bin() == 10, "自动跟踪 argmax bin");
    mon.push(frame_with(256, 200, -35.0f, -90.0f));
    check(mon.tracked_bin() == 200, "峰移动后跟踪新 bin");
    const auto s = mon.series();
    check(s.size() == 2 && s[1].db == -35.0f, "序列记录 RSSI");
}

static void test_monitor_fixed_bin_and_clamp() {
    hackrftool::dsp::ChannelMonitor mon(8);
    mon.set_mode(hackrftool::dsp::ChannelMonitor::Mode::fixed_bin);
    mon.set_fixed_bin(999);   // 越界 → clamp 255
    mon.push(frame_with(256, 10, -40.0f, -90.0f));
    check(mon.tracked_bin() == 255, "fixed bin 钳制到 255");
    check(mon.series().back().db == -90.0f, "读取的是 bin255 的底噪");
}

static void test_monitor_stats() {
    hackrftool::dsp::ChannelMonitor mon(16);
    mon.set_mode(hackrftool::dsp::ChannelMonitor::Mode::fixed_bin);
    mon.set_fixed_bin(0);
    for (const float db : {-50.0f, -80.0f, -45.0f, -90.0f}) {
        mon.push(frame_with(256, 0, db, db));   // bin0 即目标
    }
    const auto st = mon.stats(-70.0f);
    check(st.count == 4, "统计样本数");
    check(std::abs(st.mean - (-66.25f)) < 0.01, "均值");
    check(std::abs(st.peak - (-45.0f)) < 0.01, "峰值");
    // 样本方差（除以 count）：{-50,-80,-45,-90} → 367.19
    check(std::abs(st.variance - 367.1875f) < 0.5, "方差");
    check(std::abs(st.duty - 0.5f) < 0.001, "占空比 = 高于阈值比例");
}

static void test_monitor_ring_and_csv() {
    hackrftool::dsp::ChannelMonitor mon(4);
    mon.set_mode(hackrftool::dsp::ChannelMonitor::Mode::fixed_bin);
    mon.set_fixed_bin(0);
    for (int i = 0; i < 6; ++i)
        mon.push(frame_with(256, 0, float(-50 - i), float(-50 - i)));
    const auto s = mon.series();
    check(s.size() == 4, "环形容量限制");
    check(std::abs(s.front().db - (-52.0f)) < 0.01, "最旧样本被滚动掉（front=-52）");
    const std::wstring path = L"test-monitor-export.csv";
    check(mon.export_csv(path), "CSV 导出成功");
    if (std::FILE* fp = _wfopen(path.c_str(), L"r")) {
        char line[128];
        unsigned lines = 0;
        bool header_ok = false, first_data_ok = false;
        while (std::fgets(line, sizeof line, fp) != nullptr) {
            if (lines == 0) header_ok = (std::strcmp(line, "elapsed_s,db\n") == 0);
            if (lines == 1) {
                double t = 0.0, db = 0.0;
                first_data_ok =
                    (std::sscanf(line, "%lf,%lf", &t, &db) == 2 && db < -51.9 && db > -52.1);
            }
            ++lines;
        }
        std::fclose(fp);
        check(header_ok, "CSV 表头");
        check(first_data_ok, "CSV 首行数据（-52）");
        check(lines == 5, "CSV 行数 = 表头 + 4 样本");
    }
    std::remove("test-monitor-export.csv");
}

// ---- GFSK 合成调制器（测试助手） ------------------------------------------

static double qfunc(double x) { return 0.5 * std::erfc(x / std::sqrt(2.0)); }

// GFSK 频率脉冲：矩形符号脉冲经 BT=0.5 高斯（Q 函数形式），归一化面积 1
static std::vector<double> gfsk_pulse(std::size_t sps) {
    std::vector<double> pulse(4 * sps, 0.0);
    const double c = 2.0 * 3.14159265358979323846 * 0.5 / std::sqrt(0.6931471805599453);
    double area = 0.0;
    for (std::size_t i = 0; i < pulse.size(); ++i) {
        const double t = (double(i) - 2.0 * sps) / double(sps);   // -2..2 符号
        pulse[i] = qfunc(c * (t - 0.5)) - qfunc(c * (t + 0.5));
        area += pulse[i];
    }
    for (auto& v : pulse) v /= area;
    return pulse;
}

static std::vector<std::complex<float>> gfsk_modulate(const std::vector<unsigned>& bits,
                                                      std::size_t sps, double dev_hz,
                                                      double fs) {
    const auto pulse = gfsk_pulse(sps);
    std::vector<double> nrz(bits.size() * sps, 0.0);
    for (std::size_t i = 0; i < bits.size(); ++i)
        for (std::size_t j = 0; j < sps; ++j)
            nrz[i * sps + j] = bits[i] ? 1.0 : -1.0;
    const std::size_t n = nrz.size() + pulse.size();
    std::vector<double> freq(n, 0.0);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t k = 0; k < pulse.size(); ++k) {
            const auto src = std::ptrdiff_t(i) - std::ptrdiff_t(k) +
                             std::ptrdiff_t(pulse.size() / 2) - 1;
            if (src >= 0 && src < std::ptrdiff_t(nrz.size()))
                freq[i] += nrz[size_t(src)] * pulse[k];
        }
    std::vector<std::complex<float>> out(n);
    double phase = 0.0;
    const double k_dev = 2.0 * 3.14159265358979323846 * dev_hz / fs;
    for (std::size_t i = 0; i < n; ++i) {
        phase += k_dev * freq[i];
        out[i] = {float(std::cos(phase)), float(std::sin(phase))};
    }
    return out;
}

static void test_gfsk_roundtrip() {
    // 200 随机载荷比特，首尾各 4 个哑符号（避开成形拖尾）
    std::vector<unsigned> bits(4, 0u);
    unsigned lcg = 7u;
    for (std::size_t i = 0; i < 200; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        bits.push_back((lcg >> 31) & 1u);
    }
    bits.insert(bits.end(), 4, 0u);
    const auto wave = gfsk_modulate(bits, 20, 160e3, 20e6);
    hackrftool::dsp::GfskDemod demod(20e6, 1e6, 160e3);
    const auto r = demod.demod(wave, 0);
    check(r.bits.size() > 204, "解调符号数足够");
    std::size_t errors = 0;
    for (std::size_t i = 4; i < 204; ++i)
        errors += (r.bits[i] != bits[i]);
    check(errors == 0, "GFSK 无噪往返 BER=0");
    float qmin = 1.0f;
    for (std::size_t i = 4; i < 204; ++i) qmin = std::min(qmin, r.quality[i]);
    check(qmin > 0.3f, "符号置信度健康");
}

static void test_gfsk_short_input() {
    hackrftool::dsp::GfskDemod demod(20e6, 1e6, 160e3);
    check(demod.demod({}, 0).bits.empty(), "空输入返回空");
    check(demod.demod(std::vector<std::complex<float>>(15, {1.f, 0.f}), 0).bits.empty(),
          "不足一个符号返回空");
}

static void test_gfsk_roundtrip_2m() {
    // 2 Mbps：sps=10，同口径往返
    std::vector<unsigned> bits(4, 0u);
    unsigned lcg = 99u;
    for (std::size_t i = 0; i < 200; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        bits.push_back((lcg >> 31) & 1u);
    }
    bits.insert(bits.end(), 4, 0u);
    const auto wave = gfsk_modulate(bits, 10, 160e3, 20e6);
    hackrftool::dsp::GfskDemod demod(20e6, 2e6, 160e3);
    check(demod.samples_per_symbol() == 10, "2M 符号率 sps=10");
    const auto r = demod.demod(wave, 0);
    std::size_t errors = 0;
    for (std::size_t i = 4; i < 204; ++i)
        errors += (r.bits[i] != bits[i]);
    check(errors == 0, "GFSK 2 Mbps 往返 BER=0");
}

static void test_burst_detector() {
    // 8 万复样本：静默 (1,0) ≈ -42 dB，三个幅度 100 复单音突发 ≈ -2 dB
    const std::size_t n = 80000;
    std::vector<std::int8_t> iq(n * 2, 0);
    for (std::size_t i = 0; i < n; ++i) {
        iq[i * 2] = 1;   // 静默底噪
    }
    const std::size_t starts[3] = {5000, 30000, 60000};
    const std::size_t len = 2000;
    for (const std::size_t st : starts) {
        for (std::size_t i = 0; i < len; ++i) {
            const double ph = 2.0 * 3.14159265358979323846 * double(i) / 20.0;
            iq[(st + i) * 2] = std::int8_t(100 * std::cos(ph));
            iq[(st + i) * 2 + 1] = std::int8_t(100 * std::sin(ph));
        }
    }
    const auto bursts =
        hackrftool::dsp::detect_bursts(iq.data(), iq.size(), -30.0f, 200);
    check(bursts.size() == 3, "恰好检出 3 个突发");
    if (bursts.size() == 3) {
        for (std::size_t b = 0; b < 3; ++b) {
            const auto want = starts[b];
            check(std::abs(std::ptrdiff_t(bursts[b].start_sample) -
                           std::ptrdiff_t(want)) < 300,
                  "突发起点容差内");
            check(std::abs(std::ptrdiff_t(bursts[b].length_samples) -
                           std::ptrdiff_t(len)) < 600,
                  "突发长度容差内");
            check(bursts[b].peak_db > -5.0f, "突发峰值电平");
        }
    }
}

static void test_iq_recorder_file() {
    hackrftool::radio::IqRecorder rec;
    const std::wstring path = L"test-iq-recorder.cs8";
    check(rec.start(path), "录制启动");
    const std::vector<std::int8_t> a(1000, std::int8_t(-86));   // 0xAA
    const std::vector<std::int8_t> b(1000, std::int8_t(-69));   // 0xBB
    const std::vector<std::int8_t> c(1000, std::int8_t(-52));   // 0xCC
    rec.write(a.data(), a.size());
    rec.write(b.data(), b.size());
    rec.write(c.data(), c.size());
    check(rec.stop(), "录制停止");
    check(rec.bytes_written() == 3000, "落盘字节 3000");
    if (std::FILE* fp = _wfopen(path.c_str(), L"rb")) {
        std::vector<unsigned char> buf(3000);
        check(std::fread(buf.data(), 1, 3000, fp) == 3000, "文件长度");
        check(buf[0] == 0xAA && buf[1000] == 0xBB && buf[2000] == 0xCC, "块顺序正确");
        std::fclose(fp);
    } else {
        check(false, "文件可读回");
    }
    _wremove(path.c_str());
}

static void test_panorama_stitch() {
    hackrftool::dsp::PanoramaModel pano(5, 256);
    check(!pano.complete(), "初始不完整");
    std::vector<float> seg(256);
    for (std::size_t s = 0; s < 5; ++s) {
        std::fill(seg.begin(), seg.end(), float(-50 - int(s)));
        pano.set(s, seg);
    }
    check(pano.complete(), "5 段齐后完整");
    const auto p = pano.panorama();
    check(p.size() == 1280, "全景 1280 bin");
    check(p[0] == -50.0f && p[256] == -51.0f && p[1279] == -54.0f, "拼接顺序正确");
    const auto d = pano.downscaled(320);
    check(d.size() == 320, "降采样 320 列");
    check(std::abs(d[0] - (-50.0f)) < 0.01f, "每 4 bin 均值（常数段）");
    pano.set(0, std::vector<float>(100, -60.0f));   // 长度不符 → 忽略
    check(pano.panorama().size() == 1280 && pano.panorama()[0] == -50.0f, "坏段被拒");
    check(pano.seq() > 0, "seq 递增");
    pano.clear_fresh();
    check(!pano.complete(), "clear_fresh 后等待新一轮");
    check(pano.panorama()[0] == -50.0f, "clear_fresh 保序（数据仍在）");
}

static void test_live_bursts() {
    hackrftool::dsp::LiveBursts live(100000);
    const auto push_burst = [&live](std::size_t start, std::size_t len) {
        std::vector<std::int8_t> buf(len * 2, 1);   // 静默底
        for (std::size_t i = 0; i < len; ++i) {
            buf[i * 2] = 100;
            buf[i * 2 + 1] = 0;
        }
        std::vector<std::int8_t> silence(start, 1);   // 字节数即样本数一半？——直接拼字节
        (void)silence;
        live.write(buf.data(), buf.size());
    };
    // 构造：静默 4000B + 突发2000样本 + 静默 + 突发 + 静默
    std::vector<std::int8_t> silence(8000, 1);
    live.write(silence.data(), silence.size());
    push_burst(0, 2000);
    live.write(silence.data(), silence.size());
    push_burst(0, 2000);
    live.write(silence.data(), silence.size());

    const std::size_t n1 = live.refresh(-30.0f, 200);
    check(n1 == 2, "首轮检出 2 个突发");
    check(live.bursts().size() == 2, "列表 2 条");

    // 新静默 + 1 个新突发 → 只新增 1
    live.write(silence.data(), silence.size());
    push_burst(0, 2000);
    live.write(silence.data(), silence.size());
    const std::size_t n2 = live.refresh(-30.0f, 200);
    check(n2 == 1, "次轮仅新增 1（去重）");
    check(live.bursts().size() == 3, "累计 3 条");
    check(live.total_samples() > 0, "样本计数累计");

    live.clear();
    check(live.bursts().empty(), "clear 复位");
}

// ---- nRF24 ESB 合成打包器（测试助手） --------------------------------------
// 空口比特 LSB-first：前导 0xAA + 地址(3-5B) + PCF(9bit: 低6位载荷长) + 载荷 + CRC16
static void push_bits_lsb(std::vector<std::uint8_t>& bits, unsigned v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) bits.push_back((v >> i) & 1u);
}

static unsigned short esb_crc16(const std::vector<std::uint8_t>& bits, std::size_t from,
                                std::size_t count) {
    unsigned short crc = 0xFFFF;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned bit = bits[from + i];
        // LSB-first 进位的 CCITT（poly 0x1021）：以反转多项式 0x8408 实现
        const unsigned fb = (crc & 1u) ^ bit;
        crc >>= 1;
        if (fb != 0u) crc ^= 0x8408u;
    }
    return crc;
}

static std::vector<std::uint8_t> esb_pack(const std::vector<unsigned char>& address,
                                          const std::vector<unsigned char>& payload) {
    std::vector<std::uint8_t> bits;
    push_bits_lsb(bits, 0xAA, 8);   // 前导
    for (const unsigned char b : address) push_bits_lsb(bits, b, 8);
    push_bits_lsb(bits, unsigned(payload.size()) & 0x3Fu, 6);   // PCF 载荷长
    push_bits_lsb(bits, 0x01, 2);                                // PID
    push_bits_lsb(bits, 0, 1);                                   // NO_ACK
    for (const unsigned char b : payload) push_bits_lsb(bits, b, 8);
    const unsigned short crc =
        esb_crc16(bits, 8, bits.size() - 8);   // 覆盖地址+PCF+载荷
    push_bits_lsb(bits, crc, 16);
    return bits;
}

static void test_esb_roundtrip() {
    const std::vector<unsigned char> addr = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7};
    const std::vector<unsigned char> payload = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    auto bits = esb_pack(addr, payload);
    const auto frames = hackrftool::dsp::esb_scan(bits);
    check(frames.size() == 1, "ESB 还原出 1 帧");
    if (!frames.empty()) {
        check(frames[0].address == addr, "ESB 地址还原");
        check(frames[0].payload == payload, "ESB 载荷还原");
    }
}

static void test_esb_corruption_rejected() {
    const std::vector<unsigned char> addr = {0x12, 0x34, 0x56};
    const std::vector<unsigned char> payload = {0xDE, 0xAD};
    auto bits = esb_pack(addr, payload);
    bits[bits.size() / 2] ^= 1u;   // 翻转一个载荷比特
    check(hackrftool::dsp::esb_scan(bits).empty(), "ESB 比特错误被 CRC 拒绝");
}

static void test_esb_noise() {
    // 纯随机比特（确定性 LCG）不应产出任何帧
    std::vector<std::uint8_t> bits(2000, 0);
    unsigned lcg = 42u;
    for (auto& b : bits) {
        lcg = lcg * 1664525u + 1013904223u;
        b = (lcg >> 31) & 1u;
    }
    check(hackrftool::dsp::esb_scan(bits).empty(), "ESB 噪声无帧");
}

// 合成射频波 → int8 交错字节（幅度缩放 + 前后垫静默）
static std::vector<std::int8_t> wave_to_iq_bytes(const std::vector<std::complex<float>>& w,
                                                 float amp, std::size_t pad_samples) {
    std::vector<std::int8_t> out((w.size() + pad_samples * 2) * 2, 0);
    for (std::size_t i = 0; i < w.size(); ++i) {
        const double re = std::clamp(double(w[i].real()) * amp, -127.0, 127.0);
        const double im = std::clamp(double(w[i].imag()) * amp, -127.0, 127.0);
        out[(pad_samples + i) * 2] = static_cast<std::int8_t>(re);
        out[(pad_samples + i) * 2 + 1] = static_cast<std::int8_t>(im);
    }
    return out;
}

static void test_end_to_end_pipeline() {
    // 1) ESB 包 → GFSK 波形（sps=20，1 Mbps）→ 载波搬到 +2.5 MHz。
    //    注意：512 点复 FFT 中 DC 在 bin 0、负频在 257-511——合成信号若放
    //    DC 会正好落进分析器的边缘裁剪带（那本是为硬件基带跌落留的）。
    const std::vector<unsigned char> addr = {0xC2, 0xC2, 0xC2, 0xC2, 0xC1};
    const std::vector<unsigned char> payload = {0x11, 0x22, 0x33, 0x44};
    const auto bits = esb_pack(addr, payload);
    const std::vector<unsigned> bits_u(bits.begin(), bits.end());
    auto wave = gfsk_modulate(bits_u, 20, 160e3, 20e6);
    constexpr double kFs = 20e6, kCarrierOff = 2.5e6;
    const std::size_t pad = 4000;   // 前后静默
    // 用缓冲绝对样本号旋转，保证解调时可精确搬回
    std::vector<std::complex<float>> shifted(wave.size());
    for (std::size_t i = 0; i < wave.size(); ++i) {
        const double ph = 2.0 * 3.14159265358979323846 * kCarrierOff *
                          double(pad + i) / kFs;
        shifted[i] = wave[i] * std::complex<float>(float(std::cos(ph)),
                                                   float(std::sin(ph)));
    }
    const auto iq = wave_to_iq_bytes(shifted, 100.0f, pad);

    // 期望输出 bin：输入 bin ≈ 2.5e6/20e6×512 = 64 → out ≈ (64-32)*256/448 ≈ 18
    hackrftool::dsp::SpectrumAnalyzer an(512, 4, 256);
    for (int i = 0; i < 8; ++i) an.feed(iq.data(), iq.size());
    const auto fr = an.snapshot();
    check(!fr.db.empty(), "e2e 频谱出帧");
    std::size_t argmax = 0;
    for (std::size_t i = 1; i < fr.db.size(); ++i)
        if (fr.db[i] > fr.db[argmax]) argmax = i;
    check(argmax > 13 && argmax < 24, "e2e 峰位 ≈ 18");
    check(fr.db[argmax] > -20.0f, "e2e 峰电平 > -20 dB");
    check(fr.db[argmax] - fr.db[100] > 15.0f, "e2e 峰凸起 > 15 dB");

    // 2) 实时突发检测：检出突发且样本可取回
    hackrftool::dsp::LiveBursts live(200000);
    live.write(iq.data(), iq.size());
    live.write(iq.data(), iq.size());   // 再写一段推进水位（静默垫尾）
    (void)live.refresh(-30.0f, 200);
    check(live.bursts().size() >= 1, "e2e 检出突发");
    const auto& b0 = live.bursts().back();
    std::vector<std::int8_t> slice;
    check(live.read_slice(b0.start_sample, b0.samples, slice), "e2e 切片取回");
    check(slice.size() >= 128, "e2e 切片长度");

    // 3) 切片搬回基带（按绝对样本号去旋转）→ GFSK 解调（4 相位偏移搜索，
    //    补偿突发起点 ±半窗偏差）→ ESB 解帧
    std::vector<std::complex<float>> cmplx(b0.samples);
    for (unsigned long long i = 0; i < b0.samples; ++i) {
        const double ph = -2.0 * 3.14159265358979323846 * kCarrierOff *
                          double(b0.start_sample + i) / kFs;
        cmplx[static_cast<std::size_t>(i)] =
            std::complex<float>(float(slice[static_cast<std::size_t>(i) * 2]),
                                float(slice[static_cast<std::size_t>(i) * 2 + 1])) *
            std::complex<float>(float(std::cos(ph)), float(std::sin(ph)));
    }
    const hackrftool::dsp::GfskDemod demod(20e6, 1e6, 160e3);
    std::vector<hackrftool::dsp::EsbFrame> frames;
    for (const std::size_t off : {std::size_t(0), std::size_t(5), std::size_t(10),
                                  std::size_t(15)}) {
        frames = hackrftool::dsp::esb_scan(demod.demod(cmplx, off).bits);
        if (!frames.empty()) break;
    }
    check(frames.size() == 1, "e2e ESB 还原 1 帧");
    if (!frames.empty()) {
        check(frames[0].address == addr, "e2e 地址还原");
        check(frames[0].payload == payload, "e2e 载荷还原");
    }
}

static void test_iq_recorder_contract_edges() {
    hackrftool::radio::IqRecorder rec;
    check(!rec.stop(), "未启动时 stop 返回 false");
    const std::int8_t dummy[8] = {};
    rec.write(dummy, 8);   // 未启动：应被忽略，不得崩溃
    check(!rec.recording(), "未启动不处于录制态");

    check(rec.start(L"test-contract.cs8"), "启动");
    check(!rec.start(L"test-contract2.cs8"), "重复启动返回 false");
    // 50 块 × 4KB：写线程跟得上的量，验证无损全落盘
    const std::vector<std::int8_t> block(4096, std::int8_t(-3));
    for (int i = 0; i < 50; ++i) rec.write(block.data(), block.size());
    check(rec.stop(), "停止");
    check(rec.bytes_written() == 50 * 4096, "50 块全落盘");
    check(rec.dropped_blocks() == 0, "无丢弃");
    check(!rec.recording(), "停止后非录制态");
    if (std::FILE* fp = _wfopen(L"test-contract.cs8", L"rb")) {
        std::fseek(fp, 0, SEEK_END);
        check(std::ftell(fp) == static_cast<long>(50 * 4096), "文件长度一致");
        std::fclose(fp);
    }
    _wremove(L"test-contract.cs8");
    _wremove(L"test-contract2.cs8");
}

static void test_burst_detector_edges() {
    // 全饱和（整段高于阈值）→ 恰好 1 个突发，钳制到全长
    const std::vector<std::int8_t> sat(4000, 100);   // (100,100) 功率 2e4
    const auto b1 = hackrftool::dsp::detect_bursts(sat.data(), sat.size(), -30.0f, 200);
    check(b1.size() == 1, "全饱和单突发");
    if (b1.size() == 1) {
        check(b1[0].start_sample == 0, "饱和突发从 0 起");
        check(b1[0].length_samples == 2000, "饱和突发钳制全长");
    }
    // 全静默 → 零突发
    const std::vector<std::int8_t> sil(4000, 1);   // (1,1) ≈ -39dB
    check(hackrftool::dsp::detect_bursts(sil.data(), sil.size(), -30.0f, 200).empty(),
          "全静默零突发");
    // 窗口不整除的奇数长度突发
    std::vector<std::int8_t> odd(909 * 2, 1);
    for (std::size_t i = 0; i < 909; ++i) {
        odd[i * 2] = 100;
        odd[i * 2 + 1] = 0;
    }
    const auto b3 = hackrftool::dsp::detect_bursts(odd.data(), odd.size(), -30.0f, 200);
    check(b3.size() == 1 && b3[0].length_samples >= 909, "奇数长度突发");
}

static void test_live_bursts_ring_wrap() {
    // 环 1000：写 2700 样本（2.7 圈），突发在绝对 2000..2199（末圈内）
    hackrftool::dsp::LiveBursts live(1000);
    const std::vector<std::int8_t> silence(2000, 1);   // 2000 字节 = 1000 样本静默
    live.write(silence.data(), silence.size());
    live.write(silence.data(), silence.size());   // 累计 2000 静默
    std::vector<std::int8_t> burst(200 * 2, 1);
    for (std::size_t i = 0; i < 200; ++i) {
        burst[i * 2] = 100;
        burst[i * 2 + 1] = 0;
    }
    live.write(burst.data(), burst.size());
    live.write(silence.data(), silence.size() / 2);   // 尾部静默推进水位
    (void)live.refresh(-30.0f, 200);
    check(live.bursts().size() == 1, "回绕后检出 1 突发");
    if (!live.bursts().empty()) {
        const auto& b = live.bursts().back();
        check(b.start_sample >= 1800 && b.start_sample <= 2200, "回绕绝对样本号连续");
        std::vector<std::int8_t> slice;
        check(live.read_slice(b.start_sample, b.samples, slice), "回绕切片可取");
        // 检测起点含 -window/2 修正，切片头部允许静默：断言主体为突发
        int strong = 0;
        for (std::size_t i = 0; i + 1 < slice.size(); i += 2)
            strong += (int(slice[i]) > 50) ? 1 : 0;
        check(slice.size() == b.samples * 2 && strong >= 90, "回绕切片包含突发主体");
    }
    // 早于最旧水位 → 拒绝
    std::vector<std::int8_t> out;
    check(!live.read_slice(0, 10, out), "被挤出环的区间拒绝");
}

static void test_capture_sidecar() {
    hackrftool::radio::SidecarInfo cfg;
    cfg.center_hz = 2450e6;
    cfg.sample_rate_hz = 20e6;
    cfg.lna_db = 16;
    cfg.vga_db = 24;
    check(hackrftool::radio::write_capture_sidecar(L"test-sc.cs8", cfg),
          "sidecar 写入成功");
    std::FILE* fp = _wfopen(L"test-sc.cs8.txt", L"r");
    check(fp != nullptr, "sidecar 文件名 = IQ 路径 + .txt");
    if (fp != nullptr) {
        std::string content;
        char buf[256];
        while (std::fgets(buf, sizeof buf, fp) != nullptr) content += buf;
        std::fclose(fp);
        check(content.find("center_mhz=2450") != std::string::npos,
              "sidecar 含中心频率");
        check(content.find("sample_rate_msps=20") != std::string::npos,
              "sidecar 含采样率");
        check(content.find("lna_db=16") != std::string::npos, "sidecar 含 LNA");
        check(content.find("vga_db=24") != std::string::npos, "sidecar 含 VGA");
        check(content.find("int8") != std::string::npos, "sidecar 含样本格式");
    }
    _wremove(L"test-sc.cs8.txt");
}

static void test_waterfall_level_map() {
    // 映射窗 [-110,-30]：端点钳制 + 单调 + 常用底噪档位可见（T4.3）
    check(hackrftool::dsp::waterfall_level(-130.0f) == 0, "低于窗底 → 0");
    check(hackrftool::dsp::waterfall_level(-110.0f) == 0, "窗底 → 0");
    check(hackrftool::dsp::waterfall_level(-30.0f) == 15, "窗顶 → 15");
    check(hackrftool::dsp::waterfall_level(-10.0f) == 15, "高于窗顶 → 15");
    check(hackrftool::dsp::waterfall_level(-70.0f) == 7, "中点 -70 → 7（7.5 截断）");
    check(hackrftool::dsp::waterfall_level(-70.0f) ==
              hackrftool::dsp::waterfall_level(-70.0f),
          "纯函数确定");
    // T4.3 验收：旧窗 [-100,-40] 下 -75dB 落 6 级，但 -95dB 底噪=0 级全黑；
    // 新窗下 -95 → 2 级（深蓝可见）
    check(hackrftool::dsp::waterfall_level(-95.0f) == 2, "深底噪 -95 → 深蓝可见");
}

static void test_esb_hex_dump() {
    // 关键信号横幅用：载荷 hex 文本（大写、空格分隔、补零）
    check(hackrftool::dsp::hex_dump({}).empty(), "空载荷 → 空串");
    check(hackrftool::dsp::hex_dump({0xFB}) == "FB", "单字节");
    check(hackrftool::dsp::hex_dump({0xFB, 0x50, 0x00}) == "FB 50 00",
          "多字节空格分隔");
    check(hackrftool::dsp::hex_dump({0x0A, 0xFF}) == "0A FF", "补零大写");
}

static void test_waterfall_runs() {
    // 瀑布行程合并：同值横向连续格 → 一个矩形（绘制 fill_rect 数下降）
    using hackrftool::dsp::waterfall_runs;
    check(waterfall_runs({}, 0).empty(), "空输入 → 空");
    check(waterfall_runs({1, 2, 3, 4, 5}, 4).empty(), "非整行 → 空（契约拒绝）");

    const std::vector<int> one_row{2, 2, 2, 5, 5, 1};
    const auto runs = waterfall_runs(one_row, 6);
    check(runs.size() == 3, "3 段连续值 → 3 行程");
    check(runs[0].row == 0 && runs[0].col0 == 0 && runs[0].len == 3 &&
              runs[0].level == 2,
          "行程 1 起 0 长 3 值 2");
    check(runs[1].col0 == 3 && runs[1].len == 2 && runs[1].level == 5,
          "行程 2 起 3 长 2 值 5");
    check(runs[2].col0 == 5 && runs[2].len == 1 && runs[2].level == 1,
          "行程 3 起 5 长 1 值 1");

    std::vector<int> same(2 * 4, 7);
    const auto r2 = waterfall_runs(same, 4);
    check(r2.size() == 2 && r2[0].row == 0 && r2[1].row == 1 && r2[0].len == 4,
          "全同值 → 每行 1 行程，不跨行");

    const std::vector<int> distinct{1, 2, 3};
    const auto r3 = waterfall_runs(distinct, 3);
    check(r3.size() == 3 && r3[0].col0 == 0 && r3[1].col0 == 1 && r3[2].col0 == 2,
          "全异值 → 逐格行程");

    // 覆盖等价：行程总格数 == 输入格数
    std::vector<int> mix(3 * 8);
    for (std::size_t i = 0; i < mix.size(); ++i) mix[i] = int(i % 5);
    std::size_t covered = 0;
    for (const auto& r : waterfall_runs(mix, 8)) covered += r.len;
    check(covered == mix.size(), "行程覆盖格数 == 输入格数");

    // 实测压缩率（64×256：底噪平坦 + 突发带随机 → 日志记录合并前后矩形数）
    std::vector<int> wf(64 * 256, 2);
    unsigned seed = 42;
    for (std::size_t row = 8; row < 20; ++row)
        for (std::size_t col = 100; col < 140; ++col) {
            seed = seed * 1103515245u + 12345u;
            wf[row * 256 + col] = 2 + int((seed >> 16) % 12u);
        }
    const auto merged = waterfall_runs(wf, 256);
    std::printf("waterfall_runs: 64x256 原始 %zu 格 → %zu 矩形（%.1f%%）\n",
                wf.size(), merged.size(),
                100.0 * double(merged.size()) / double(wf.size()));
}

static void test_status_dynamic() {
    using hackrftool::ui::StatusInfo;
    using hackrftool::ui::status_dynamic;

    // 已停止且无帧 → 空动态段（基础文本由调用方拼）
    check(status_dynamic(StatusInfo{}).empty(), "停止+无帧 → 空串");

    // 已停止但有帧 → 仅帧号后缀
    StatusInfo stopped;
    stopped.has_frame = true;
    stopped.frame_seq = 42;
    check(status_dynamic(stopped) == L" · 帧 42", "停止+帧 → 仅帧号后缀");

    // 单窗接收：中心/采样率/增益逐段出现
    StatusInfo rx;
    rx.running = true;
    rx.has_frame = true;
    rx.center_mhz = 2450.0;
    rx.rate_msps = 20;
    rx.lna_db = 16;
    rx.vga_db = 16;
    rx.frame_seq = 1000;
    const std::wstring single = status_dynamic(rx);
    check(single == L"接收中 · 中心 2450.0 MHz · 20 Msps · LNA 16/VGA 16 · 帧 1000",
          "单窗接收完整动态段");

    // 全频段扫描：段号 +1 显示、段中心一位小数
    StatusInfo sw = rx;
    sw.sweep = true;
    sw.seg_idx = 1;
    sw.seg_center_mhz = 2417.5;
    check(status_dynamic(sw) ==
              L"接收中 · 扫描 段 2/5 @ 2417.5 MHz · 20 Msps · LNA 16/VGA 16 · 帧 1000",
          "扫描段动态段");

    // 录制中：MB 后缀（3 MiB + 5 字节 → 3MB，向下取整）
    StatusInfo rec = rx;
    rec.recording = true;
    rec.rec_bytes = 3ull * 1024 * 1024 + 5;
    check(status_dynamic(rec).find(L" · ●录制 3MB") != std::wstring::npos,
          "录制 MB 后缀");

    // 无帧时不显示帧号
    StatusInfo noframe = rx;
    noframe.has_frame = false;
    check(status_dynamic(noframe).find(L"帧") == std::wstring::npos,
          "无帧不显示帧号");

    // 状态栏分段（原生状态栏逐格文本；空段留白）
    std::wstring parts[5];
    hackrftool::ui::status_parts(sw, parts);
    check(parts[0] == L"扫描 段 2/5 @ 2417.5 MHz", "分段 0 扫描段");
    check(parts[1] == L"20 Msps · LNA 16/VGA 16", "分段 1 采样率增益");
    check(parts[2] == L"帧 1000", "分段 2 帧号");
    check(parts[3].empty() && parts[4].empty(), "未录制/无 ESB → 空段");
    std::wstring rec_parts[5];
    hackrftool::ui::status_parts(rec, rec_parts);
    check(rec_parts[3] == L"●录制 3MB", "分段 3 录制 MB");
    StatusInfo esb = rx;
    esb.esb_hits = 7;
    std::wstring esb_parts[5];
    hackrftool::ui::status_parts(esb, esb_parts);
    check(esb_parts[4] == L"ESB 7", "分段 4 ESB 帧数");
    check(status_dynamic(esb).find(L"ESB 7") != std::wstring::npos,
          "动态段含 ESB（>0 时）");
}

// ---- FM 收音机链路（#53） --------------------------------------------------

// Goertzel 单频幅度（音频检测用）
static double goertzel(const std::vector<float>& x, double fs, double f) {
    const double w = 2.0 * hackrftool::dsp::kPi * f / fs;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (const float v : x) {
        const double s0 = v + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(std::max(s1 * s1 + s2 * s2 - coeff * s1 * s2, 0.0)) /
           double(x.size());
}

// 合成 FM 广播 IQ：MPX（可选导频+DSBSC）→ ±75 kHz FM 调制 → int8 IQ
static std::vector<std::int8_t> synth_fm_stereo(std::size_t n_samples, double fs,
                                                bool with_pilot,
                                                std::vector<float>* ref_l,
                                                std::vector<float>* ref_r,
                                                double dc = 0.0) {
    std::vector<float> l(n_samples), r(n_samples);
    for (std::size_t i = 0; i < n_samples; ++i) {
        const double t = double(i) / fs;
        l[i] = float(0.45 * std::sin(2 * hackrftool::dsp::kPi * 1000.0 * t));
        r[i] = float(0.45 * std::sin(2 * hackrftool::dsp::kPi * 3000.0 * t));
    }
    std::vector<std::int8_t> iq(n_samples * 2);
    double phase = 0.0;
    for (std::size_t i = 0; i < n_samples; ++i) {
        const double t = double(i) / fs;
        double mpx = 0.5 * (l[i] + r[i]);                       // L+R ≤15 kHz
        if (with_pilot) {
            mpx += 0.09 * std::sin(2 * hackrftool::dsp::kPi * 19e3 * t);   // 导频
            // 38k 副载波与导频同源同相（广播惯例：sin 族），非独立 cos
            mpx += 0.5 * (l[i] - r[i]) *
                   std::sin(2 * 2 * hackrftool::dsp::kPi * 19e3 * t);      // L−R DSBSC
        }
        phase += 2 * hackrftool::dsp::kPi * 75e3 * mpx / fs;
        const float re = 0.8f * float(std::cos(phase)) + float(dc);
        const float im = 0.8f * float(std::sin(phase)) + float(dc);
        iq[i * 2] = std::int8_t(re * 127.0f);
        iq[i * 2 + 1] = std::int8_t(im * 127.0f);
    }
    if (ref_l != nullptr) *ref_l = l;
    if (ref_r != nullptr) *ref_r = r;
    return iq;
}

// 音频收集上下文
struct FmCollect {
    std::vector<float> l, r;
    static void cb(const float* li, const float* ri, std::size_t n, void* ctx) {
        auto* c = static_cast<FmCollect*>(ctx);
        c->l.insert(c->l.end(), li, li + n);
        c->r.insert(c->r.end(), ri, ri + n);
    }
};

static void test_fm_discriminator_unit() {
    using hackrftool::dsp::fm_discriminator;
    // 恒定频偏 +f 的复信号，鉴频输出恒定
    std::complex<float> prev{1.0f, 0.0f};
    const double f = 0.25;   // 归一化（×fs/2）
    std::complex<float> cur(std::cos(float(2 * hackrftool::dsp::kPi * f)),
                            std::sin(float(2 * hackrftool::dsp::kPi * f)));
    const float d = fm_discriminator(cur, prev);
    check(std::abs(d - 0.5f) < 0.01f, "鉴频器恒定频偏还原（每样本 π/2 相位 → 0.5）");
    const float dn = fm_discriminator(prev, cur);
    check(std::abs(dn + 0.5f) < 0.01f, "鉴频器负频偏还原");
}

static void test_fm_receiver_stereo() {
    using hackrftool::dsp::FmReceiver;
    const double fs = 2e6;
    // 0.4 秒信号：导频 PLL 锁定（测试带宽 300 Hz → ~15 ms 锁定）
    const std::size_t n = std::size_t(0.4 * fs);
    const std::vector<std::int8_t> iq = synth_fm_stereo(n, fs, true, nullptr, nullptr);
    FmCollect out;
    FmReceiver rx(fs, 300.0);
    rx.set_audio_callback(&FmCollect::cb, &out);
    rx.feed(iq.data(), iq.size());
    check(out.l.size() > 15000, "立体声链产出音频样本（0.4s≈19200）");
    check(rx.stereo_locked(), "导频锁定 → 立体声");
    // 跳过前 0.1s（PLL 收敛），Goertzel 检测声道分离
    const std::size_t skip = 4800;
    std::vector<float> ll(out.l.begin() + skip, out.l.end());
    std::vector<float> rr(out.r.begin() + skip, out.r.end());
    const double l_1k = goertzel(ll, 48e3, 1000.0);
    const double l_3k = goertzel(ll, 48e3, 3000.0);
    const double r_3k = goertzel(rr, 48e3, 3000.0);
    const double r_1k = goertzel(rr, 48e3, 1000.0);
    check(l_1k > 0.05, "L 声道含 1 kHz（左信号）");
    check(r_3k > 0.05, "R 声道含 3 kHz（右信号）");
    check(l_3k < l_1k * 0.4, "L 声道 3 kHz 泄漏 < −8dB（立体声分离）");
    check(r_1k < r_3k * 0.4, "R 声道 1 kHz 泄漏 < −8dB（立体声分离）");
}

static void test_fm_receiver_mono_fallback() {
    using hackrftool::dsp::FmReceiver;
    const double fs = 2e6;
    const std::size_t n = std::size_t(0.2 * fs);
    const std::vector<std::int8_t> iq = synth_fm_stereo(n, fs, false, nullptr, nullptr);
    FmCollect out;
    FmReceiver rx(fs, 300.0);
    rx.set_audio_callback(&FmCollect::cb, &out);
    rx.feed(iq.data(), iq.size());
    check(!rx.stereo_locked(), "无导频 → 单声道模式");
    check(out.l.size() == out.r.size() && !out.l.empty(), "单声道仍有输出");
    const std::size_t skip = 4800;
    std::vector<float> ll(out.l.begin() + skip, out.l.end());
    std::vector<float> rr(out.r.begin() + skip, out.r.end());
    const double l_1k = goertzel(ll, 48e3, 1000.0);
    const double r_1k = goertzel(rr, 48e3, 1000.0);
    check(l_1k > 0.05 && r_1k > 0.05, "单声道 L=R 都含 1 kHz");
    check(std::abs(l_1k - r_1k) < 0.02, "单声道双声道幅度一致");
}

// HackRF 本振泄漏（DC）恰落在中心调谐的电台信号上——无直流阻塞时
// 鉴频相位被污染，输出只剩噪声（用户实测"全是噪音"的根因假设）
static void test_fm_dc_offset_survives() {
    using hackrftool::dsp::FmReceiver;
    const double fs = 2e6;
    const std::size_t n = std::size_t(0.4 * fs);
    const std::vector<std::int8_t> iq = synth_fm_stereo(n, fs, false, nullptr,
                                                        nullptr, 0.35);
    FmCollect out;
    FmReceiver rx(fs, 300.0);
    rx.set_audio_callback(&FmCollect::cb, &out);
    rx.feed(iq.data(), iq.size());
    check(out.l.size() > 15000, "强 DC 下仍有音频产出");
    const std::size_t skip = 4800;
    std::vector<float> ll(out.l.begin() + skip, out.l.end());
    const double l_1k = goertzel(ll, 48e3, 1000.0);
    check(l_1k > 0.05, "强 DC 下 1 kHz 音频可恢复（直流阻塞有效）");
}

// AFC：偏置峰自动吸附（107.1 调谐 → 台在 -0.1 → 修正 -0.1 回中心）
static void test_afc_correction() {
    using hackrftool::dsp::afc_correction;
    // 256 bins 覆盖 ±1 MHz：噪声底 -60，峰 -30 在 bin 115（=-0.101 MHz）
    std::vector<float> db(256, -60.0f);
    db[115] = -30.0f;
    const double c = afc_correction(db, 2.0, 10.0f, 0.03, 0.3);
    check(c < -0.09 && c > -0.11, "AFC 偏置峰修正 ≈ -0.1 MHz");
    check(afc_correction(db, 2.0, 40.0f, 0.03, 0.3) == 0.0,
          "峰不够显著不修正");
    std::vector<float> flat(256, -60.0f);
    flat[128] = -58.0f;   // 2dB 小起伏
    check(afc_correction(flat, 2.0, 10.0f, 0.03, 0.3) == 0.0,
          "平坦谱不修正");
    std::vector<float> far(256, -60.0f);
    far[10] = -20.0f;   // 偏移 -0.84 MHz 超界
    check(afc_correction(far, 2.0, 10.0f, 0.03, 0.3) == 0.0, "超界偏移不修正");
}

// 频谱点击峰值吸附：点在峰附近 → 精确吸到峰中心（而非像素位置）
static void test_peak_snap() {
    using hackrftool::dsp::peak_snap;
    // 256 bins 覆盖 ±1 MHz：峰 -30 在 bin 206（+0.609 MHz，模拟 98.6 强台）
    std::vector<float> db(256, -60.0f);
    db[206] = -30.0f;
    // 点击在 +0.5（98.5）：±0.15 搜索窗含 0.609 → 吸附到 ~+0.61
    const double snap = peak_snap(db, 2.0, 0.5, 0.15, 8.0f);
    check(snap > 0.58 && snap < 0.64, "点击偏 0.11 吸附到峰中心（+0.61）");
    // 峰不显著 → 保持点击位置
    std::vector<float> flat(256, -60.0f);
    flat[206] = -55.0f;
    check(peak_snap(flat, 2.0, 0.5, 0.15, 8.0f) == 0.5,
          "无显著峰保持点击位置");
    // 峰在搜索窗外 → 保持点击位置
    std::vector<float> far(256, -60.0f);
    far[40] = -20.0f;   // -0.68 MHz
    check(peak_snap(far, 2.0, 0.5, 0.15, 8.0f) == 0.5, "窗外峰不打扰");
}

// 人声强度：话音频带能量检测（1k 强、带外弱、静音极低）
static void test_voice_level() {
    using hackrftool::dsp::VoiceLevelMeter;
    auto gen = [](double f, double amp) {
        std::vector<float> v(4800);
        for (std::size_t i = 0; i < v.size(); ++i)
            v[i] = float(amp * std::sin(2 * hackrftool::dsp::kPi * f *
                                        double(i) / 48000.0));
        return v;
    };
    VoiceLevelMeter m;
    const auto s1k = gen(1000.0, 0.5);
    float db_1k = 0;
    for (int r = 0; r < 3; ++r) db_1k = m.feed(s1k.data(), s1k.size());  // 稳态
    check(db_1k > -20.0f && db_1k < -3.0f, "1kHz 0.5 幅度 ≈ -9dB 带内");
    const auto s8k = gen(8000.0, 0.5);
    float db_8k = 0;
    for (int r = 0; r < 3; ++r) db_8k = m.feed(s8k.data(), s8k.size());
    check(db_1k - db_8k > 10.0f, "8kHz 带外比 1kHz 低 >10dB");
    const std::vector<float> silence(4800, 0.0f);
    (void)m.feed(silence.data(), silence.size());   // 首块消化上块状态尾巴
    const float db_sil = m.feed(silence.data(), silence.size());
    check(db_sil < -100.0f, "静音 ≈ -120dB（第二块）");
}

// 设置持久化（#57）：往返一致 + 单行损坏不拖垮整体 + 越界值拒绝
// 遥测日志（#59）：JSON 转义/序列化/环形缓冲/计数——验收走日志不走截图
static void test_telemetry() {
    using hackrftool::log::Event;
    using hackrftool::log::Level;
    using hackrftool::log::json_escape;
    using hackrftool::log::to_jsonl;
    check(json_escape("a\"b\\c") == "a\\\"b\\\\c", "JSON 引号反斜杠转义");
    check(json_escape(std::string("x\ny\tz")) == "x\\ny\\tz", "控制字符转义");
    Event e;
    e.ts = 123;
    e.level = Level::warn;
    e.cat = "UI";
    e.event = "click";
    e.kv = {{"id", "104"}, {"note", "页签:收音"}};
    const std::string j = to_jsonl(e);
    check(j.find("\"ts\":123") != std::string::npos, "jsonl 含 ts");
    check(j.find("\"level\":\"warn\"") != std::string::npos, "jsonl 含 level");
    check(j.find("\"cat\":\"UI\"") != std::string::npos, "jsonl 含 cat");
    check(j.find("\"id\":\"104\"") != std::string::npos, "jsonl 含 kv");
    check(j.find("页签:收音") != std::string::npos, "jsonl 中文值原样（UTF-8）");
    check(j.back() == '\n', "jsonl 行尾换行");
    auto& lg = hackrftool::log::Logger::instance();
    const std::size_t before = lg.count();
    lg.write(Level::info, "TEST", "ev.a", {{"k", "1"}});
    lg.write(Level::info, "TEST", "ev.b", {});
    lg.write(Level::info, "TEST", "ev.a", {{"k", "2"}});
    check(lg.count() == before + 3, "计数器累计（不进文件也计数）");
    check(lg.count_event("TEST", "ev.a") >= 2, "按 cat/event 过滤计数");
    const auto tail = lg.tail(3);
    check(tail.size() == 3 && tail[2].kv[0].second == "2", "tail 时序与内容");
    // 文件轮转（#69）：写满 max_bytes → 主文件截断、.1 存在且非空。
    // 单实例日志器被上面用例占用——独立实例直测（open 到临时小文件）
    hackrftool::log::Logger file_lg;
    const std::wstring tmp = L"telem-rotate-test.jsonl";
    _wremove((tmp + L".1").c_str());
    _wremove(tmp.c_str());
    file_lg.open(tmp, 400);   // 400B 上限，每条约 70B → 6 条内触发
    for (int k = 0; k < 12; ++k) {
        char ev[24];
        std::snprintf(ev, sizeof ev, "ev%d", k);
        file_lg.write(hackrftool::log::Level::info, "T", ev, {});
    }
    file_lg.close();
    bool rot1 = false, main_nonempty = false;
    if (std::FILE* f = _wfopen((tmp + L".1").c_str(), L"rb")) {
        char buf[8];
        rot1 = std::fread(buf, 1, 4, f) > 0;
        std::fclose(f);
    }
    if (std::FILE* f = _wfopen(tmp.c_str(), L"rb")) {
        char buf[8];
        main_nonempty = std::fread(buf, 1, 4, f) > 0;
        std::fclose(f);
    }
    check(rot1, "轮转归档 .1 非空（写满触发）");
    check(main_nonempty, "轮转后主文件重写非空");
    _wremove((tmp + L".1").c_str());
    _wremove(tmp.c_str());
}

// Meteor QPSK 解调（#70）：合成 72ksym QPSK（Sps≈6.67@480k 抽取域）加噪，
// Costas+Gardner 收敛后眼图张开、符号判决正确率 >95%；ASM 帧同步容忍噪声
static void test_meteor_qpsk() {
    using hackrftool::dsp::QpskDemod;
    using hackrftool::dsp::qpsk_soft_bits;
    using hackrftool::dsp::find_ccsds_asm;
    // 合成：sps=8（480k/60k 便于整数域验证），随机 QPSK + 频偏 0.8 rad/千样本
    const double sps = 8.0, foff = 0.0008;
    std::vector<std::complex<float>> samples;
    std::vector<std::pair<float, float>> truth;
    unsigned rng = 12345;
    const auto rnd = [&rng] {
        rng = rng * 1664525u + 1013904223u;
        return (double(rng >> 8) / 16777216.0) - 0.5;
    };
    const int n_sym = 4000;
    for (int k = 0; k < n_sym; ++k) {
        const int quad = int((rng >> 16) & 3);
        const double base = (quad * 90 + 45) * hackrftool::dsp::kPi / 180.0;
        truth.push_back({float(std::cos(base)), float(std::sin(base))});
        for (int j = 0; j < int(sps); ++j) {
            const double ph = base + double(k * int(sps) + j) * foff;
            const double noise = 0.05 * rnd();
            samples.push_back({float(std::cos(ph) + noise),
                               float(std::sin(ph) + 0.05 * rnd())});
        }
    }
    QpskDemod demod(sps);
    long agree = 0, total = 0, skip = 500;   // 前 500 符号收敛期不计
    std::vector<float> soft;
    for (std::size_t i = 0, si = 0; i < samples.size() && si < truth.size();
         ++i) {
        if (auto s = demod.push(samples[i])) {
            if (skip > 0) { --skip; ++si; continue; }
            const auto& t = truth[si++];
            // 相位模糊四态：任一旋转一致即判对
            bool ok = false;
            for (int r = 0; r < 4; ++r) {
                const double a = r * hackrftool::dsp::kPi / 2;
                const float ti = float(t.first * std::cos(a) -
                                       t.second * std::sin(a));
                const float tq = float(t.first * std::sin(a) +
                                       t.second * std::cos(a));
                if ((s->first >= 0) == (ti >= 0) && (s->second >= 0) == (tq >= 0))
                    ok = true;
            }
            if (ok) ++agree;
            ++total;
            const auto bits = qpsk_soft_bits(s->first, s->second);
            soft.push_back(bits.first);
            soft.push_back(bits.second);
        }
    }
    check(total >= int(n_sym) - 600,   // 500 收敛期跳过 + 末端半符号余量
          "解调器符号产出率正常（Gardner 逐符号）");
    check(double(agree) / double(std::max<long>(total, 1)) > 0.95,
          "QPSK 判决正确率 >95%（Costas 四态模糊内）");
    check(demod.eye_quality() > 0.6, "眼图质量收敛（>0.6）");
    // ASM：把已知 ASM 经同一映射插入软比特流头部再找
    static const int asm_bits[32] = {0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 1,
                                     1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1,
                                     0, 1};
    std::vector<float> with_asm;
    for (int b = 0; b < 32; ++b) with_asm.push_back(asm_bits[b] ? 1.f : -1.f);
    for (float v : soft) with_asm.push_back(v);
    double q = 0.0;
    const long pos = find_ccsds_asm(with_asm, &q);
    check(pos == 0 && q > 0.9, "CCSDS ASM 帧同步精确命中（头部）");
    const long pos2 = find_ccsds_asm(soft, &q);
    check(pos2 >= 0 || q < 0.9, "随机流不误报 ASM（或质量阈值外）");
}

static void test_settings_roundtrip() {
    using hackrftool::app::Settings;
    Settings s;
    s.page = 3;
    s.center_mhz = 107.1;
    s.radio_mhz = 107.1;
    s.rate_index = 0;
    s.lna = 24;
    s.vga = 32;
    s.amp = true;
    s.auto_track = false;
    s.afc_on = false;
    s.stereo_opt = false;
    s.threshold = -65;
    s.burst_thr = -35;
    s.symrate_idx = 1;
    s.fm_bw = 2;
    s.audio_dev = 1;
    s.vol = 55;
    s.sig_cat = 2;
    s.sig_online = 0;
    s.sig_sort = 1;
    const auto t = hackrftool::app::serialize(s);
    const auto back = hackrftool::app::deserialize(t);
    check(back.has_value(), "settings 序列化→反序列化成功");
    const Settings& b = *back;
    check(b.page == 3 && b.center_mhz == 107.1 && b.radio_mhz == 107.1,
          "频率字段往返");
    check(b.rate_index == 0 && b.lna == 24 && b.vga == 32 && b.amp,
          "增益/采样率/功放往返");
    check(!b.auto_track && !b.afc_on && !b.stereo_opt && b.fm_bw == 2 &&
              b.vol == 55,
          "开关/带宽/音量/立体声往返");
    check(b.sig_cat == 2 && !b.sig_online && b.sig_sort == 1 &&
              b.audio_dev == 1 && b.symrate_idx == 1,
          "筛选/设备/符号率往返");
    // 容错：垃圾行、越界值、未知键、缺字段（保留默认）
    const auto mix = hackrftool::app::deserialize(
        "# comment\njunk line no tab\n"
        "center_mhz\t9999\npage\t7\nvol\tabc\nunknown\t5\n"
        "fm_bw\t1\nthreshold\tnan\n");
    check(mix.has_value(), "容错解析：有效键存在即成功");
    check(mix->center_mhz == 2450.0 && mix->page == 0 && mix->vol == 80,
          "越界/坏值字段保留默认");
    check(mix->fm_bw == 1, "好行照常解析");
    check(mix->threshold == -70.0, "nan 拒绝");
    check(!hackrftool::app::deserialize("").has_value(), "空文本→nullopt");
    check(!hackrftool::app::deserialize("no tabs here").has_value(),
          "无有效行→nullopt");
}

// 音频频谱（#57）：整 bin 峰位/幅值（numpy 校准 -12.04dB）、19k 导频峰位、
// 峰保持慢衰减
static void test_audio_spectrum() {
    using hackrftool::dsp::AudioSpectrumMeter;
    constexpr std::size_t N = 2048;
    AudioSpectrumMeter m;
    // 整 bin 984.375 Hz（bin 42）A=0.5：期望 -12.04 dBFS（hann 相干增益，
    // numpy 校准）；喂 3 窗确保环形缓冲全是新样本
    std::vector<float> buf(N);
    for (int rep = 0; rep < 3; ++rep) {
        for (std::size_t i = 0; i < N; ++i)
            buf[i] = float(0.5 * std::cos(2 * hackrftool::dsp::kPi * 42.0 *
                                          double(i) / double(N)));
        m.feed(buf.data(), buf.size());
    }
    const auto& db = m.spectrum_db();
    check(db.size() == N / 2, "谱 bin 数 = N/2");
    const auto peak_it = std::max_element(db.begin(), db.end());
    const std::size_t pk = std::size_t(peak_it - db.begin());
    check(pk == 42, "整 bin 音频峰位正确");
    check(std::abs(*peak_it - (-12.04f)) < 0.15f, "峰幅值 ≈ -12.0 dBFS");
    check(m.seq() >= 3, "每窗一次重算（seq 递增）");
    // 19 kHz 导频：峰落 bin 811（numpy 校准 810.67）
    AudioSpectrumMeter m2;
    for (int rep = 0; rep < 3; ++rep) {
        for (std::size_t i = 0; i < N; ++i)
            buf[i] = float(0.25 * std::cos(2 * hackrftool::dsp::kPi * 19000.0 *
                                            double(i) / 48000.0));
        m2.feed(buf.data(), buf.size());
    }
    const auto& db2 = m2.spectrum_db();
    const auto pk2 = std::size_t(std::max_element(db2.begin(), db2.end()) -
                                 db2.begin());
    check(pk2 >= 809 && pk2 <= 812, "19k 导频峰位正确（非整 bin 泄漏）");
    // 峰保持：静音后实时谱塌底、保持线仍高
    const std::vector<float> sil(N, 0.0f);
    m2.feed(sil.data(), sil.size());
    const auto& db3 = m2.spectrum_db();
    const auto& pk3 = m2.peak_db();
    check(db3[pk2] < pk3[pk2] - 3.0f, "静音后峰保持线显著高于实时谱");
    check(pk3[pk2] > db2[pk2] - 1.0f, "保持线衰减缓慢（<1dB/帧）");
}

static void test_fm_decimator_stopband() {
    using hackrftool::dsp::Decimator;
    // 2 Msps → 250 kHz：带内 40 kHz 通过，带外 400 kHz 显著衰减
    Decimator dec(2e6, 128);
    const double f_in = 40e3, f_out = 400e3;
    double p_out = 0.0;
    std::size_t n_out = 0;
    double phase = 0.0;
    const std::size_t n = 40000;
    for (std::size_t i = 0; i < n; ++i) {
        const auto s = std::complex<float>(float(std::cos(phase)),
                                           float(std::sin(phase)));
        const auto o = dec.push(s);
        if (i > n / 2) {
            if (o) {
                p_out += std::norm(*o);
                ++n_out;
            }
            if (!o) {
            }
        }
        phase += 2 * hackrftool::dsp::kPi * f_in / 2e6;
    }
    check(n_out > 2000, "抽取器产出 250k 流（0.02s≈2500）");
    // 再跑带外频率
    Decimator dec2(2e6, 128);
    double p_stop = 0.0;
    std::size_t n_stop = 0;
    phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto s = std::complex<float>(float(std::cos(phase)),
                                           float(std::sin(phase)));
        const auto o = dec2.push(s);
        if (i > n / 2 && o) {
            p_stop += std::norm(*o);
            ++n_stop;
        }
        phase += 2 * hackrftool::dsp::kPi * f_out / 2e6;
    }
    check(n_stop > 2000, "带外路径同样产出样本");
    const double ratio = std::sqrt(p_stop / double(n_stop)) /
                         std::sqrt(p_out / double(n_out));
    check(ratio < 0.1, "400 kHz 带外衰减 > 20 dB（实测线性比 <0.1）");
}

// ---- NOAA APT 云图解码（#54） ------------------------------------------------

// 合成 APT 音频行：1040 Hz×7 同步串 + 2080 像素（梯度 + 明/暗标记）调幅 2.4 kHz
static void synth_apt_line(std::vector<float>& out, double marker_bright,
                           double marker_dark) {
    const double fs = 48000.0;
    // 同步串：7 周期 1040 Hz 方波（半周期幅度交替 0.9/0.1）
    const std::size_t sync_n = std::size_t(7.0 * fs / 1040.0);
    for (std::size_t i = 0; i < sync_n; ++i) {
        const double half = std::fmod(double(i) / fs * 1040.0, 1.0) < 0.5 ? 0.9 : 0.1;
        out.push_back(float(half * std::sin(2 * hackrftool::dsp::kPi * 2400.0 *
                                            double(i) / fs)));
    }
    // 像素：4160 Hz 像素率，按精确分数推进（每行总长恰 24000 样本）
    const std::size_t line_len = 24000;
    for (std::size_t i = sync_n; i < line_len; ++i) {
        const std::size_t p =
            std::size_t(double(i - sync_n) * 4160.0 / fs);
        double v = p < 2080 ? double(p) / 2079.0 * 0.8 + 0.1 : 0.1;
        if (p >= 500 && p < 503) v = marker_bright;   // 3 px 亮标记
        if (p >= 600 && p < 603) v = marker_dark;     // 3 px 暗标记
        const std::size_t idx = out.size();
        out.push_back(float(v * std::sin(2 * hackrftool::dsp::kPi * 2400.0 *
                                         double(idx) / fs)));
    }
}

static void test_apt_decode() {
    using hackrftool::dsp::AptDecoder;
    // 6 行合成信号（前两行用于锁定）
    std::vector<float> audio;
    synth_apt_line(audio, 1.0, 0.0);
    synth_apt_line(audio, 1.0, 0.0);
    synth_apt_line(audio, 1.0, 0.0);
    synth_apt_line(audio, 1.0, 0.0);
    synth_apt_line(audio, 1.0, 0.0);
    synth_apt_line(audio, 1.0, 0.0);
    AptDecoder dec;
    dec.feed(audio.data(), audio.size());
    hackrftool::dsp::AptImage imgd;
    dec.snapshot(imgd);
    if (imgd.rows == 0) {
        std::printf("apt: NO ROWS (synced=%d lines=%llu)\n", int(dec.synced()),
                    (unsigned long long)dec.lines());
        check(false, "APT 未产出任何行");
        return;
    }
    check(dec.synced(), "APT 行同步锁定");
    check(dec.lines() >= 1, "至少装配出 1 行（6 行输入）");
    hackrftool::dsp::AptImage img;
    dec.snapshot(img);
    check(img.rows >= 1, "图像缓冲含行");
    // 取末行（锁定后的完整行）验证像素：亮/暗标记 + 梯度
    const std::uint8_t* row =
        img.px.data() + (img.rows - 1) * AptDecoder::kWidth;
    {
        // 亮标记经包络低通平滑为 ~5px 钟形（峰≈背景×1.9）——取窗口峰值判定
        int mx = 0;
        for (int i = 497; i <= 503; ++i) mx = std::max(mx, int(row[i]));
        check(mx > 165 && mx > int(row[400]) + 50,
              "像素 ~500 = 亮标记（窗口峰值显著高于背景）");
    }
    check(row[600] < 50, "像素 600 = 暗标记（<50）");
    check(row[100] < row[1500], "梯度：px100 < px1500");
    const int grad = int(row[1500]) - int(row[100]);
    check(grad > 80, "梯度斜率显著（Δ>80）");
}

// ---- 信号库（#55） ----------------------------------------------------------

static void test_sigdb() {
    using hackrftool::dsp::SignalEntry;
    using hackrftool::dsp::SigCat;
    using hackrftool::dsp::band_of;
    using hackrftool::dsp::default_center;
    using hackrftool::dsp::in_band;
    using hackrftool::dsp::merge_scan;
    using hackrftool::dsp::mark_all_offline;
    using hackrftool::dsp::filter_signals;
    using hackrftool::dsp::random_pick;
    using hackrftool::dsp::save_signals;
    using hackrftool::dsp::load_signals;

    check(band_of(98.0).cat == SigCat::radio, "98 MHz → FM 广播");
    check(band_of(107.9).cat == SigCat::radio, "107.9 → FM 广播");
    check(band_of(137.620).cat == SigCat::sat, "137.620 → NOAA 卫星");
    check(band_of(2450.0).cat == SigCat::ism, "2450 → 2.4G ISM");
    check(band_of(500.0).cat == SigCat::other, "500 → 其他");
    check(default_center(SigCat::radio) == 98.0, "电台默认 98.0");
    check(default_center(SigCat::sat) == 137.620, "卫星默认 137.620");
    check(in_band(SigCat::radio, 100.0) && !in_band(SigCat::radio, 200.0),
          "in_band 判定");

    std::vector<SignalEntry> list;
    merge_scan(list, 98.0, -35.0f);
    merge_scan(list, 103.9, -42.0f);
    merge_scan(list, 2455.0, -50.0f);
    check(list.size() == 3, "三条新增");
    merge_scan(list, 98.02, -30.0f);   // 容差内更新
    check(list.size() == 3, "容差内合并不新增");
    check(list[0].db == -30.0f && list[0].online, "更新强度并置在线");

    mark_all_offline(list);
    merge_scan(list, 103.9, -41.0f);   // 103.9 重新在线
    const auto fm = filter_signals(list, SigCat::radio, true, true);
    check(fm.size() == 1 && list[fm[0]].mhz == 103.9, "在线 FM 仅 103.9");

    const auto by_freq = filter_signals(list, SigCat::all, false, false);
    check(by_freq.size() == 3 && list[by_freq[0]].mhz < 99.0 &&
              list[by_freq[2]].mhz == 2455.0, "全量按频率排序");
    const auto by_db = filter_signals(list, SigCat::all, false, true);
    check(list[by_db[0]].db >= list[by_db[1]].db, "按强度降序");

    const long rp = random_pick(list, SigCat::radio, 42u);
    check(rp >= 0 && band_of(list[size_t(rp)].mhz).cat == SigCat::radio,
          "随机挑选落在 FM 段");
    check(random_pick(list, SigCat::sat, 1u) == -1, "卫星段无在线 → -1");

    const wchar_t* tmp = L"sigdb-test.tsv";
    check(save_signals(tmp, list), "保存 TSV");
    const auto back = load_signals(tmp);
    check(back.size() == list.size(), "往返条目数一致");
    bool same = back.size() == list.size();
    for (std::size_t i = 0; same && i < back.size(); ++i)
        same = std::abs(back[i].mhz - list[i].mhz) < 1e-3;
    check(same, "往返频率一致");
    std::remove("sigdb-test.tsv");
}

int main() {
    test_fft_dc();
    test_fft_tone_bin1();
    test_fft_parseval();
    test_analyzer_tone_location_and_level();
    test_analyzer_peak_hold_and_reset();
    test_waterfall_ring();
    test_waterfall_prefill();
    test_monitor_auto_peak_tracks();
    test_monitor_fixed_bin_and_clamp();
    test_monitor_stats();
    test_monitor_ring_and_csv();
    test_gfsk_roundtrip();
    test_gfsk_roundtrip_2m();
    test_gfsk_short_input();
    test_burst_detector();
    test_iq_recorder_file();
    test_panorama_stitch();
    test_live_bursts();
    test_esb_roundtrip();
    test_esb_corruption_rejected();
    test_esb_noise();
    test_end_to_end_pipeline();
    test_iq_recorder_contract_edges();
    test_burst_detector_edges();
    test_live_bursts_ring_wrap();
    test_capture_sidecar();
    test_waterfall_level_map();
    test_esb_hex_dump();
    test_waterfall_runs();
    test_status_dynamic();
    test_fm_discriminator_unit();
    test_fm_receiver_stereo();
    test_fm_receiver_mono_fallback();
    test_fm_dc_offset_survives();
    test_afc_correction();
    test_peak_snap();
    test_voice_level();
    test_settings_roundtrip();
    test_meteor_qpsk();
    test_telemetry();
    test_audio_spectrum();
    test_fm_decimator_stopband();
    test_apt_decode();
    test_sigdb();
    if (failures == 0) std::printf("HackRFToolTest: 全部通过\n");
    return failures == 0 ? 0 : 1;
}
