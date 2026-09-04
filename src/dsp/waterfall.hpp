// 瀑布历史：定长环形行缓冲。仅 UI 线程调用，无锁。
#pragma once

#include <cstddef>
#include <vector>

namespace hackrftool::dsp {

class WaterfallModel {
public:
    WaterfallModel(std::size_t cols = 256, std::size_t rows = 64);

    // db 长度必须 == cols；成功返回 true 并使 seq +1
    bool push(const std::vector<float>& db);

    // rows*cols，行 0 最新；未填满的行 = -130
    [[nodiscard]] std::vector<float> snapshot() const noexcept;

    [[nodiscard]] unsigned seq() const noexcept { return seq_; }
    [[nodiscard]] std::size_t rows() const noexcept { return rows_; }
    [[nodiscard]] std::size_t cols() const noexcept { return cols_; }

private:
    const std::size_t cols_;
    const std::size_t rows_;
    std::vector<float> buf_;   // rows*cols 环形
    std::size_t next_ = 0;     // 下一写入行
    std::size_t filled_ = 0;
    unsigned seq_ = 0;
};

} // namespace hackrftool::dsp
