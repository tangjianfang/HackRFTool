#include "dsp/panorama.hpp"

#include <algorithm>

namespace hackrftool::dsp {

namespace {
constexpr float kMissingDb = -130.0f;
}

PanoramaModel::PanoramaModel(std::size_t segments, std::size_t bins_per_segment)
    : segments_(segments), bins_per_segment_(bins_per_segment),
      data_(segments * bins_per_segment, kMissingDb), fresh_(segments, false) {}

void PanoramaModel::set(std::size_t segment, const std::vector<float>& db) {
    if (segment >= segments_ || db.size() != bins_per_segment_) return;
    std::copy(db.begin(), db.end(),
              data_.begin() + static_cast<std::ptrdiff_t>(segment * bins_per_segment_));
    fresh_[segment] = true;
    ++seq_;
}

bool PanoramaModel::complete() const noexcept {
    for (const bool f : fresh_)
        if (!f) return false;
    return true;
}

void PanoramaModel::clear_fresh() noexcept { std::fill(fresh_.begin(), fresh_.end(), false); }

std::vector<float> PanoramaModel::panorama() const { return data_; }

std::vector<float> PanoramaModel::downscaled(std::size_t cols) const {
    std::vector<float> out(cols, kMissingDb);
    if (cols == 0) return out;
    const std::size_t total = data_.size();
    for (std::size_t c = 0; c < cols; ++c) {
        const std::size_t lo = c * total / cols;
        const std::size_t hi = std::max(lo + 1, (c + 1) * total / cols);
        double acc = 0.0;
        for (std::size_t i = lo; i < hi && i < total; ++i) acc += data_[i];
        out[c] = float(acc / double(hi - lo));
    }
    return out;
}

} // namespace hackrftool::dsp
