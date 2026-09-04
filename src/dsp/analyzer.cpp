#include "dsp/analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>

#include "dsp/fft.hpp"

namespace hackrftool::dsp {

namespace {
// 满幅参考：int8 单频幅度 127 定义为 0 dBFS
constexpr double kFullScaleAmp = 127.0;
constexpr double kDbFloor = -100.0;
constexpr double kPeakDecayDb = 0.2;   // 每帧峰值衰减
} // namespace

SpectrumAnalyzer::SpectrumAnalyzer(std::size_t fft_size, std::size_t average_blocks,
                                   std::size_t bins_out)
    : fft_size_(fft_size),
      average_blocks_(average_blocks),
      bins_out_(bins_out),
      skip_(fft_size / 16),
      accum_(fft_size, 0.0) {}

void SpectrumAnalyzer::feed(const std::int8_t* iq, std::size_t byte_count) {
    const std::size_t complex_samples = byte_count / 2;
    std::vector<std::complex<double>> buf(fft_size_);

    std::size_t used = 0;
    while (used + fft_size_ <= complex_samples) {
        for (std::size_t i = 0; i < fft_size_; ++i)
            buf[i] = {double(iq[(used + i) * 2]), double(iq[(used + i) * 2 + 1])};
        fft(buf);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (std::size_t k = 0; k < fft_size_; ++k) accum_[k] += std::norm(buf[k]);
            if (++accum_count_ >= average_blocks_) finish_frame_locked();
        }
        used += fft_size_;
    }
}

void SpectrumAnalyzer::finish_frame_locked() {
    const std::size_t usable = fft_size_ - 2 * skip_;
    // 满幅参考功率：幅度 127 的单频累计 average_blocks_ 块后 = 127²·N²·blocks
    const double ref = kFullScaleAmp * kFullScaleAmp * double(fft_size_) *
                       double(fft_size_) * double(accum_count_);

    SpectrumFrame next;
    next.db.resize(bins_out_);
    next.peak.resize(bins_out_);
    next.seq = frame_.seq + 1;
    const float floor_db = float(kDbFloor - 30.0);   // 底以下钳到 -130（与瀑布空底一致）

    for (std::size_t i = 0; i < bins_out_; ++i) {
        const std::size_t in = skip_ + (i * usable + bins_out_ / 2) / bins_out_;
        const double db =
            std::max(double(floor_db), 10.0 * std::log10(accum_[in] / ref + 1e-30));
        next.db[i] = float(db);
        const float prev_peak =
            (frame_.peak.size() == bins_out_) ? frame_.peak[i] : floor_db;
        next.peak[i] = std::max(prev_peak - float(kPeakDecayDb), float(db));
    }

    frame_ = std::move(next);
    std::fill(accum_.begin(), accum_.end(), 0.0);
    accum_count_ = 0;
}

SpectrumFrame SpectrumAnalyzer::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frame_;
}

void SpectrumAnalyzer::reset_peaks() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fill(frame_.peak.begin(), frame_.peak.end(), float(kDbFloor - 30.0));
}

} // namespace hackrftool::dsp
