// 信道监测视图：RSSI 时间条带图（Props::paint 自绘）
#pragma once

#include <flux/flux.hpp>

#include "dsp/channel_monitor.hpp"

namespace hackrftool::ui {

// x=时间（旧→新），y=dBFS [-100,0]，红色横线=活动阈值
[[nodiscard]] flux::ElementPtr rssi_strip(
    const flux::Palette& pal, const std::vector<hackrftool::dsp::MonitorSample>& samples,
    float threshold_db, unsigned seq);

} // namespace hackrftool::ui
