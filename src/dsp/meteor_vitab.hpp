// Meteor LRPT 维特比译码（EP-1.1）：CCSDS 卷积码 r=1/2、K=7、
// G1=0x79(171o)、G2=0x5B(133o)，G2 相位反转输出（转移检测惯例）。
// 软判决（±1 对）→ 最大似然路径 → 信息比特流。
// 编码器一并提供（合成测试自回环）；纯 C++ 无 UI/硬件依赖。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hackrftool::dsp {

// 卷积编码器（K=7 移位寄存器；输出 G1 直接 + G2 取反，bit 对交错）
class ConvolutionalEncoder {
public:
    // 输入 0/1 比特，输出一对软符号约定值（+1/-1）：{G1, G2}
    [[nodiscard]] std::pair<int, int> push(int bit);

    [[nodiscard]] std::uint64_t bits_in() const noexcept { return n_; }

private:
    std::uint64_t reg_ = 0;   // 低 6 位为移位寄存器（最新在 bit0）
    std::uint64_t n_ = 0;
};

// 软判决维特比（64 状态、回溯深度 72 ≈ 5×(K+7)）
class ViterbiDecoder {
public:
    ViterbiDecoder();

    // 喂一对软符号（±1 附近任意实数；噪声容错）；每次产出一个信息比特
    // （回溯引入固定延迟——首 72 比特在 pipeline 填充期，产出即有效流）
    int push(float soft_g1, float soft_g2);

    // 诊断：当前最优/最差路径度量差（>30 视为高置信锁定）
    [[nodiscard]] float metric_spread() const noexcept;

    // 误码统计（自回环合成用）：清零后累计 push 产出的比特错误
    void reset_stats() noexcept;
    [[nodiscard]] std::uint64_t bit_errors() const noexcept { return errors_; }
    void count_error() noexcept { ++errors_; }

private:
    float pm_[64];            // 路径度量（加比选后）
    float pm_next_[64];
    // 回溯历史：每步每状态的前驱（6bit）——环形 72 层
    std::uint8_t hist_[72][64];
    std::size_t hist_pos_ = 0;
    std::uint64_t depth_ = 0;
    std::uint64_t errors_ = 0;
};

} // namespace hackrftool::dsp
