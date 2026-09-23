#include "dsp/gfsk.hpp"

#include <cmath>
#include <numbers>

namespace hackrftool::dsp {

GfskDemod::GfskDemod(double sample_rate, double symbol_rate, double deviation_hz)
    : sample_rate_(sample_rate), deviation_hz_(deviation_hz),
      sps_(static_cast<std::size_t>(sample_rate / symbol_rate + 0.5)) {}

double estimate_freq_offset_rad(
    const std::vector<std::complex<float>>& iq, std::size_t from,
    std::size_t to) {
    // 瞬时相位差向量求和（圆均值，避免 ±π 回绕跳变）
    if (iq.size() < 2 || from >= iq.size() - 1) return 0.0;
    const std::size_t hi = std::min(to, iq.size() - 1);
    if (hi <= from + 1) return 0.0;
    double sx = 0.0, sy = 0.0;
    for (std::size_t k = from + 1; k <= hi; ++k) {
        const auto& a = iq[k];
        const auto& b = iq[k - 1];
        const double re = double(a.real()) * b.real() +
                          double(a.imag()) * b.imag();
        const double im = double(a.imag()) * b.real() -
                          double(a.real()) * b.imag();
        sx += re;
        sy += im;
    }
    return std::atan2(sy, sx);
}

GfskResult GfskDemod::demod(const std::vector<std::complex<float>>& iq,
                            std::size_t start_offset,
                            float window_frac) const {
    GfskResult r;
    if (sps_ == 0 || iq.size() < start_offset + sps_) return r;

    // 瞬时频率（rad/样本）：arg(z[k] · conj(z[k-1]))
    std::vector<float> freq(iq.size(), 0.0f);
    for (std::size_t k = 1; k < iq.size(); ++k) {
        const auto& a = iq[k];
        const auto& b = iq[k - 1];
        const float re = a.real() * b.real() + a.imag() * b.imag();
        const float im = a.imag() * b.real() - a.real() * b.imag();
        freq[k] = std::atan2(im, re);
    }

    // 中窗积分（#111）：只积符号眼图张开区（中心 window_frac 部分），
    // 跳过高斯滤波（BT=0.5）的符号过渡 ramp——ISI 主要来源
    const float wf = window_frac <= 0.0f || window_frac > 1.0f
                         ? 1.0f
                         : window_frac;
    std::size_t skip = static_cast<std::size_t>(
        double(sps_) * (1.0 - double(wf)) * 0.5 + 0.5);
    std::size_t n_int = sps_ - 2 * skip;
    if (n_int == 0) {   // 极低采样率（如 2Msps×0.5 窗）防御：回退全窗
        skip = 0;
        n_int = sps_;
    }

    const std::size_t n_sym = (iq.size() - start_offset) / sps_;
    r.bits.resize(n_sym);
    r.quality.resize(n_sym);
    const double to_hz = sample_rate_ / (2.0 * std::numbers::pi);
    for (std::size_t s = 0; s < n_sym; ++s) {
        double acc = 0.0;
        const std::size_t base = start_offset + s * sps_ + skip;
        for (std::size_t j = 0; j < n_int; ++j) acc += freq[base + j];
        const double f_hz = acc / double(n_int == 0 ? 1 : n_int) * to_hz;
        r.bits[s] = f_hz > 0.0 ? std::uint8_t(1) : std::uint8_t(0);
        r.quality[s] = float(std::abs(f_hz) / deviation_hz_);
    }
    return r;
}

} // namespace hackrftool::dsp
