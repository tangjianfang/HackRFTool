// GFSK 解调器：瞬时频率 → 符号窗口均值 → 硬比特 + 置信度。纯函数，无状态。
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hackrftool::dsp {

struct GfskResult {
    std::vector<std::uint8_t> bits;     // 硬比特（1 = 正频偏）
    std::vector<float> quality;         // 每符号 |f_hz| / deviation
};

// AFC（#111）：估计载波频偏（rad/样本）= 突发内瞬时频率均值。GFSK 符号
// 正负对称，足够长时均值≈频偏——判决阈值 0 前必须去旋转（BLE 收发晶振
// 偏差 ±50–120kHz 会直接淹没弱符号）。区间 [from,to) 供跳过突发首尾瞬态
[[nodiscard]] double estimate_freq_offset_rad(
    const std::vector<std::complex<float>>& iq, std::size_t from = 0,
    std::size_t to = static_cast<std::size_t>(-1));

class GfskDemod {
public:
    GfskDemod(double sample_rate, double symbol_rate, double deviation_hz);

    // start_offset：首符号窗口起点（样本下标，通常来自突发检测）。
    // window_frac（#111）：符号内积分窗占比（0.5=只积中半，跳过 BT=0.5
    // 高斯滤波的符号过渡区=匹配滤波近似）；默认 1.0 行为与历史完全一致
    [[nodiscard]] GfskResult demod(const std::vector<std::complex<float>>& iq,
                                   std::size_t start_offset,
                                   float window_frac = 1.0f) const;

    [[nodiscard]] std::size_t samples_per_symbol() const noexcept { return sps_; }

private:
    double sample_rate_;
    double deviation_hz_;
    std::size_t sps_;
};

} // namespace hackrftool::dsp
