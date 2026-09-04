#include "ui/views.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace hackrftool::ui {

namespace {
constexpr float kDbTop = 0.0f;
constexpr float kDbFloor = -100.0f;

float db_to_y(float db, float y, float h) noexcept {
    const float t = (kDbTop - db) / (kDbTop - kDbFloor);   // 0..1，上小下大
    return y + 4.0f + t * (h - 22.0f);   // 上下留白给轴标签
}

// 16 级瀑布色带：深蓝 → 蓝 → 绿 → 黄 → 红
flux::Color wf_color(int level) noexcept {
    level = std::clamp(level, 0, 15);
    static const flux::Color stops[5] = {
        {16, 24, 44}, {22, 98, 158}, {36, 168, 92}, {235, 200, 60}, {235, 72, 48},
    };
    const float t = float(level) / 15.0f * 4.0f;
    const int i = int(t);
    const float f = t - float(i);
    const auto& a = stops[i];
    const auto& b = stops[std::min(i + 1, 4)];
    return flux::Color{
        static_cast<unsigned char>(float(a.r) + f * float(int(b.r) - int(a.r))),
        static_cast<unsigned char>(float(a.g) + f * float(int(b.g) - int(a.g))),
        static_cast<unsigned char>(float(a.b) + f * float(int(b.b) - int(a.b))), 255};
}
} // namespace

flux::ElementPtr spectrum_view(const flux::Palette& pal,
                               const hackrftool::dsp::SpectrumFrame& frame,
                               double center_mhz) {
    flux::Props p;
    p.flex_grow = 1.0f;
    p.background = pal.surface;
    p.radius = 10.0f;
    p.paint_id = frame.seq;   // 帧不变则保持局部重绘
    p.paint = [pal, frame, center_mhz](flux::D2DRenderer& r, float x, float y,
                                       float w, float h, bool, float, float) {
        // 网格：每 20 dB 一条
        for (float db = -20.0f; db > kDbFloor; db -= 20.0f) {
            const float gy = db_to_y(db, y, h);
            r.draw_line(x + 8.0f, gy, x + w - 8.0f, gy, pal.divider, 1.0f, 0.5f);
        }
        // 频率轴：中心 ±10 MHz（标签钳制在绘图区内，防首尾裁切）
        const auto axis = [&](double mhz, const wchar_t* text) {
            const float fx = x + 8.0f + float((mhz - (center_mhz - 10.0)) / 20.0) *
                                           (w - 16.0f);
            r.draw_line(fx, y + 4.0f, fx, y + h - 20.0f, pal.divider, 1.0f, 0.4f);
            const float lx = std::clamp(fx - 28.0f, x, x + w - 56.0f);
            r.draw_text(flux::Rect{lx, y + h - 18.0f, 56.0f, 14.0f}, text,
                        10.0f, pal.text_secondary, false, flux::Align::center);
        };
        axis(center_mhz - 10.0, L"-10M");
        axis(center_mhz, L"中心");
        axis(center_mhz + 10.0, L"+10M");

        if (frame.db.empty()) {
            r.draw_text(flux::Rect{x, y, w, h}, L"等待数据…", 14.0f, pal.text_secondary,
                        false, flux::Align::center);
            return;
        }
        const std::size_t n = frame.db.size();
        std::vector<std::pair<float, float>> cur(n), peak(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float fx = x + 8.0f + float(i) / float(n - 1) * (w - 16.0f);
            cur[i] = {fx, db_to_y(frame.db[i], y, h)};
            peak[i] = {fx, db_to_y(frame.peak[i], y, h)};
        }
        r.draw_polyline(peak, pal.text_secondary, 1.0f, 0.55f);
        r.draw_polyline(cur, pal.accent, 2.0f, 1.0f);
    };
    return flux::view(std::move(p));
}

flux::ElementPtr waterfall_view(const flux::Palette& pal,
                                const hackrftool::dsp::WaterfallModel& wf,
                                unsigned seq) {
    flux::Props p;
    p.height = 220.0f;
    p.background = pal.surface;
    p.radius = 10.0f;
    p.paint_id = seq;
    p.paint = [pal, &wf](flux::D2DRenderer& r, float x, float y, float w, float h,
                         bool, float, float) {
        const auto snap = wf.snapshot();
        const std::size_t cols = wf.cols(), rows = wf.rows();
        const float cw = w / float(cols), rh = (h - 4.0f) / float(rows);
        for (std::size_t row = 0; row < rows; ++row) {
            for (std::size_t col = 0; col < cols; ++col) {
                const float db = snap[row * cols + col];
                // dB → 16 级：[-100,-40] 映射 [0,15]，≤-100 视为底
                const int level =
                    (db <= kDbFloor)
                        ? 0
                        : std::clamp(int((db - kDbFloor) / 60.0f * 15.0f), 0, 15);
                r.fill_rect(flux::Rect{x + float(col) * cw, y + 2.0f + float(row) * rh,
                                       cw + 1.0f, rh + 1.0f},
                            wf_color(level));
            }
        }
    };
    return flux::view(std::move(p));
}

} // namespace hackrftool::ui
