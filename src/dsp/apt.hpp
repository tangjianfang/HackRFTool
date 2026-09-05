// NOAA APT 卫星云图解码（#54）：48 kHz 音频 → 2.4 kHz 子载波 I/Q 同步检波
// → 1040 Hz 行同步 → 2080 像素行装配 → 灰度图像环形缓冲。
// 纯 C++；feed 在 fm 线程调用、image() 在 UI 线程快照（互斥保护）。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace hackrftool::dsp {

// APT 图像：宽 2080 灰度行，max_rows 滚动窗口（一次过境 ~1440 行）
struct AptImage {
    static constexpr std::size_t kWidth = 2080;
    std::vector<std::uint8_t> px;   // rows*kWidth
    std::size_t rows = 0;
    std::size_t max_rows = 1800;
    std::uint64_t total_lines = 0;   // 累计行数（含滚出）

    void push_line(const std::uint8_t* line) {
        if (rows < max_rows) {
            px.insert(px.end(), line, line + kWidth);
            ++rows;
        } else {
            px.erase(px.begin(), px.begin() + kWidth);
            px.insert(px.end(), line, line + kWidth);
        }
        ++total_lines;
    }
};

class AptDecoder {
public:
    static constexpr std::size_t kWidth = AptImage::kWidth;
    static constexpr double kAudioHz = 48000.0;
    static constexpr std::size_t kLineSamples = 24000;   // 0.5 s/行
    static constexpr std::size_t kSyncDelay = 23;        // 1040 Hz 半周期样本

    void feed(const float* audio, std::size_t n);

    [[nodiscard]] bool synced() const;
    [[nodiscard]] std::uint64_t lines() const;
    void snapshot(AptImage& out);   // UI 线程取图像拷贝

private:
    void envelope_step(float x);
    void sync_step(float env);
    void emit_pixel(float env);

    // 2.4 kHz NCO（频率固定：FM 解调后子载波不受多普勒影响）
    double nco_phase_ = 0.0;
    float i_lpf_ = 0.0f, q_lpf_ = 0.0f;
    // 包络 AGC（慢速最大值跟踪，归一 0..1）
    float env_max_ = 0.0f;
    // 像素装配：48000/4160 ≈ 11.538 样本/像素
    double px_budget_ = kAudioHz / 4160.0;
    double px_pos_ = 0.0;
    float px_acc_ = 0.0f;
    std::size_t px_cnt_ = 0;
    std::uint8_t line_[kWidth];
    std::size_t line_pos_ = 0;
    // 行同步：包络 1040 Hz 正交分量（相位无关）→ 平滑 → 峰沿 + 周期验证
    float sq_i_ = 0.0f, sq_q_ = 0.0f;   // 1040 Hz I/Q 检测低通
    double sq_phase_ = 0.0;             // 1040 Hz NCO 相位
    float sync_sm_ = 0.0f;
    float sync_max_ = 0.0f;
    bool in_peak_ = false;   // 峰沿检测（同步串期间条件持续满足，只取进入沿）
    std::size_t since_peak_ = 0;
    long expect_next_ = -1;   // 期望下一峰距离（样本）；-1 未锁定
    std::size_t lock_hits_ = 0;
    // 输出
    mutable std::mutex mtx_;
    AptImage img_;
    std::atomic<bool> synced_{false};
    float lo_ = 1e9f, hi_ = -1e9f;   // 归一化包络动态范围（自动对比度）
};

} // namespace hackrftool::dsp
