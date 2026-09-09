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
        r.draw_text(flux::Rect{x + 8.0f, y + 2.0f, 140.0f, 16.0f}, lab, 12.0f,
                    pal.text_secondary, false, flux::Align::start);
        // 网格刻度（#90：逐线标注，右端对齐；-60 为网格最深线）
        for (float db = -15.0f; db > -60.0f; db -= 15.0f) {
            wchar_t gl[8];
            swprintf(gl, 8, L"%d", int(db));
            r.draw_text(flux::Rect{x + w - 44.0f, y_of(db) - 7.0f, 38.0f, 12.0f},
                        gl, 9.0f, pal.text_secondary, false, flux::Align::start,
                        0.8f);
        }
        r.draw_text(flux::Rect{x + w - 44.0f, y_of(0.0f) - 7.0f, 38.0f, 12.0f},
                    L"0 dB", 9.0f, pal.text_secondary, false, flux::Align::start);
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

flux::ElementPtr audio_spectrum_strip(
    const flux::Palette& pal, const std::vector<float>& spec_db,
    const std::vector<float>& peak_db, unsigned seq, bool pilot_on) {
    flux::Props p;
    p.height = 150.0f;
    p.background = pal.surface;
    p.radius = 10.0f;
    p.paint_id = seq;
    p.paint = [pal, spec_db, peak_db, pilot_on](flux::D2DRenderer& r, float x, float y,
                                      float w, float h, bool, float, float) {
        const float top = y + 4.0f, bot = y + h - 18.0f;
        // 纵轴 -90..-10 dBFS（语音带内典型 -60..-20）
        const auto y_of = [&](float db) {
            const float t = (-10.0f - db) / 80.0f;
            return top + t * (bot - top);
        };
        // 横轴 20Hz–20kHz 人耳可听域（#107：原 0–24k 含超声段，噪声杂波
        // 全谱铺满看不清谱形）；2048 点 FFT@48k → bin 23.4Hz
        const auto x_of = [&](double hz) {
            return float(x + 8.0 + (std::clamp(hz, 20.0, 20000.0) - 20.0) /
                                   19980.0 * double(w - 16.0));
        };
        for (float db = -20.0f; db > -90.0f; db -= 20.0f) {
            const float gy = y_of(db);
            r.draw_line(x + 8.0f, gy, x + w - 8.0f, gy, pal.divider, 1.0f, 0.5f);
        }
        // 纵轴刻度（#90：与网格线同源标注；原 -10/-90 两值不落在网格线上）
        const auto ylab = [&](float db, bool unit = false) {
            wchar_t lab[12];
            swprintf(lab, 12, unit ? L"%d dB" : L"%d", int(db));
            r.draw_text(flux::Rect{x + w - 46.0f, y_of(db) - 7.0f, 40.0f, 14.0f},
                        lab, 9.0f, pal.text_secondary, false, flux::Align::start,
                        0.8f);
        };
        ylab(-20.0f, true);
        for (float db = -40.0f; db > -90.0f; db -= 20.0f) ylab(db);
        // 横轴刻度：20Hz 起点 + 5k..20k（#107 新量程）
        {
            const float gx0 = x_of(20.0);
            r.draw_line(gx0, bot, gx0, bot + 4.0f, pal.divider, 1.0f, 0.5f);
            r.draw_text(flux::Rect{gx0 - 14.0f, bot + 4.0f, 28.0f, 12.0f}, L"20",
                        9.0f, pal.text_secondary, false, flux::Align::center);
        }
        for (int khz = 5; khz <= 20; khz += 5) {
            const float gx = x_of(double(khz) * 1000.0);
            r.draw_line(gx, bot, gx, bot + 4.0f, pal.divider, 1.0f, 0.5f);
            wchar_t lab[12];
            swprintf(lab, 12, L"%dk", khz);
            r.draw_text(flux::Rect{gx - 14.0f, bot + 4.0f, 28.0f, 12.0f}, lab,
                        9.0f, pal.text_secondary, false, flux::Align::center);
        }
        // 19 kHz 导频参考（#107：仅立体声锁定时高亮为导频峰，否则灰化刻度）
        const float px = x_of(19000.0);
        r.draw_line(px, top, px, bot, pilot_on ? pal.accent : pal.divider, 1.0f,
                    pilot_on ? 0.9f : 0.5f);
        r.draw_text(flux::Rect{px - 20.0f, top + 2.0f, 40.0f, 12.0f}, L"19k 导频",
                    9.0f, pilot_on ? pal.accent : pal.text_secondary, false,
                    flux::Align::center);
        r.draw_text(flux::Rect{x + 8.0f, y + 2.0f, 200.0f, 16.0f},
                    L"音频频谱 20 Hz–20 kHz（静噪开时更新）", 12.0f,
                    pal.text_secondary, false, flux::Align::start);
        if (spec_db.empty()) {
            r.draw_text(flux::Rect{x, y, w, h}, L"等待音频…", 14.0f, pal.text_secondary,
                        false, flux::Align::center);
            return;
        }
        // bins → 20Hz..20kHz 均匀重采样绘制（非整 bin 映射，线性插值）
        const auto s_at = [&](double hz) -> float {
            const double b = hz / 23.4375;
            const std::size_t i0 = std::min<std::size_t>(
                std::size_t(b), spec_db.size() - 1);
            const std::size_t i1 = std::min<std::size_t>(i0 + 1, spec_db.size() - 1);
            const double f = b - std::floor(b);
            return float(spec_db[i0] * (1.0 - f) + spec_db[i1] * f);
        };
        const auto p_at = [&](double hz) -> float {
            if (peak_db.empty()) return s_at(hz);
            const double b = hz / 23.4375;
            const std::size_t i0 = std::min<std::size_t>(
                std::size_t(b), peak_db.size() - 1);
            const std::size_t i1 = std::min<std::size_t>(i0 + 1, peak_db.size() - 1);
            const double f = b - std::floor(b);
            return float(peak_db[i0] * (1.0 - f) + peak_db[i1] * f);
        };
        const std::size_t n = 256;
        std::vector<std::pair<float, float>> pk(n), cur(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double hz = 20.0 + double(i) / double(n - 1) * 19980.0;
            const float cx = x_of(hz);
            cur[i] = {cx, y_of(std::clamp(s_at(hz), -90.0f, -10.0f))};
            pk[i] = {cx, y_of(std::clamp(p_at(hz), -90.0f, -10.0f))};
        }
        r.draw_polyline(pk, pal.text_secondary, 1.0f, 0.7f);   // 峰保持（淡）
        r.draw_polyline(cur, pal.accent, 1.6f, 1.0f);           // 实时谱
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

        // 左侧刻度（#90：与 20dB 网格同源逐线标注，顶格带单位；-100 底界不标）
        const auto ylab = [&](float db, bool unit = false) {
            wchar_t lab[12];
            swprintf(lab, 12, unit ? L"%d dB" : L"%d", int(db));
            // 底衬防止波形穿越刻度文字
            r.fill_rect(flux::Rect{x, db_to_y(db, y, h) - 8.0f, 40.0f, 16.0f},
                        pal.surface);
            r.draw_text(flux::Rect{x, db_to_y(db, y, h) - 7.0f, 44.0f, 14.0f},
                        lab, 10.0f, pal.text_secondary, false,
                        flux::Align::start, 0.7f);
        };
        ylab(kDbTop, true);
        for (float db = -20.0f; db > kDbFloor + 1.0f; db -= 20.0f) ylab(db);
    };
    return flux::view(std::move(p));
}

} // namespace hackrftool::ui
