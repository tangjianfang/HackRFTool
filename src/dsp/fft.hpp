// 迭代式 radix-2 复数 FFT（无第三方依赖，M1 处理量 64×512 点/帧，性能足够）
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace hackrftool::dsp {

inline constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] inline bool is_power_of_two(std::size_t n) noexcept {
    return n != 0 && (n & (n - 1)) == 0;
}

// 就地前向 FFT。前置条件：a.size() 为 2 的幂。
// 幅度约定：bin k 单频（复幅度 A）⇒ |X[k]| = A·N，不做 1/N 归一化。
void fft(std::vector<std::complex<double>>& a);

} // namespace hackrftool::dsp
