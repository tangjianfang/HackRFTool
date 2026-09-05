#include "dsp/meteor.hpp"

#include <algorithm>
#include <cmath>

#include "dsp/fft.hpp"

namespace hackrftool::dsp {

QpskDemod::QpskDemod(double nominal_sps) : sps_nominal_(nominal_sps) {}

std::optional<std::pair<float, float>> QpskDemod::push(std::complex<float> s) {
    // 归一幅度（AGC 外置；此处只保相位信息）
    const float mag = std::abs(s);
    if (mag < 1e-9f) return std::nullopt;
    s /= mag;

    // Costas 校正：旋转 -phase_
    const double cph = std::cos(phase_), sph = std::sin(phase_);
    const std::complex<float> rot(
        float(s.real() * cph + s.imag() * sph),
        float(-s.real() * sph + s.imag() * cph));

    // Gardner 定时：t_ 按 sps 推进，跨过整数即一个符号时刻
    t_ += 1.0;
    const double frac = t_ / sps_nominal_;
    std::optional<std::pair<float, float>> out;
    if (frac >= 0.5 && !have_mid_) {
        prev_mid_ = rot;   // 符号间隔中点（Gardner 判决用）
        have_mid_ = true;
    }
    if (frac >= 1.0) {
        t_ -= sps_nominal_;
        have_mid_ = false;
        // QPSK 四象限 Costas 鉴相（atan(max·sign)+…经典式：err =
        // sign(I)·Q − sign(Q)·I；锁定到 45° 对角簇）
        const float si = rot.real() >= 0 ? 1.0f : -1.0f;
        const float sq = rot.imag() >= 0 ? 1.0f : -1.0f;
        const double pd = double(si) * rot.imag() - double(sq) * rot.real();
        freq_est_ += costas_k_ * 0.02 * pd;
        freq_est_ = std::clamp(freq_est_, -0.05, 0.05);   // ±rad/sample 限幅
        phase_ += freq_est_ + costas_k_ * pd;
        if (phase_ > kPi) phase_ -= 2 * kPi;
        if (phase_ < -kPi) phase_ += 2 * kPi;

        // Gardner 误差：相邻符号差 × 中点（过零时刻偏差）
        if (have_sym_) {
            gardner_err_ =
                double((rot.real() - prev_sym_.real()) * prev_mid_.real() +
                       (rot.imag() - prev_sym_.imag()) * prev_mid_.imag());
            sps_nominal_ += gardner_k_ * 0.004 * gardner_err_;
            sps_nominal_ = std::clamp(sps_nominal_, 4.0, 16.0);
        }
        prev_sym_ = rot;
        have_sym_ = true;

        // 眼图质量：判决点离单位圆的程度（|I|+|Q| → sqrt2 归一）
        const float e = std::abs(rot.real()) + std::abs(rot.imag());
        eye_ += 0.01 * (double(e / 1.41421356f) - eye_);
        out = {rot.real(), rot.imag()};
    }
    return out;
}

std::pair<float, float> qpsk_soft_bits(float i, float q) noexcept {
    // 格雷映射（旋转 45° 到轴对齐再取正负）：b0=I−Q 符号、b1=I+Q 符号
    const float b0 = (i - q) * 0.70710678f;
    const float b1 = (i + q) * 0.70710678f;
    return {b0, b1};
}

long find_ccsds_asm(const std::vector<float>& soft_bits, double* quality_out) {
    if (soft_bits.size() < 32) return -1;
    // ASM 0x1ACFFC1D 的 32 位（MSB 先）
    static const int asm_bits[32] = {
        0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1};
    long best = -1;
    double best_q = 0.0;
    const long n = long(soft_bits.size()) - 32;
    for (long k = 0; k <= n; ++k) {
        double agree = 0.0;
        for (int b = 0; b < 32; ++b) {
            const int want = asm_bits[b] ? 1 : -1;
            agree += want * soft_bits[size_t(k + b)];
        }
        const double q = agree / 32.0;
        if (q > best_q) {
            best_q = q;
            best = k;
        }
    }
    if (quality_out != nullptr) *quality_out = best_q;
    return best_q >= 0.75 ? best : -1;   // 75% 位一致才认帧头
}

} // namespace hackrftool::dsp
