#include "dsp/burst_detector.hpp"

#include <algorithm>
#include <cmath>

namespace hackrftool::dsp {

std::vector<RfBurst> detect_bursts(const std::int8_t* iq, std::size_t byte_count,
                                   float threshold_db, std::size_t window_samples) {
    std::vector<RfBurst> bursts;
    const std::size_t n_c = byte_count / 2;
    if (n_c == 0 || window_samples == 0) return bursts;

    // 满幅参考：幅度 127 ⇒ 0 dBFS（与 SpectrumAnalyzer 同口径）
    constexpr double kRef = 127.0 * 127.0;
    const std::size_t n_win = n_c / window_samples;
    std::vector<float> win_db(n_win, -300.0f);
    for (std::size_t w = 0; w < n_win; ++w) {
        double acc = 0.0;
        const std::size_t base = w * window_samples * 2;
        for (std::size_t j = 0; j < window_samples; ++j) {
            const double re = double(iq[base + j * 2]);
            const double im = double(iq[base + j * 2 + 1]);
            acc += re * re + im * im;
        }
        win_db[w] = float(10.0 * std::log10(acc / double(window_samples) / kRef + 1e-30));
    }

    // 过阈值连通段，间隙 < 2 窗合并
    const std::ptrdiff_t gap_merge = 2;
    std::ptrdiff_t span_begin = -1;
    std::ptrdiff_t last_above = -1;
    for (std::ptrdiff_t w = 0; w <= std::ptrdiff_t(n_win); ++w) {
        const bool above = (w < std::ptrdiff_t(n_win)) && (win_db[size_t(w)] >= threshold_db);
        if (above) {
            if (span_begin < 0 || w - last_above > gap_merge) {
                if (span_begin >= 0) {
                    // 前一段结束（间隙超限）
                    RfBurst b;
                    b.start_sample =
                        std::max<std::ptrdiff_t>(0, span_begin * std::ptrdiff_t(window_samples) -
                                                        std::ptrdiff_t(window_samples / 2));
                    b.length_samples = std::size_t(
                        (last_above + 1 - span_begin) * std::ptrdiff_t(window_samples) +
                        std::ptrdiff_t(window_samples));
                    float peak = -300.0f;
                    for (std::ptrdiff_t k = span_begin; k <= last_above; ++k)
                        peak = std::max(peak, win_db[size_t(k)]);
                    b.peak_db = peak;
                    b.length_samples = std::min(b.length_samples, n_c - b.start_sample);
                    bursts.push_back(b);
                    span_begin = -1;
                }
                if (above) span_begin = w;
            }
            last_above = w;
        }
    }
    if (span_begin >= 0) {   // 收尾段
        RfBurst b;
        b.start_sample =
            std::max<std::ptrdiff_t>(0, span_begin * std::ptrdiff_t(window_samples) -
                                            std::ptrdiff_t(window_samples / 2));
        b.length_samples = std::size_t((last_above + 1 - span_begin) *
                                           std::ptrdiff_t(window_samples) +
                                       std::ptrdiff_t(window_samples));
        float peak = -300.0f;
        for (std::ptrdiff_t k = span_begin; k <= last_above; ++k)
            peak = std::max(peak, win_db[size_t(k)]);
        b.peak_db = peak;
        b.length_samples = std::min(b.length_samples, n_c - b.start_sample);
        bursts.push_back(b);
    }
    return bursts;
}

} // namespace hackrftool::dsp
