// 频谱分析器：int8 IQ → 块平均功率谱（dBFS）→ 峰值保持。线程安全。
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace hackrftool::dsp {

struct SpectrumFrame {
    std::vector<float> db;     // 当前帧 dBFS，[-100, 0]
    std::vector<float> peak;   // 峰值保持（每帧衰减 0.2 dB）
    unsigned seq = 0;          // 帧序号，完成一帧 +1
};

class SpectrumAnalyzer {
public:
    SpectrumAnalyzer(std::size_t fft_size = 512, std::size_t average_blocks = 64,
                     std::size_t bins_out = 256);

    // USB 回调线程调用；byte_count 为字节数（I/Q 交错，每样本 2 字节）。
    // 不足一个 FFT 块的尾巴丢弃；chunk_stride_=4 表示每 4 块只算 1 块
    // （功率谱平均抽样，CPU 减负 4 倍，显示质量无损）。
    void feed(const std::int8_t* iq, std::size_t byte_count);

    [[nodiscard]] SpectrumFrame snapshot() const;
    void reset_peaks();
    [[nodiscard]] std::size_t bins() const noexcept { return bins_out_; }

private:
    void finish_frame_locked();

    const std::size_t fft_size_;
    const std::size_t average_blocks_;
    const std::size_t bins_out_;
    const std::size_t skip_;          // 边缘裁剪（基带滤波器过渡带），每侧 fft_size_/16
    static constexpr std::size_t kChunkStride = 4;

    mutable std::mutex mutex_;
    std::vector<double> accum_;                    // 每 bin 功率累计
    std::size_t accum_count_ = 0;
    SpectrumFrame frame_;                          // 最近完成的帧
    std::vector<std::complex<double>> scratch_;    // feed 线程专用，免每次分配
};

} // namespace hackrftool::dsp
