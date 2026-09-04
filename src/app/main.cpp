// HackRFTool M1 —— 2.4G 频谱检测工具
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <string>

#include <flux/flux.hpp>
#include <flux/Components.hpp>

#include "dsp/analyzer.hpp"
#include "dsp/waterfall.hpp"
#include "radio/hackrf.hpp"
#include "ui/views.hpp"

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

struct App {
    flux::Host host;
    hackrftool::dsp::SpectrumAnalyzer analyzer{512, 64, 256};
    hackrftool::dsp::WaterfallModel waterfall{256, 64};
    hackrftool::radio::HackRadio radio;

    flux::State<bool> running;
    flux::State<std::wstring> status;
    flux::State<std::wstring> freq_text;   // MHz 文本
    flux::State<double> center_mhz;
    flux::State<double> lna;   // 8..40，步进 8
    flux::State<double> vga;   // 2..62，步进 2
    flux::State<int> rate_index;
    hackrftool::dsp::SpectrumFrame frame;   // 本帧快照（build 前拉取）
    unsigned build_count = 0;               // 已弃用：诊断期遗留，M2 清理
};

constexpr double kRatesMsps[4] = {8.0, 10.0, 16.0, 20.0};

double clamp_center(double mhz) noexcept { return std::clamp(mhz, 2400.0, 2483.5); }

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
    app.status.set(L"接收中 " + std::to_wstring(app.center_mhz.get()) + L" MHz / " +
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

flux::ElementPtr build(App& app) {
    app.host.animate_for(100);   // 10 fps 心跳：驱动持续重绘

    // 帧拉取：新帧 → 瀑布推进
    const auto f = app.analyzer.snapshot();
    if (!f.db.empty()) {
        if (f.seq != app.frame.seq) app.waterfall.push(f.db);
        app.frame = f;
    }
    ++app.build_count;

    const flux::Palette& pal = app.host.palette();
    const bool running = app.running.get();

    // 顶栏
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
        pal, L"中心频率 MHz",
        flux::text_input(app.freq_text.get(),
                         [&app](std::wstring v) { app.freq_text.set(v); })));
    flux::Props btn_secondary;
    btn_secondary.background = pal.surface;
    btn_secondary.hover_background = pal.surface_hover;
    btn_secondary.text_color = pal.text;
    btn_secondary.border_color = pal.border;
    btn_secondary.border_width = 1.0f;
    controls->children.push_back(flux::button(L"应用频率", [&app] {
        app.center_mhz.set(
            clamp_center(std::wcstod(app.freq_text.get().c_str(), nullptr)));
        if (app.running.get()) apply_radio(app);
    }, std::move(btn_secondary)));
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

    // 状态栏（追加实时帧号，便于肉眼确认数据在流动）
    flux::Props cap_p;
    cap_p.text_align = flux::Align::start;
    const std::wstring status_text =
        app.status.get() +
        (app.frame.db.empty() ? L"" : L"  ·  帧 " + std::to_wstring(app.frame.seq));
    auto status = flux::ui::caption(pal, status_text, std::move(cap_p));

    flux::Props root_p;
    root_p.direction = flux::Direction::column;
    root_p.align = flux::Align::stretch;   // 子元素横向撑满（否则纯 paint 卡片宽度塌 0）
    root_p.gap = 8.0f;
    root_p.flex_grow = 1.0f;
    root_p.background = pal.background;
    root_p.padding = flux::EdgeInsets{16.0f, 16.0f, 16.0f, 16.0f};
    auto root = flux::view(std::move(root_p));
    root->children.push_back(std::move(header));
    root->children.push_back(std::move(controls));
    root->children.push_back(
        hackrftool::ui::spectrum_view(pal, app.frame, app.center_mhz.get()));
    root->children.push_back(
        hackrftool::ui::waterfall_view(pal, app.waterfall, app.waterfall.seq()));
    root->children.push_back(std::move(status));
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

    // 命令行带 auto 参数：启动即开接收（供自动化截图验证）
    const bool auto_start = (cmd_line != nullptr && wcscmp(cmd_line, L"auto") == 0);
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
