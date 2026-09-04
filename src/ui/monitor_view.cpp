#include "ui/monitor_view.hpp"

#include <utility>
#include <vector>

namespace hackrftool::ui {

namespace {
constexpr float kDbTop = 0.0f;
constexpr float kDbFloor = -100.0f;

float db_to_y(float db, float y, float h) noexcept {
    const float t = (kDbTop - db) / (kDbTop - kDbFloor);
    return y + 4.0f + t * (h - 22.0f);
}
} // namespace

flux::ElementPtr rssi_strip(const flux::Palette& pal,
                            const std::vector<hackrftool::dsp::MonitorSample>& samples,
                            float threshold_db, unsigned seq) {
    flux::Props p;
    p.flex_grow = 1.0f;
    p.background = pal.surface;
    p.radius = 10.0f;
    p.paint_id = seq;
    p.paint = [pal, samples, threshold_db](flux::D2DRenderer& r, float x, float y,
                                           float w, float h, bool, float, float) {
        // 网格：每 20 dB
        for (float db = -20.0f; db > kDbFloor; db -= 20.0f) {
            const float gy = db_to_y(db, y, h);
            r.draw_line(x + 8.0f, gy, x + w - 8.0f, gy, pal.divider, 1.0f, 0.5f);
        }
        // 活动阈值线（红色）
        const float ty = db_to_y(threshold_db, y, h);
        r.draw_line(x + 8.0f, ty, x + w - 8.0f, ty, pal.danger, 1.5f, 0.8f);

        if (samples.empty()) {
            r.draw_text(flux::Rect{x, y, w, h}, L"等待数据…", 14.0f, pal.text_secondary,
                        false, flux::Align::center);
            return;
        }
        std::vector<std::pair<float, float>> pts(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const float fx = x + 8.0f + float(i) / float(samples.size() - 1) * (w - 16.0f);
            pts[i] = {fx, db_to_y(samples[i].db, y, h)};
        }
        r.draw_polyline(pts, pal.accent, 2.0f, 1.0f);

        // 左侧刻度
        const auto ylab = [&](float db) {
            r.draw_text(flux::Rect{x, db_to_y(db, y, h) - 7.0f, 34.0f, 14.0f},
                        std::to_wstring(int(db)), 10.0f, pal.text_secondary, false,
                        flux::Align::start, 0.7f);
        };
        ylab(0.0f);
        ylab(-50.0f);
        ylab(-100.0f);
    };
    return flux::view(std::move(p));
}

} // namespace hackrftool::ui
