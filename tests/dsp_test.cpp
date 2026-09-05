// 纯 C++ DSP 单元测试 —— 沿用 WinFlux 的裸 main + check 模式，无测试框架
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "dsp/analyzer.hpp"
#include "dsp/burst_detector.hpp"
#include "dsp/channel_monitor.hpp"
#include "dsp/fft.hpp"
#include "dsp/esb.hpp"
#include "dsp/gfsk.hpp"
#include "dsp/level_map.hpp"
#include "dsp/live_bursts.hpp"
#include "dsp/panorama.hpp"
#include "dsp/waterfall.hpp"
#include "radio/iq_recorder.hpp"

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
    if (failures == 0) std::printf("HackRFToolTest: 全部通过\n");
    return failures == 0 ? 0 : 1;
}
