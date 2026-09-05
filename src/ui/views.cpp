#include "ui/views.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "dsp/level_map.hpp"

namespace hackrftool::ui {

namespace {
constexpr float kDbTop = 0.0f;
constexpr float kDbFloor = -100.0f;

float db_to_y(float db, float y, float h, float floor_db) noexcept {
    const float t = (kDbTop - std::clamp(db, floor_db, kDbTop)) /
                    (kDbTop - floor_db);   // 0..1，上小下大
    return y + 4.0f + t * (h - 22.0f);   // 上下留白给轴标签
}

// dB → 16 级色阶（dsp/level_map.hpp，映射窗 [-110,-30]：原 [-100,-40] 下
// 深蓝档在常用增益（16/16，底噪 -70~-50）永不触发）

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

flux::ElementPtr spectrum_view(const flux::Palette& pal, const std::vector<float>& db,
                               const std::vector<float>& peak, double f_lo_mhz,
                               double f_hi_mhz, const std::vector<SpectrumTick>& ticks,
                               unsigned seq, SpectrumGeom* geom_out,
                               std::function<void()> on_tune,
                               float y_floor) {
    flux::Props p;
    p.flex_grow = 1.0f;
    p.background = pal.surface;
    p.radius = 10.0f;
    p.paint_id = seq;   // 数据不变则保持局部重绘
    p.paint = [pal, db, peak, f_lo_mhz, f_hi_mhz, ticks, geom_out, y_floor](
                  flux::D2DRenderer& r, float x, float y, float w, float h,
                  bool, float, float) {
        if (geom_out != nullptr) {   // 点击换算用几何（每帧刷新）
            geom_out->x = x;
            geom_out->w = w;
            geom_out->lo_mhz = f_lo_mhz;
            geom_out->hi_mhz = f_hi_mhz;
        }
        // 网格：每 (span/5) dB 一条（全动态 20dB 细节档 12dB）
        const float gstep = (kDbTop - y_floor) / 5.0f;
        for (float grid_db = kDbTop - gstep; grid_db > y_floor + 1.0f;
             grid_db -= gstep) {
            const float gy = db_to_y(grid_db, y, h, y_floor);
            r.draw_line(x + 8.0f, gy, x + w - 8.0f, gy, pal.divider, 1.0f, 0.5f);
        }
        // 频率刻度（标签钳制在绘图区内，防首尾裁切）
        const double span = std::max(f_hi_mhz - f_lo_mhz, 1e-6);
        for (const auto& tick : ticks) {
            const float fx = x + 8.0f + float((tick.mhz - f_lo_mhz) / span) * (w - 16.0f);
            r.draw_line(fx, y + 4.0f, fx, y + h - 20.0f, pal.divider, 1.0f, 0.4f);
            const float lx = std::clamp(fx - 28.0f, x, x + w - 56.0f);
            r.draw_text(flux::Rect{lx, y + h - 18.0f, 56.0f, 14.0f}, tick.label, 10.0f,
                        pal.text_secondary, false, flux::Align::center);
        }
        // 纵轴 dB 刻度（与监测页 RSSI 图一致；-100 即图底界，该处空间留给
        // 底部频率刻度，标注会重叠故跳过）。注意：必须在波形折线之后调用——
        // 渲染器顺序绘制无分层，先画会被后画的波形盖住（检查员 F4 驳回点）
        const auto ylab = [&](float db) {
            // 底衬防止网格/波形穿越刻度文字
            r.fill_rect(
                flux::Rect{x, db_to_y(db, y, h, y_floor) - 8.0f, 40.0f, 16.0f},
                pal.surface);
            r.draw_text(
                flux::Rect{x + 2.0f, db_to_y(db, y, h, y_floor) - 7.0f, 34.0f,
                           14.0f},
                        std::to_wstring(int(db)), 10.0f, pal.text_secondary, false,
                        flux::Align::start, 0.7f);
        };

        if (db.empty()) {
            ylab(0.0f);
            ylab(y_floor + (kDbTop - y_floor) / 2.0f);
            r.draw_text(flux::Rect{x, y, w, h}, L"等待数据…", 14.0f, pal.text_secondary,
                        false, flux::Align::center);
            return;
        }
        const std::size_t n = db.size();
        std::vector<std::pair<float, float>> cur(n);
        for (std::size_t i = 0; i < n; ++i)
            cur[i] = {x + 8.0f + float(i) / float(n - 1) * (w - 16.0f),
                      db_to_y(db[i], y, h, y_floor)};
        if (peak.size() == n) {
            std::vector<std::pair<float, float>> pk(n);
            for (std::size_t i = 0; i < n; ++i)
                pk[i] = {cur[i].first, db_to_y(peak[i], y, h, y_floor)};
            r.draw_polyline(pk, pal.text_secondary, 1.0f, 0.55f);
        }
        r.draw_polyline(cur, pal.accent, 2.0f, 1.0f);
        ylab(0.0f);   // 刻度最后画：底衬才盖得住波形（顺序绘制无分层）
        ylab(y_floor + (kDbTop - y_floor) / 2.0f);
    };
    if (on_tune) p.on_click = std::move(on_tune);   // 点频谱=调谐（#55）
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
    p.paint = [pal, &wf, seq](flux::D2DRenderer& r, float x, float y, float w, float h,
                              bool, float, float) {
        if (seq == 0) {   // 尚无数据：占位提示，避免空黑面板
            r.draw_text(flux::Rect{x, y, w, h}, L"等待数据…", 14.0f,
                        pal.text_secondary, false, flux::Align::center);
            return;
        }
        const auto snap = wf.snapshot();
        const std::size_t cols = wf.cols(), rows = wf.rows();
        const float cw = w / float(cols), rh = (h - 4.0f) / float(rows);
        std::vector<int> levels(snap.size());
        for (std::size_t i = 0; i < snap.size(); ++i)
            levels[i] = hackrftool::dsp::waterfall_level(snap[i]);
        // 同值横向行程合并：底噪平坦区整行 1 个矩形，fill_rect 数大幅下降
        //（窗口拉伸时每次 WM_SIZE 全量重绘，收益最直接）
        for (const auto& run : hackrftool::dsp::waterfall_runs(levels, cols)) {
            r.fill_rect(flux::Rect{x + float(run.col0) * cw,
                                   y + 2.0f + float(run.row) * rh,
                                   float(run.len) * cw + 1.0f, rh + 1.0f},
                        wf_color(run.level));
        }
    };
    return flux::view(std::move(p));
}

} // namespace hackrftool::ui
