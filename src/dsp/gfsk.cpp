#include "dsp/gfsk.hpp"

#include <cmath>
#include <numbers>

namespace hackrftool::dsp {

GfskDemod::GfskDemod(double sample_rate, double symbol_rate, double deviation_hz)
    : sample_rate_(sample_rate), deviation_hz_(deviation_hz),
      sps_(static_cast<std::size_t>(sample_rate / symbol_rate + 0.5)) {}

GfskResult GfskDemod::demod(const std::vector<std::complex<float>>& iq,
                            std::size_t start_offset) const {
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

    const std::size_t n_sym = (iq.size() - start_offset) / sps_;
    r.bits.resize(n_sym);
    r.quality.resize(n_sym);
    const double to_hz = sample_rate_ / (2.0 * std::numbers::pi);
    for (std::size_t s = 0; s < n_sym; ++s) {
        double acc = 0.0;
        const std::size_t base = start_offset + s * sps_;
        for (std::size_t j = 0; j < sps_; ++j) acc += freq[base + j];
        const double f_hz = acc / double(sps_) * to_hz;
        r.bits[s] = f_hz > 0.0 ? std::uint8_t(1) : std::uint8_t(0);
        r.quality[s] = float(std::abs(f_hz) / deviation_hz_);
    }
    return r;
}

} // namespace hackrftool::dsp
