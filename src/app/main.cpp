// HackRFTool —— 2.4G 频谱检测工具（M1 频谱 + M2 信道监测）
#include <windows.h>

#include <commdlg.h>

#include <algorithm>
#include <cwchar>
#include <string>

#include <flux/flux.hpp>
#include <flux/Components.hpp>

#include "dsp/analyzer.hpp"
#include "dsp/channel_monitor.hpp"
#include "dsp/panorama.hpp"
#include "dsp/waterfall.hpp"
#include "radio/hackrf.hpp"
#include "ui/monitor_view.hpp"
#include "ui/views.hpp"

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

// 一位小数的宽字符串（统计展示用）
std::wstring wd1(double v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f", v);
    return buf;
}

struct App {
    flux::Host host;
    hackrftool::dsp::SpectrumAnalyzer analyzer{512, 64, 256};
    hackrftool::dsp::WaterfallModel waterfall{256, 64};        // 单窗瀑布
    hackrftool::dsp::WaterfallModel waterfall_sweep{320, 64};  // 全频段瀑布
    hackrftool::dsp::PanoramaModel pano{5, 256};               // 全频段拼接
    hackrftool::dsp::ChannelMonitor monitor{3600};
    hackrftool::radio::HackRadio radio;

    flux::State<bool> running;
    flux::State<std::wstring> status;
    flux::State<std::wstring> freq_text;   // MHz 文本
    flux::State<double> center_mhz;
    flux::State<double> lna;   // 8..40，步进 8
    flux::State<double> vga;   // 2..62，步进 2
    flux::State<int> rate_index;
    // M4：全频段扫描
    flux::State<int> sweep_on;             // 0=单窗 1=全频段
    ULONGLONG hop_ms = 0;                  // 上次跳频时刻
    std::size_t seg_idx = 4;               // 从 4 起步，首次跳频即到段 0
    // M2：监测页
    flux::State<int> page;                 // 0=频谱 1=信道监测
    flux::State<bool> auto_track;          // 自动跟踪最强峰
    flux::State<std::wstring> mon_text;    // 监测目标 MHz 文本
    flux::State<double> threshold;         // 活动阈值 dB
    hackrftool::dsp::SpectrumFrame frame;   // 本帧快照（build 前拉取）
};

constexpr double kSweepLoMhz = 2400.0;
constexpr double kSweepHiMhz = 2483.5;
constexpr ULONGLONG kDwellMs = 200;    // 每段驻留
constexpr ULONGLONG kSettleMs = 30;    // 跳频后静默（丢弃旧频数据）

// 段中心：usable 17.5 MHz（20 MHz 窗每侧裁 1/16），5 段均匀覆盖 83.5 MHz
double seg_center_mhz(std::size_t i) {
    constexpr double usable = 17.5;
    return kSweepLoMhz + usable / 2.0 + double(i) * (kSweepHiMhz - kSweepLoMhz - usable) / 4.0;
}

constexpr double kRatesMsps[4] = {8.0, 10.0, 16.0, 20.0};

double clamp_center(double mhz) noexcept { return std::clamp(mhz, 2400.0, 2483.5); }

std::size_t mhz_to_bin(double mhz, double center_mhz) noexcept {
    const double t = (mhz - (center_mhz - 10.0)) / 20.0;   // 0..1
    return static_cast<std::size_t>(std::clamp(t, 0.0, 1.0) * 255.0 + 0.5);
}

void rx_trampoline_ui(const std::int8_t* iq, std::size_t bytes, void* ctx) {
    static_cast<hackrftool::dsp::SpectrumAnalyzer*>(ctx)->feed(iq, bytes);
}

void apply_radio(App& app) {
    hackrftool::radio::RadioConfig cfg;
    cfg.center_hz = app.center_mhz.get() * 1e6;
    cfg.sample_rate_hz = kRatesMsps[size_t(app.rate_index.get())] * 1e6;
    cfg.lna_gain_db = unsigned(app.lna.get());
    cfg.vga_gain_db = unsigned(app.vga.get());
    std::string err;
    if (!app.radio.apply(cfg, &err)) {
        app.status.set(L"配置失败: " + widen(err));
        return;
    }
    app.status.set(L"接收中 " + wd1(app.center_mhz.get()) + L" MHz / " +
                    std::to_wstring(unsigned(cfg.sample_rate_hz / 1e6)) + L" Msps / LNA " +
                    std::to_wstring(cfg.lna_gain_db) + L" / VGA " +
                    std::to_wstring(cfg.vga_gain_db));
}

void toggle_rx(App& app) {
    if (app.running.get()) {
        app.radio.stop_rx();
        app.running.set(false);
        app.status.set(L"已停止");
        return;
    }
    std::string err;
    if (!app.radio.open(&err)) {
        app.status.set(L"打开失败: " + widen(err));
        return;
    }
    apply_radio(app);
    if (!app.radio.start_rx(&rx_trampoline_ui, &app.analyzer, &err)) {
        app.status.set(L"启动接收失败: " + widen(err));
        return;
    }
    app.running.set(true);
}

void export_csv_dialog(App& app) {
    wchar_t path[MAX_PATH] = L"hackrftool-monitor.csv";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"CSV 文件 (*.csv)\0*.csv\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;
    app.status.set(app.monitor.export_csv(path) ? L"已导出: " + std::wstring(path)
                                                : L"导出失败（无法写文件）");
}

// ---- 频谱页（M1） ----------------------------------------------------------

flux::ElementPtr spectrum_page(App& app, const flux::Palette& pal) {
    const bool running = app.running.get();

    flux::Props header_p;
    header_p.direction = flux::Direction::row;
    header_p.align = flux::Align::center;
    header_p.gap = 12.0f;
    auto header = flux::view(std::move(header_p));
    flux::Props title_p;
    title_p.bold = true;
    title_p.text_align = flux::Align::start;
    title_p.flex_grow = 1.0f;
    header->children.push_back(
        flux::label(L"HackRFTool · 2.4G 频谱检测", std::move(title_p)));
    header->children.push_back(flux::ui::badge(
        pal, running ? flux::ui::BadgeKind::success : flux::ui::BadgeKind::neutral,
        running ? L"接收中" : L"已停止"));
    header->children.push_back(flux::ui::badge(pal, flux::ui::BadgeKind::info, L"仅接收"));
    if (app.sweep_on.get() == 1)
        header->children.push_back(
            flux::ui::badge(pal, flux::ui::BadgeKind::warning, L"全频段"));

    // 控制行（WinFlux 的 button() 不带默认底色，须显式配色）
    flux::Props controls_p;
    controls_p.direction = flux::Direction::row;
    controls_p.align = flux::Align::center;
    controls_p.gap = 12.0f;
    auto controls = flux::view(std::move(controls_p));
    flux::Props btn_primary;
    btn_primary.background = pal.accent;
    btn_primary.hover_background = pal.accent_hover;
    btn_primary.text_color = pal.on_accent;
    controls->children.push_back(flux::button(running ? L"停止" : L"开始",
                                              [&app] { toggle_rx(app); },
                                              std::move(btn_primary)));
    controls->children.push_back(flux::ui::field(
        pal, L"模式",
        flux::ui::segmented(pal, {L"单窗", L"全频段"}, app.sweep_on.get(), [&app](int i) {
            app.sweep_on.set(i);
            app.pano.clear_fresh();
            app.hop_ms = 0;   // 立即重跳到段 0
        })));
    if (app.sweep_on.get() == 0) {
        controls->children.push_back(flux::ui::field(
            pal, L"中心频率 MHz",
            flux::text_input(app.freq_text.get(),
                             [&app](std::wstring v) { app.freq_text.set(v); })));
    }
    flux::Props btn_secondary;
    btn_secondary.background = pal.surface;
    btn_secondary.hover_background = pal.surface_hover;
    btn_secondary.text_color = pal.text;
    btn_secondary.border_color = pal.border;
    btn_secondary.border_width = 1.0f;
    if (app.sweep_on.get() == 0) {
        controls->children.push_back(flux::button(L"应用频率", [&app] {
            app.center_mhz.set(
                clamp_center(std::wcstod(app.freq_text.get().c_str(), nullptr)));
            if (app.running.get()) apply_radio(app);
        }, std::move(btn_secondary)));
    }
    controls->children.push_back(flux::ui::field(
        pal, L"LNA " + std::to_wstring(unsigned(app.lna.get())) + L" dB",
        flux::slider(float(app.lna.get()), 8.0f, 40.0f, [&app](float v) {
            app.lna.set(double(unsigned(v / 8.0f + 0.5f) * 8));
            if (app.running.get()) apply_radio(app);
        })));
    controls->children.push_back(flux::ui::field(
        pal, L"VGA " + std::to_wstring(unsigned(app.vga.get())) + L" dB",
        flux::slider(float(app.vga.get()), 2.0f, 62.0f, [&app](float v) {
            app.vga.set(double(unsigned(v / 2.0f + 0.5f) * 2));
            if (app.running.get()) apply_radio(app);
        })));
    controls->children.push_back(flux::ui::field(
        pal, L"采样率 Msps",
        flux::ui::segmented(pal, {L"8", L"10", L"16", L"20"}, app.rate_index.get(),
                            [&app](int i) {
                                app.rate_index.set(i);
                                if (app.running.get()) apply_radio(app);
                            })));

    flux::Props page_p;
    page_p.direction = flux::Direction::column;
    page_p.align = flux::Align::stretch;
    page_p.gap = 8.0f;
    page_p.flex_grow = 1.0f;
    auto page_el = flux::view(std::move(page_p));
    page_el->children.push_back(std::move(header));
    page_el->children.push_back(std::move(controls));
    if (app.sweep_on.get() == 0) {
        const std::vector<hackrftool::ui::SpectrumTick> ticks = {
            {app.center_mhz.get() - 10.0, L"-10M"},
            {app.center_mhz.get(), L"中心"},
            {app.center_mhz.get() + 10.0, L"+10M"},
        };
        page_el->children.push_back(hackrftool::ui::spectrum_view(
            pal, app.frame.db, app.frame.peak, app.center_mhz.get() - 10.0,
            app.center_mhz.get() + 10.0, ticks, app.frame.seq));
        page_el->children.push_back(
            hackrftool::ui::waterfall_view(pal, app.waterfall, app.waterfall.seq()));
    } else {
        const std::vector<hackrftool::ui::SpectrumTick> ticks = {
            {2400.0, L"2400"}, {2420.0, L"2420"}, {2440.0, L"2440"},
            {2460.0, L"2460"}, {2480.0, L"2480"},
        };
        page_el->children.push_back(hackrftool::ui::spectrum_view(
            pal, app.pano.panorama(), {}, kSweepLoMhz, kSweepHiMhz, ticks,
            app.pano.seq()));
        page_el->children.push_back(hackrftool::ui::waterfall_view(
            pal, app.waterfall_sweep, app.waterfall_sweep.seq()));
    }
    return page_el;
}

// ---- 监测页（M2） ----------------------------------------------------------

flux::ElementPtr monitor_page(App& app, const flux::Palette& pal) {
    // 目标选择行
    flux::Props sel_p;
    sel_p.direction = flux::Direction::row;
    sel_p.align = flux::Align::center;
    sel_p.gap = 12.0f;
    auto sel = flux::view(std::move(sel_p));
    sel->children.push_back(flux::ui::field(
        pal, L"自动跟踪最强信号",
        flux::toggle_switch(app.auto_track.get(), [&app](bool on) {
            app.monitor.set_mode(on ? hackrftool::dsp::ChannelMonitor::Mode::auto_peak
                                    : hackrftool::dsp::ChannelMonitor::Mode::fixed_bin);
            app.auto_track.set(on);
        })));
    sel->children.push_back(flux::ui::field(
        pal, L"目标频率 MHz",
        flux::text_input(app.mon_text.get(),
                         [&app](std::wstring v) { app.mon_text.set(v); })));
    flux::Props btn_lock;
    btn_lock.background = pal.surface;
    btn_lock.hover_background = pal.surface_hover;
    btn_lock.text_color = pal.text;
    btn_lock.border_color = pal.border;
    btn_lock.border_width = 1.0f;
    sel->children.push_back(flux::button(L"锁定该频率", [&app] {
        const double mhz =
            clamp_center(std::wcstod(app.mon_text.get().c_str(), nullptr));
        app.monitor.set_fixed_bin(mhz_to_bin(mhz, app.center_mhz.get()));
        app.monitor.set_mode(hackrftool::dsp::ChannelMonitor::Mode::fixed_bin);
        app.auto_track.set(false);
        app.status.set(L"监测已锁定 " + wd1(mhz) + L" MHz（bin " +
                       std::to_wstring(app.monitor.tracked_bin()) + L"）");
    }, std::move(btn_lock)));
    flux::Props btn_reset;
    btn_reset.background = pal.surface;
    btn_reset.hover_background = pal.surface_hover;
    btn_reset.text_color = pal.text;
    btn_reset.border_color = pal.border;
    btn_reset.border_width = 1.0f;
    sel->children.push_back(flux::button(L"恢复自动", [&app] {
        app.monitor.set_mode(hackrftool::dsp::ChannelMonitor::Mode::auto_peak);
        app.auto_track.set(true);
    }, std::move(btn_reset)));

    // 阈值行 + 导出
    flux::Props tools_p;
    tools_p.direction = flux::Direction::row;
    tools_p.align = flux::Align::center;
    tools_p.gap = 12.0f;
    auto tools = flux::view(std::move(tools_p));
    tools->children.push_back(flux::ui::field(
        pal, L"活动阈值 " + wd1(app.threshold.get()) + L" dB",
        flux::slider(float(app.threshold.get()), -100.0f, -40.0f, [&app](float v) {
            app.threshold.set(double(int(v)));
        })));
    flux::Props btn_export;
    btn_export.background = pal.accent;
    btn_export.hover_background = pal.accent_hover;
    btn_export.text_color = pal.on_accent;
    tools->children.push_back(
        flux::button(L"导出 CSV", [&app] { export_csv_dialog(app); },
                     std::move(btn_export)));

    // 统计行
    const auto st = app.monitor.stats(float(app.threshold.get()));
    const std::wstring stats_text =
        (st.count == 0)
            ? L"暂无统计（等待数据）"
            : L"均值 " + wd1(st.mean) + L" dB / 峰值 " + wd1(st.peak) + L" dB / 方差 " +
                  wd1(st.variance) + L" / 占空比 " +
                  std::to_wstring(unsigned(st.duty * 100.0f + 0.5f)) + L"% / 样本 " +
                  std::to_wstring(st.count) + L" / 跟踪 bin " +
                  std::to_wstring(app.monitor.tracked_bin());
    flux::Props stats_p;
    stats_p.text_align = flux::Align::start;
    auto stats_el = flux::ui::caption(pal, stats_text, std::move(stats_p));

    flux::Props page_p;
    page_p.direction = flux::Direction::column;
    page_p.align = flux::Align::stretch;
    page_p.gap = 8.0f;
    page_p.flex_grow = 1.0f;
    auto page_el = flux::view(std::move(page_p));
    page_el->children.push_back(std::move(sel));
    page_el->children.push_back(hackrftool::ui::rssi_strip(
        pal, app.monitor.series(), float(app.threshold.get()), app.frame.seq));
    page_el->children.push_back(std::move(tools));
    page_el->children.push_back(std::move(stats_el));
    return page_el;
}

// ---- 根 -------------------------------------------------------------------

flux::ElementPtr build(App& app) {
    app.host.animate_for(100);   // 10 fps 心跳：驱动持续重绘

    // 全频段扫描：驻留到点 → 跳下一段（UI 心跳驱动，±1 tick 抖动可接受）
    const ULONGLONG now = GetTickCount64();
    const bool running = app.running.get();
    if (running && app.sweep_on.get() == 1 && now - app.hop_ms >= kDwellMs) {
        app.seg_idx = (app.seg_idx + 1) % app.pano.segments();
        app.hop_ms = now;
        hackrftool::radio::RadioConfig cfg;
        cfg.center_hz = seg_center_mhz(app.seg_idx) * 1e6;
        cfg.sample_rate_hz = kRatesMsps[size_t(app.rate_index.get())] * 1e6;
        cfg.lna_gain_db = unsigned(app.lna.get());
        cfg.vga_gain_db = unsigned(app.vga.get());
        (void)app.radio.apply(cfg);   // 跳频失败静默（下个心跳重试）
    }

    // 帧拉取：新帧 → 瀑布/全景 + 信道监测推进
    const auto f = app.analyzer.snapshot();
    if (!f.db.empty()) {
        const bool fresh = f.seq != app.frame.seq;
        if (app.sweep_on.get() == 0) {
            if (fresh) {
                app.waterfall.push(f.db);
                app.monitor.push(f);
            }
        } else if (fresh && now - app.hop_ms > kSettleMs) {
            // 跳频后静默期外的帧才属于当前段
            app.pano.set(app.seg_idx, f.db);
            app.monitor.push(f);
            if (app.pano.complete()) {
                app.waterfall_sweep.push(app.pano.downscaled(320));
                app.pano.clear_fresh();
            }
        }
        app.frame = f;
    }

    const flux::Palette& pal = app.host.palette();

    flux::Props root_p;
    root_p.direction = flux::Direction::column;
    root_p.align = flux::Align::stretch;   // 子元素横向撑满（否则纯 paint 卡片宽度塌 0）
    root_p.gap = 8.0f;
    root_p.flex_grow = 1.0f;
    root_p.background = pal.background;
    root_p.padding = flux::EdgeInsets{16.0f, 16.0f, 16.0f, 16.0f};
    auto root = flux::view(std::move(root_p));
    root->children.push_back(flux::ui::tabs(
        pal, {L"频谱", L"信道监测"}, app.page.get(),
        [&app](int i) { app.page.set(i); }));
    root->children.push_back(app.page.get() == 0 ? spectrum_page(app, pal)
                                                 : monitor_page(app, pal));

    // 状态栏（追加扫描进度与帧号，便于肉眼确认数据在流动）
    flux::Props cap_p;
    cap_p.text_align = flux::Align::start;
    std::wstring status_text = app.status.get();
    if (app.running.get() && app.sweep_on.get() == 1) {
        status_text += L"  ·  扫描 段 " + std::to_wstring(app.seg_idx + 1) + L"/5 @ " +
                       wd1(seg_center_mhz(app.seg_idx)) + L" MHz";
    }
    status_text +=
        (app.frame.db.empty() ? L"" : L"  ·  帧 " + std::to_wstring(app.frame.seq));
    root->children.push_back(flux::ui::caption(pal, status_text, std::move(cap_p)));
    return root;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd_line, int) {
    flux::enable_per_monitor_dpi_v2();

    App app;
    app.running = app.host.make_state<bool>(false);
    app.status = app.host.make_state<std::wstring>(L"未开始（点击「开始」）");
    app.freq_text = app.host.make_state<std::wstring>(L"2450");
    app.center_mhz = app.host.make_state<double>(2450.0);
    app.lna = app.host.make_state<double>(32.0);
    app.vga = app.host.make_state<double>(30.0);
    app.rate_index = app.host.make_state<int>(3);
    app.sweep_on = app.host.make_state<int>(0);
    app.page = app.host.make_state<int>(0);
    app.auto_track = app.host.make_state<bool>(true);
    app.mon_text = app.host.make_state<std::wstring>(L"2450");
    app.threshold = app.host.make_state<double>(-70.0);

    // 命令行：auto = 启动即接收（频谱页）；autom = 接收+监测页；autos = 接收+全频段扫描
    bool auto_start = false;
    if (cmd_line != nullptr) {
        if (wcscmp(cmd_line, L"auto") == 0) auto_start = true;
        if (wcscmp(cmd_line, L"autom") == 0) {
            auto_start = true;
            app.page.set(1);
        }
        if (wcscmp(cmd_line, L"autos") == 0) {
            auto_start = true;
            app.sweep_on.set(1);
        }
    }
    if (auto_start) {
        std::string err;
        if (!app.radio.open(&err)) {
            app.status.set(L"打开失败: " + widen(err));
        } else {
            apply_radio(app);
            if (app.radio.start_rx(&rx_trampoline_ui, &app.analyzer, &err)) {
                app.running.set(true);
            } else {
                app.status.set(L"启动接收失败: " + widen(err));
            }
        }
    }

    app.host.set_root_builder([&app] { return build(app); });

    flux::Host::Config cfg;
    cfg.title = L"HackRFTool";
    cfg.width = 1200;
    cfg.height = 860;
    if (!app.host.create(cfg, instance)) return 1;
    return app.host.run();
}
