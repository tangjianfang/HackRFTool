// 突发检测器：滑窗能量（dBFS）→ 阈值连通段合并。纯函数。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hackrftool::dsp {

struct RfBurst {
    std::size_t start_sample = 0;    // 复样本下标
    std::size_t length_samples = 0;  // 复样本数
    float peak_db = -130.0f;
};

// iq：交错 int8 I/Q 字节；window_samples：能量窗口（复样本数），相邻段间隙
// 小于 2×window 合并。返回按时间排序的突发列表。
[[nodiscard]] std::vector<RfBurst> detect_bursts(const std::int8_t* iq,
                                                 std::size_t byte_count,
                                                 float threshold_db,
                                                 std::size_t window_samples);

} // namespace hackrftool::dsp
