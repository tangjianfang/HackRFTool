// 频谱与瀑布视图：WinFlux 元素 + Props::paint 自定义绘制
#pragma once

#include <flux/flux.hpp>

#include "dsp/analyzer.hpp"
#include "dsp/waterfall.hpp"

namespace hackrftool::ui {

// 频谱图：当前谱（accent 实线）+ 峰值保持（次级色细线）+ 网格 + 频率轴
[[nodiscard]] flux::ElementPtr spectrum_view(const flux::Palette& pal,
                                             const hackrftool::dsp::SpectrumFrame& frame,
                                             double center_mhz);

// 瀑布图：历史 64 行 × 256 列，16 级量化配色（深蓝→红）
[[nodiscard]] flux::ElementPtr waterfall_view(const flux::Palette& pal,
                                              const hackrftool::dsp::WaterfallModel& wf,
                                              unsigned seq);

} // namespace hackrftool::ui
