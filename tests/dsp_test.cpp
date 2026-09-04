// 纯 C++ DSP 单元测试 —— 沿用 WinFlux 的裸 main + check 模式，无测试框架
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

#include "dsp/analyzer.hpp"
#include "dsp/channel_monitor.hpp"
#include "dsp/fft.hpp"
#include "dsp/waterfall.hpp"

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
    if (failures == 0) std::printf("HackRFToolTest: 全部通过\n");
    return failures == 0 ? 0 : 1;
}
