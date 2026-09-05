// 信道监测视图：RSSI 时间条带图（Props::paint 自绘）
#pragma once

#include <flux/flux.hpp>

#include "dsp/channel_monitor.hpp"

namespace hackrftool::ui {

// x=时间（旧→新），y=dBFS [-100,0]，红色横线=活动阈值
// 人声强度滚动波形（#55g）：-60..0 dBFS 话音频带能量，人声越强越高
[[nodiscard]] flux::ElementPtr audio_level_strip(
    const flux::Palette& pal, const std::vector<float>& hist_db, float cur_db,
    unsigned seq);

// 音频频谱（#57）：0..24 kHz 实 FFT 幅度谱 dBFS + 峰保持线 + 19k 导频参考线
[[nodiscard]] flux::ElementPtr audio_spectrum_strip(
    const flux::Palette& pal, const std::vector<float>& spec_db,
    const std::vector<float>& peak_db, unsigned seq);

[[nodiscard]] flux::ElementPtr rssi_strip(
    const flux::Palette& pal, const std::vector<hackrftool::dsp::MonitorSample>& samples,
    float threshold_db, unsigned seq);

} // namespace hackrftool::ui
