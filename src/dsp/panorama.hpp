// 全景拼接模型：N 段（每段 bins_per_segment）拼接为全频段 bin 序列。UI 线程，无锁。
#pragma once

#include <cstddef>
#include <vector>

namespace hackrftool::dsp {

class PanoramaModel {
public:
    PanoramaModel(std::size_t segments = 5, std::size_t bins_per_segment = 256);

    // 段越界或 db 长度不符时忽略；成功则置新鲜位且 seq+1
    void set(std::size_t segment, const std::vector<float>& db);
    // 全部段新鲜？
    [[nodiscard]] bool complete() const noexcept;
    // 清新鲜位（开始新一轮），保留已有数据供继续显示
    void clear_fresh() noexcept;
    // segments × bins_per_segment（未齐的段为 -130）
    [[nodiscard]] std::vector<float> panorama() const;
    // 均值降采样到 cols 列
    [[nodiscard]] std::vector<float> downscaled(std::size_t cols) const;
    [[nodiscard]] unsigned seq() const noexcept { return seq_; }
    [[nodiscard]] std::size_t segments() const noexcept { return segments_; }
    [[nodiscard]] std::size_t bins() const noexcept { return bins_per_segment_; }

private:
    const std::size_t segments_;
    const std::size_t bins_per_segment_;
    std::vector<float> data_;
    std::vector<bool> fresh_;
    unsigned seq_ = 0;
};

} // namespace hackrftool::dsp
