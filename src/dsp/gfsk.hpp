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

class GfskDemod {
public:
    GfskDemod(double sample_rate, double symbol_rate, double deviation_hz);

    // start_offset：首符号窗口起点（样本下标，通常来自突发检测）
    [[nodiscard]] GfskResult demod(const std::vector<std::complex<float>>& iq,
                                   std::size_t start_offset) const;

    [[nodiscard]] std::size_t samples_per_symbol() const noexcept { return sps_; }

private:
    double sample_rate_;
    double deviation_hz_;
    std::size_t sps_;
};

} // namespace hackrftool::dsp
