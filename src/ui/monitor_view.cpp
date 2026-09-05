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

flux::ElementPtr audio_level_strip(const flux::Palette& pal,
                                   const std::vector<float>& hist_db,
                                   float cur_db, unsigned seq) {
    flux::Props p;
    p.height = 120.0f;
    p.background = pal.surface;
    p.radius = 10.0f;
    p.paint_id = seq;
    p.paint = [pal, hist_db, cur_db](flux::D2DRenderer& r, float x, float y,
                                     float w, float h, bool, float, float) {
        // 刻度：-60..0 dBFS（-60 即图底，留白给标注）
        const auto y_of = [&](float db) {
            const float t = (0.0f - db) / 60.0f;   // 0dB 顶、-60dB 底
            return y + 4.0f + t * (h - 22.0f);
        };
        for (float db = -15.0f; db > -60.0f; db -= 15.0f) {
            const float gy = y_of(db);
            r.draw_line(x + 8.0f, gy, x + w - 8.0f, gy, pal.divider, 1.0f, 0.5f);
        }
        wchar_t lab[24];
        swprintf(lab, 24, L"人声 %+.0f dB", cur_db);
        r.draw_text(flux::Rect{x + 8.0f, y + 2.0f, 120.0f, 14.0f}, lab, 10.0f,
                    pal.text_secondary, false, flux::Align::start);
        r.draw_text(flux::Rect{x + w - 60.0f, y_of(0.0f) - 7.0f, 40.0f, 14.0f},
                    L"0", 10.0f, pal.text_secondary, false, flux::Align::start);
        r.draw_text(flux::Rect{x + w - 60.0f, y_of(-60.0f) - 7.0f, 40.0f, 14.0f},
                    L"-60", 10.0f, pal.text_secondary, false, flux::Align::start);
        if (hist_db.empty()) {
            r.draw_text(flux::Rect{x, y, w, h}, L"等待音频…", 14.0f,
                        pal.text_secondary, false, flux::Align::center);
            return;
        }
        std::vector<std::pair<float, float>> pts(hist_db.size());
        for (std::size_t i = 0; i < hist_db.size(); ++i)
            pts[i] = {x + 8.0f + float(i) / float(hist_db.size() - 1) * (w - 16.0f),
                      y_of(std::clamp(hist_db[i], -60.0f, 0.0f))};
        r.draw_polyline(pts, pal.accent, 2.0f, 1.0f);
    };
    return flux::view(std::move(p));
}

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
            // 底衬防止波形穿越刻度文字
            r.fill_rect(flux::Rect{x, db_to_y(db, y, h) - 8.0f, 40.0f, 16.0f},
                        pal.surface);
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
