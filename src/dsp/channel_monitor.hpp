// 信道监测：对选定 bin（自动跟踪最强峰或手动锁定）记录 RSSI 时间序列，
// 提供统计与 CSV 导出。仅 UI 线程调用，无锁。
#pragma once

#include <cstddef>
#include <vector>

#include "dsp/analyzer.hpp"

namespace hackrftool::dsp {

struct MonitorSample {
    double elapsed_s = 0.0;
    float db = -130.0f;
};

class ChannelMonitor {
public:
    struct Stats {
        float mean = -130.0f;
        float peak = -130.0f;
        float variance = 0.0f;
        float duty = 0.0f;   // db >= threshold 的样本占比
        std::size_t count = 0;
    };

    enum class Mode { auto_peak, fixed_bin };

    explicit ChannelMonitor(std::size_t capacity = 3600);

    void set_mode(Mode m) noexcept { mode_ = m; }
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    void set_fixed_bin(std::size_t bin) noexcept;   // 内部 clamp
    [[nodiscard]] std::size_t tracked_bin() const noexcept { return last_bin_; }

    // 有新帧时调用（frame.db 非空才记录）
    void push(const SpectrumFrame& frame);

    [[nodiscard]] std::vector<MonitorSample> series() const;   // 旧→新
    [[nodiscard]] Stats stats(float threshold_db) const;
    [[nodiscard]] bool export_csv(const std::wstring& path) const;

private:
    const std::size_t capacity_;
    std::vector<MonitorSample> ring_;
    std::size_t head_ = 0;   // 下一写入位置
    std::size_t count_ = 0;
    Mode mode_ = Mode::auto_peak;
    std::size_t fixed_bin_ = 128;
    std::size_t last_bin_ = 128;
    unsigned long long start_ms_ = 0;
};

} // namespace hackrftool::dsp
