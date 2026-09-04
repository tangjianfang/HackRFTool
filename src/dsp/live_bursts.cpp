#include "dsp/live_bursts.hpp"

#include <algorithm>

#include "dsp/burst_detector.hpp"

namespace hackrftool::dsp {

namespace {
constexpr std::size_t kMaxBursts = 500;
}

LiveBursts::LiveBursts(std::size_t ring_complex)
    : ring_complex_(ring_complex), ring_(ring_complex * 2, 0) {}

void LiveBursts::write(const std::int8_t* iq, std::size_t bytes) {
    const std::size_t complex_in = bytes / 2;
    if (complex_in == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < complex_in; ++i) {
        ring_[ring_pos_ * 2] = iq[i * 2];
        ring_[ring_pos_ * 2 + 1] = iq[i * 2 + 1];
        ring_pos_ = (ring_pos_ + 1) % ring_complex_;
    }
    written_ += complex_in;
}

std::size_t LiveBursts::refresh(float threshold_db, std::size_t window_samples) {
    std::vector<std::int8_t> snapshot;
    unsigned long long base = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (written_ <= detected_upto_) return 0;
        snapshot.assign(ring_.begin(), ring_.end());
        if (written_ >= ring_complex_) {
            // 已回绕：最旧数据在 ring_pos_ 处，旋转成时间序
            std::rotate(snapshot.begin(),
                        snapshot.begin() + static_cast<std::ptrdiff_t>(ring_pos_ * 2),
                        snapshot.end());
            base = written_ - ring_complex_;
        }
        // 未回绕：数据天然在 [0, written)，base=0
    }

    const auto found = detect_bursts(snapshot.data(), snapshot.size(), threshold_db,
                                     window_samples);
    const unsigned long long horizon =
        written_ > window_samples ? written_ - window_samples : 0;

    std::size_t added = 0;
    const unsigned long long last_start =
        bursts_.empty() ? 0 : bursts_.back().start_sample;
    for (const auto& b : found) {
        const unsigned long long abs_start = base + b.start_sample;
        // 仅接受新突发：起点越过既有最大且不在重叠保留区（避免边界半成品）
        if (abs_start > last_start && abs_start < horizon) {
            bursts_.push_back(LiveBurst{abs_start, b.length_samples, b.peak_db});
            ++added;
        }
    }
    if (bursts_.size() > kMaxBursts)
        bursts_.erase(bursts_.begin(),
                      bursts_.end() - static_cast<std::ptrdiff_t>(kMaxBursts));
    detected_upto_ = horizon;
    return added;
}

void LiveBursts::clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    bursts_.clear();
}

unsigned long long LiveBursts::total_samples() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return written_;
}

bool LiveBursts::read_slice(unsigned long long start, unsigned long long count,
                            std::vector<std::int8_t>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const unsigned long long oldest =
        (written_ > ring_complex_) ? written_ - ring_complex_ : 0;
    if (start < oldest || start + count > written_ || count == 0) return false;
    out.resize(static_cast<std::size_t>(count) * 2);
    for (unsigned long long i = 0; i < count; ++i) {
        const std::size_t idx = static_cast<std::size_t>((start + i) % ring_complex_);
        out[static_cast<std::size_t>(i) * 2] = ring_[idx * 2];
        out[static_cast<std::size_t>(i) * 2 + 1] = ring_[idx * 2 + 1];
    }
    return true;
}

} // namespace hackrftool::dsp
