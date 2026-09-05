// dB → 瀑布 16 级色阶映射（纯函数，供 UI 与测试共用）
#pragma once

namespace hackrftool::dsp {

// 映射窗 [lo, hi]（默认 [-110,-30]）：≤lo → 0（深蓝），≥hi → 15（红）
constexpr int waterfall_level(float db, float lo = -110.0f, float hi = -30.0f) noexcept {
    if (db <= lo) return 0;
    if (db >= hi) return 15;
    return int((db - lo) / (hi - lo) * 15.0f);
}

} // namespace hackrftool::dsp
