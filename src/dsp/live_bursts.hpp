// 实时突发列表：USB 线程写入近期 IQ 环形缓冲，UI 线程增量检测突发。
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace hackrftool::dsp {

struct LiveBurst {
    unsigned long long start_sample = 0;   // 绝对复样本号（自启动累计）
    unsigned long long samples = 0;
    float peak_db = -130.0f;
};

class LiveBursts {
public:
    explicit LiveBursts(std::size_t ring_complex = 2'000'000);   // ~100ms @20Msps

    // USB 回调线程：追加 IQ 字节（I/Q 交错 int8）
    void write(const std::int8_t* iq, std::size_t bytes);

    // UI 线程：对未检测区间跑突发检测，追加新突发（按 start_sample 单调去重）。
    // 返回本次新增条数。
    std::size_t refresh(float threshold_db, std::size_t window_samples = 256);

    [[nodiscard]] const std::vector<LiveBurst>& bursts() const noexcept { return bursts_; }
    void clear() noexcept;
    [[nodiscard]] unsigned long long total_samples() const noexcept;

    // 取 [start, start+count) 的 IQ 切片（字节交错 int8）；越界/已被挤出环返回 false
    [[nodiscard]] bool read_slice(unsigned long long start, unsigned long long count,
                                  std::vector<std::int8_t>& out);

private:
    const std::size_t ring_complex_;
    mutable std::mutex mutex_;
    std::vector<std::int8_t> ring_;         // 2×ring_complex_ 字节
    std::size_t ring_pos_ = 0;              // 下一写入（复样本）
    unsigned long long written_ = 0;        // 累计复样本数
    unsigned long long detected_upto_ = 0;  // 已检测边界
    std::vector<LiveBurst> bursts_;
};

} // namespace hackrftool::dsp
