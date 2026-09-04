#include "dsp/channel_monitor.hpp"

#include <algorithm>
#include <cstdio>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace hackrftool::dsp {

ChannelMonitor::ChannelMonitor(std::size_t capacity) : capacity_(capacity) {
    ring_.reserve(capacity_);
}

void ChannelMonitor::set_fixed_bin(std::size_t bin) noexcept {
    fixed_bin_ = std::min(bin, std::size_t(255));
}

void ChannelMonitor::push(const SpectrumFrame& frame) {
    if (frame.db.empty()) return;
    const unsigned long long now_ms = GetTickCount64();
    if (start_ms_ == 0) start_ms_ = now_ms;

    if (mode_ == Mode::auto_peak) {
        std::size_t argmax = 0;
        for (std::size_t i = 1; i < frame.db.size(); ++i)
            if (frame.db[i] > frame.db[argmax]) argmax = i;
        last_bin_ = argmax;
    } else {
        last_bin_ = std::min(fixed_bin_, frame.db.size() - 1);
    }

    MonitorSample s;
    s.elapsed_s = double(now_ms - start_ms_) / 1000.0;
    s.db = frame.db[last_bin_];
    if (count_ < capacity_) {
        ring_.push_back(s);
        ++count_;
        head_ = count_ % capacity_;
    } else {
        ring_[head_] = s;
        head_ = (head_ + 1) % capacity_;
    }
}

std::vector<MonitorSample> ChannelMonitor::series() const {
    std::vector<MonitorSample> out;
    out.reserve(count_);
    const std::size_t start = (count_ == capacity_) ? head_ : 0;
    for (std::size_t i = 0; i < count_; ++i)
        out.push_back(ring_[(start + i) % capacity_]);
    return out;
}

ChannelMonitor::Stats ChannelMonitor::stats(float threshold_db) const {
    Stats st;
    if (count_ == 0) return st;
    const auto s = series();
    double sum = 0.0, above = 0.0;
    st.peak = s.front().db;
    for (const auto& sm : s) {
        sum += sm.db;
        st.peak = std::max(st.peak, sm.db);
        if (sm.db >= threshold_db) above += 1.0;
    }
    st.count = s.size();
    st.mean = float(sum / double(s.size()));
    double var = 0.0;
    for (const auto& sm : s) var += double(sm.db - st.mean) * double(sm.db - st.mean);
    st.variance = float(var / double(s.size()));
    st.duty = float(above / double(s.size()));
    return st;
}

bool ChannelMonitor::export_csv(const std::wstring& path) const {
    if (std::FILE* fp = _wfopen(path.c_str(), L"w")) {
        std::fprintf(fp, "elapsed_s,db\n");
        for (const auto& sm : series())
            std::fprintf(fp, "%.3f,%.1f\n", sm.elapsed_s, sm.db);
        std::fclose(fp);
        return true;
    }
    return false;
}

} // namespace hackrftool::dsp
