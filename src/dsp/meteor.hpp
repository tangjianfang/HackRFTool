// Meteor M2 LRPT 解调（#70）：72 ksym/s QPSK（带内 120 kHz）软解调。
// 阶段 1 纯函数：Costas 载波环（QPSK 四象限鉴相）+ Gardner 符号定时 +
// 软符号流输出。帧同步（CCSDS ASM）与维特比/解压缩后置。
// 纯 C++ 无 UI/硬件依赖，dsp_test 以合成 QPSK 加噪直测。
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace hackrftool::dsp {

// QPSK Costas 环 + Gardner 定时解调器
//   feed: 逐基带复样本（fm 解调后同相/正交归一化幅度）
//   输出: 软符号对 (I,Q)∈[-1,1]²，每符号一次（Gardner 定时驱动）
class QpskDemod {
public:
    // sps：样本/符号（如 48k 音频带内 72ksym → 不可整除，环内自适应）
    explicit QpskDemod(double nominal_sps);

    // 喂一个样本；返回本样本触发的软符号（多数样本无符号产出）
    [[nodiscard]] std::optional<std::pair<float, float>> push(
        std::complex<float> s);

    // 环状态诊断（遥测/UI：锁定判定用）
    [[nodiscard]] double freq_offset_hz() const noexcept { return freq_est_; }
    [[nodiscard]] double timing_error() const noexcept { return gardner_err_; }
    // 眼图质量：符号判决半径方差（1=完美，收敛后 >0.85 视为锁定）
    [[nodiscard]] double eye_quality() const noexcept { return eye_; }

    // 环路参数（合成测试标定：捕获带宽 ~±3kHz、收敛 <5000 符号）
    void set_loop_gains(double costas_k, double gardner_k) noexcept {
        costas_k_ = costas_k;
        gardner_k_ = gardner_k;
    }

private:
    double sps_nominal_;
    double phase_ = 0.0;      // Costas 校正相位
    double freq_est_ = 0.0;   // 频率跟踪（rad/sample）
    double t_ = 0.0;          // Gardner 定时计数（浮点符号相位）
    std::complex<float> prev_mid_{}, prev_sym_{};   // Gardner 前一中点/符号
    bool have_mid_ = false, have_sym_ = false;
    double gardner_err_ = 0.0;
    double eye_ = 0.0;
    double costas_k_ = 0.02, gardner_k_ = 0.08;
};

// QPSK 符号 → 比特流（格雷映射：±45°/±135° → 2 bit/符号）
// soft ∈ [-1,1]²；输出软比特（+1/−1），每符号 2 个
[[nodiscard]] std::pair<float, float> qpsk_soft_bits(float i, float q) noexcept;

// CCSDS ASM 帧同步（0x1ACFFC1D，QPSK 两路交错）：在软比特流里找 ASM，
// 返回匹配质量最高的起点下标（找不到 = -1）。容忍相位模糊（I/Q 互换
// 与反号——Costas 收敛的四种可能），匹配质量归一 0..1
[[nodiscard]] long find_ccsds_asm(const std::vector<float>& soft_bits,
                                  double* quality_out = nullptr);

} // namespace hackrftool::dsp
