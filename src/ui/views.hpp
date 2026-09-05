// 频谱与瀑布视图：WinFlux 元素 + Props::paint 自定义绘制
#pragma once

#include <flux/flux.hpp>

#include "dsp/analyzer.hpp"
#include "dsp/waterfall.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace hackrftool::ui {

struct SpectrumTick {
    double mhz = 0.0;
    std::wstring label;
};

// 频谱元素几何（paint 每帧回写，供点击位置换算频率）
struct SpectrumGeom {
    float x = 0.0f, w = 1.0f;
    double lo_mhz = 0.0, hi_mhz = 1.0;
};

// 频谱图：任意频率区间 [f_lo, f_hi]，db 为线性 bin 序列（旧→新与频率同向）。
// peak 可为空（无峰值保持线，如全频段全景）。
// geom_out/on_tune（可选，#55 点击调谐）：paint 回写几何；元素被点击时
// 回调 on_tune（无参——main 侧用 host.last_click_pos() 与 geom 换算频率）。
[[nodiscard]] flux::ElementPtr spectrum_view(
    const flux::Palette& pal, const std::vector<float>& db,
    const std::vector<float>& peak, double f_lo_mhz, double f_hi_mhz,
    const std::vector<SpectrumTick>& ticks, unsigned seq,
    SpectrumGeom* geom_out = nullptr, std::function<void()> on_tune = nullptr,
    float y_floor = -100.0f);   // 纵轴底（0 固定顶）；-60 细节档看弱信号

// 瀑布图：历史 64 行，16 级量化配色（深蓝→红）
[[nodiscard]] flux::ElementPtr waterfall_view(const flux::Palette& pal,
                                              const hackrftool::dsp::WaterfallModel& wf,
                                              unsigned seq);

} // namespace hackrftool::ui
