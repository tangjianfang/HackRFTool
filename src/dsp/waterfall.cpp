#include "dsp/waterfall.hpp"

#include <algorithm>

namespace hackrftool::dsp {

namespace {
constexpr float kEmptyDb = -130.0f;
}

WaterfallModel::WaterfallModel(std::size_t cols, std::size_t rows)
    : cols_(cols), rows_(rows), buf_(rows * cols, kEmptyDb) {}

bool WaterfallModel::push(const std::vector<float>& db) {
    if (db.size() != cols_) return false;
    std::copy(db.begin(), db.end(),
              buf_.begin() + static_cast<std::ptrdiff_t>(next_ * cols_));
    next_ = (next_ + 1) % rows_;
    filled_ = std::min(filled_ + 1, rows_);
    ++seq_;
    return true;
}

std::vector<float> WaterfallModel::snapshot() const noexcept {
    std::vector<float> out;
    out.reserve(rows_ * cols_);
    for (std::size_t r = 0; r < rows_; ++r) {
        const std::size_t src = (next_ + rows_ - 1 - r) % rows_;
        out.insert(out.end(),
                   buf_.begin() + static_cast<std::ptrdiff_t>(src * cols_),
                   buf_.begin() + static_cast<std::ptrdiff_t>((src + 1) * cols_));
    }
    return out;
}

} // namespace hackrftool::dsp
