// HackRFTool —— 2.4G 频谱检测工具（M1 频谱 + M2 信道监测）
#include <windows.h>

#include <commdlg.h>

#include <algorithm>
#include <complex>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <map>
#include <string>
#include <thread>

#include <flux/flux.hpp>
#include <flux/Components.hpp>

#include "dsp/analyzer.hpp"
#include "dsp/channel_monitor.hpp"
#include "dsp/esb.hpp"
#include "dsp/gfsk.hpp"
#include "dsp/live_bursts.hpp"
#include "dsp/panorama.hpp"
#include "dsp/waterfall.hpp"
#include "radio/hackrf.hpp"
#include "radio/iq_recorder.hpp"
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
    hackrftool::dsp::LiveBursts live;                          // M5：实时突发
    hackrftool::radio::HackRadio radio;
    hackrftool::radio::IqRecorder recorder;                    // M5：UI 内 IQ 录制
    std::map<unsigned long long, std::wstring> row_cache;      // 突发行文本（按 start_sample）

    flux::State<bool> running;
    flux::State<std::wstring> status;
    flux::State<std::wstring> freq_text;   // MHz 文本
    flux::State<double> center_mhz;
    flux::State<double> lna;   // 8..40，步进 8
    flux::State<double> vga;   // 2..62，步进 2
    flux::State<int> rate_index;
    // M4：全频段扫描（跳频在专职后台线程，UI 只读以下原子量）
    flux::State<int> sweep_on;             // 0=单窗 1=全频段
    std::atomic<int> sweep_live{0};        // 1 = 扫描线程正在跳频
    std::atomic<std::size_t> seg_idx{4};   // 从 4 起步，首次跳频即到段 0
    std::atomic<unsigned long long> hop_ms{0};
    // M2：监测页
    flux::State<int> page;                 // 0=频谱 1=信道监测 2=实时抓包
    flux::State<bool> auto_track;          // 自动跟踪最强峰
    flux::State<std::wstring> mon_text;    // 监测目标 MHz 文本
    flux::State<double> threshold;         // 活动阈值 dB
    flux::State<double> burst_thr;         // M5：突发检测阈值 dB
    flux::State<int> symrate_idx;          // M6：解调符号率 0=1M 1=2M
    double mon_lock_mhz = 0.0;             // M6：手动锁定频率（中心变化时重算 bin）
    hackrftool::dsp::SpectrumFrame frame;   // 本帧快照（build 前拉取）
    // M6：自测指标（selftest 模式汇总断言用）
    std::atomic<unsigned> build_count{0};
    std::atomic<unsigned> esb_hits{0};
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
    auto* app = static_cast<App*>(ctx);
    app->analyzer.feed(iq, bytes);
    app->live.write(iq, bytes);
    if (app->recorder.recording()) app->recorder.write(iq, bytes);
}

// 扫描线程：驻留到点改中心频率（绝不碰 UI/绘制线程——在 paint 里同步
// set_freq 会把首帧 build 挂死）
void sweep_loop(App& app) {
    while (app.sweep_live.load() == 1) {
        Sleep(20);
        if (app.sweep_live.load() != 1) break;
        const unsigned long long now = GetTickCount64();
        if (now - app.hop_ms.load() >= kDwellMs) {
            app.seg_idx.store((app.seg_idx.load() + 1) % 5);
            app.hop_ms.store(now);
            (void)app.radio.set_center_hz(seg_center_mhz(app.seg_idx.load()) * 1e6);
        }
    }
}

void set_sweep_live(App& app, bool on) {
    const bool was = app.sweep_live.load() == 1;
    if (on == was) return;
    if (on) {
        app.hop_ms.store(0);
        app.pano.clear_fresh();
        app.sweep_live.store(1);
        std::thread(sweep_loop, std::ref(app)).detach();
    } else {
        app.sweep_live.store(0);
    }
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
    // 频率等实时信息由状态栏按模式动态拼接（M5 清理：不再固化在状态文本里）
    app.status.set(L"接收中");
}

void toggle_rx(App& app) {
    if (app.running.get()) {
        set_sweep_live(app, false);
        if (app.recorder.recording()) app.recorder.stop();
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
    if (!app.radio.start_rx(&rx_trampoline_ui, &app, &err)) {
        app.status.set(L"启动接收失败: " + widen(err));
        return;
    }
    app.running.set(true);
    if (app.sweep_on.get() == 1) set_sweep_live(app, true);
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
            set_sweep_live(app, i == 1 && app.running.get());
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
            // M6：手动锁定监测时按新中心重算 bin
            if (app.mon_lock_mhz > 0.0 && !app.auto_track.get())
                app.monitor.set_fixed_bin(
                    mhz_to_bin(app.mon_lock_mhz, app.center_mhz.get()));
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
        app.mon_lock_mhz = mhz;   // 记住目标频率，中心变化时重算 bin（M6）
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

// ---- 实时抓包页（M5） -------------------------------------------------------

void record_toggle(App& app) {
    if (app.recorder.recording()) {
        app.recorder.stop();
        app.status.set(L"录制完成: " + std::to_wstring(app.recorder.bytes_written()) +
                       L" 字节");
        return;
    }
    wchar_t path[MAX_PATH] = L"hackrftool-iq.cs8";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"IQ 采集 (*.cs8)\0*.cs8\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"cs8";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;
    if (!app.recorder.start(path)) {
        app.status.set(L"无法创建录制文件");
        return;
    }
    app.status.set(L"录制中…");
}

// 突发行文本（懒生成 + 缓存）：时间/时长/峰值 + GFSK 前 8 字节 hex + ESB 帧识别
std::wstring burst_row_text(App& app, const hackrftool::dsp::LiveBurst& b,
                            double sample_rate_hz) {
    if (const auto it = app.row_cache.find(b.start_sample); it != app.row_cache.end())
        return it->second;

    // µs = 样本数 / (fs/1e6)；fs 已折算为 Msps，不能再乘 1e6
    const unsigned long long us = b.samples / unsigned(sample_rate_hz / 1e6 + 0.5);
    wchar_t head[96];
    swprintf(head, 96, L"t=%.3fs  %llu.%02llums  %.1f dB",
             double(b.start_sample) / sample_rate_hz, us / 1000, (us % 1000) / 10,
             b.peak_db);
    std::wstring text = head;

    if (b.samples >= 128) {
        // 解调切片上限 262144 样本（13ms）：增益饱和时"突发"会连成整环
        // （2M 样本），不限长会引发每行 16MB 的分配风暴
        const unsigned long long n_demod =
            std::min<unsigned long long>(b.samples, 262144);
        std::vector<std::int8_t> slice;
        if (app.live.read_slice(b.start_sample, n_demod, slice)) {
            std::vector<std::complex<float>> cmplx(n_demod);
            for (unsigned long long i = 0; i < n_demod; ++i)
                cmplx[static_cast<std::size_t>(i)] = {
                    float(slice[static_cast<std::size_t>(i) * 2]),
                    float(slice[static_cast<std::size_t>(i) * 2 + 1])};
            const hackrftool::dsp::GfskDemod demod(
                sample_rate_hz, app.symrate_idx.get() == 1 ? 2e6 : 1e6, 160e3);
            const auto r = demod.demod(cmplx, 0);
            const std::size_t n_bits = std::min<std::size_t>(r.bits.size(), 64);
            wchar_t hex[64];
            std::size_t hx = 0;
            for (std::size_t byte_i = 0; byte_i < 8; ++byte_i) {
                unsigned v = 0;
                for (std::size_t bit = 0; bit < 8; ++bit) {
                    const std::size_t idx = byte_i * 8 + bit;
                    v = (v << 1) | ((idx < n_bits) ? unsigned(r.bits[idx] & 1u) : 0u);
                }
                hx += swprintf(hex + hx, 32 - hx, L"%02X", v);
            }
            text += L"  |  ";
            text += hex;
            const auto frames = hackrftool::dsp::esb_scan(r.bits);
            if (!frames.empty()) {
                app.esb_hits.fetch_add(unsigned(frames.size()));
                text += L"  ESB✓";
                for (const auto& fr : frames) {
                    text += L" addr:";
                    for (const unsigned char a : fr.address) {
                        wchar_t ab[8];
                        swprintf(ab, 8, L"%02X", a);
                        text += ab;
                    }
                    text += L" len:" + std::to_wstring(fr.payload.size());
                }
            }
        }
    }

    app.row_cache[b.start_sample] = text;
    if (app.row_cache.size() > 800) {   // 限额淘汰最旧
        auto cut = app.row_cache.begin();
        std::advance(cut, 200);
        app.row_cache.erase(app.row_cache.begin(), cut);
    }
    return text;
}

flux::ElementPtr capture_page(App& app, const flux::Palette& pal) {
    const double fs_hz = kRatesMsps[size_t(app.rate_index.get())] * 1e6;

    // 工具行
    flux::Props tools_p;
    tools_p.direction = flux::Direction::row;
    tools_p.align = flux::Align::center;
    tools_p.gap = 12.0f;
    auto tools = flux::view(std::move(tools_p));
    tools->children.push_back(flux::ui::field(
        pal, L"突发阈值 " + wd1(app.burst_thr.get()) + L" dB",
        flux::slider(float(app.burst_thr.get()), -60.0f, -20.0f, [&app](float v) {
            app.burst_thr.set(double(int(v)));
        })));
    tools->children.push_back(flux::ui::field(
        pal, L"解调符号率",
        flux::ui::segmented(pal, {L"1 Mbps", L"2 Mbps"}, app.symrate_idx.get(),
                            [&app](int i) { app.symrate_idx.set(i); })));
    flux::Props btn_clear;
    btn_clear.background = pal.surface;
    btn_clear.hover_background = pal.surface_hover;
    btn_clear.text_color = pal.text;
    btn_clear.border_color = pal.border;
    btn_clear.border_width = 1.0f;
    tools->children.push_back(flux::button(L"清空", [&app] {
        app.live.clear();
        app.row_cache.clear();
    }, std::move(btn_clear)));
    flux::Props btn_rec;
    btn_rec.background = app.recorder.recording() ? pal.danger : pal.accent;
    btn_rec.hover_background = pal.accent_hover;
    btn_rec.text_color = pal.on_accent;
    tools->children.push_back(
        flux::button(app.recorder.recording() ? L"■ 停止录制" : L"● 录制 IQ",
                     [&app] { record_toggle(app); }, std::move(btn_rec)));

    // 概览行
    flux::Props sum_p;
    sum_p.text_align = flux::Align::start;
    auto summary = flux::ui::caption(
        pal, L"突发总数 " + std::to_wstring(app.live.bursts().size()) +
                 L"（新在上；GFSK 比特按 1 Msps 解调；ESB✓ = 识别出 nRF24 兼容帧；"
                 L"若时长都接近 100ms，说明增益过高整段连片，请下调 LNA/VGA）",
        std::move(sum_p));

    // 列表（最新 60 条）：视口必须 flex_grow 占满剩余高度，否则塌 0 不渲染
    flux::Props list_p;
    list_p.direction = flux::Direction::column;
    list_p.gap = 2.0f;
    auto list = flux::view(std::move(list_p));
    const auto& bursts = app.live.bursts();
    const std::size_t n_rows = std::min<std::size_t>(bursts.size(), 60);
    for (std::size_t k = 0; k < n_rows; ++k) {
        const auto& b = bursts[bursts.size() - 1 - k];   // 新→旧
        flux::Props row_p;
        row_p.text_align = flux::Align::start;
        row_p.font_size_pt = 12.0f;
        row_p.text_color = pal.text;
        list->children.push_back(
            flux::label(burst_row_text(app, b, fs_hz), std::move(row_p)));
    }
    flux::Props page_p;
    page_p.direction = flux::Direction::column;
    page_p.align = flux::Align::stretch;
    page_p.gap = 8.0f;
    page_p.flex_grow = 1.0f;
    auto page_el = flux::view(std::move(page_p));
    page_el->children.push_back(std::move(tools));
    page_el->children.push_back(std::move(summary));
    // 视口 flex_grow 占满剩余高度，否则列内视口高度塌 0 导致列表不渲染
    flux::Props scroll_p;
    scroll_p.flex_grow = 1.0f;
    page_el->children.push_back(flux::scroll_view(std::move(list), std::move(scroll_p)));
    return page_el;
}

// ---- 根 -------------------------------------------------------------------

flux::ElementPtr build(App& app) {
    // 心跳续期：每次 build 续 400ms——首帧冷启动（D2D/字体初始化）可能超
    // 过 100ms，窗口太窄会让动画期限在首个 tick 前过期，渲染循环被
    // KillTimer 永久熄火（builds=1 玄学的根因）
    app.host.animate_for(400);
    app.build_count.fetch_add(1);

    // 全频段扫描：先落当前段数据，后跳频（顺序关键！先跳频会让
    // now-hop_ms>settle 永不成立，慢构建下全景永远拿不到数据）
    const ULONGLONG now = GetTickCount64();

    // 帧拉取：新帧 → 瀑布/全景 + 信道监测推进（跳频由后台线程完成，
    // 此处只按原子 seg_idx/hop_ms 打段标签；静默期内的帧属旧频，丢弃）
    const auto f = app.analyzer.snapshot();
    if (!f.db.empty()) {
        const bool fresh = f.seq != app.frame.seq;
        if (app.sweep_on.get() == 0) {
            if (fresh) {
                app.waterfall.push(f.db);
                app.monitor.push(f);
            }
        } else if (fresh && now - app.hop_ms.load() > kSettleMs) {
            app.pano.set(app.seg_idx.load(), f.db);
            app.monitor.push(f);
            if (app.pano.complete()) {
                app.waterfall_sweep.push(app.pano.downscaled(320));
                app.pano.clear_fresh();
            }
        }
        app.frame = f;
    }

    // M5：实时突发检测——必须在页面构建之前跑，否则行文本生成滞后一个
    // 构建周期，突发已被环形缓冲挤出（read_slice 失败 → 永久缓存无 hex）
    if (app.running.get()) app.live.refresh(float(app.burst_thr.get()));

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
        pal, {L"频谱", L"信道监测", L"实时抓包"}, app.page.get(),
        [&app](int i) { app.page.set(i); }));
    switch (app.page.get()) {
    case 1: root->children.push_back(monitor_page(app, pal)); break;
    case 2: root->children.push_back(capture_page(app, pal)); break;
    default: root->children.push_back(spectrum_page(app, pal)); break;
    }

    // 状态栏（M5：按模式动态拼接，实时信息不再固化在 status 文本里）
    flux::Props cap_p;
    cap_p.text_align = flux::Align::start;
    std::wstring status_text = app.status.get();
    if (app.running.get()) {
        status_text = L"接收中 · ";
        status_text += (app.sweep_on.get() == 1)
                           ? L"扫描 段 " + std::to_wstring(app.seg_idx.load() + 1) +
                                 L"/5 @ " + wd1(seg_center_mhz(app.seg_idx.load())) +
                                 L" MHz"
                           : L"中心 " + wd1(app.center_mhz.get()) + L" MHz";
        status_text += L" · " +
                       std::to_wstring(
                           unsigned(kRatesMsps[size_t(app.rate_index.get())])) +
                       L" Msps · LNA " + std::to_wstring(unsigned(app.lna.get())) +
                       L"/VGA " + std::to_wstring(unsigned(app.vga.get()));
        if (!app.frame.db.empty())
            status_text += L" · 帧 " + std::to_wstring(app.frame.seq);
        if (app.recorder.recording())
            status_text += L" · ●录制 " +
                           std::to_wstring(app.recorder.bytes_written() / (1024 * 1024)) +
                           L"MB";
    } else if (!app.frame.db.empty()) {
        status_text += L"  ·  帧 " + std::to_wstring(app.frame.seq);
    }
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
    // 增益默认 16/16：32/30 时时域饱和（频谱分 bin 看不出），突发检测会
    // 把整段信号连成一片（M3 教训，详见 docs/m3、m4 交付记录）
    app.lna = app.host.make_state<double>(16.0);
    app.vga = app.host.make_state<double>(16.0);
    app.rate_index = app.host.make_state<int>(3);
    app.sweep_on = app.host.make_state<int>(0);
    app.page = app.host.make_state<int>(0);
    app.auto_track = app.host.make_state<bool>(true);
    app.mon_text = app.host.make_state<std::wstring>(L"2450");
    app.threshold = app.host.make_state<double>(-70.0);
    app.burst_thr = app.host.make_state<double>(-40.0);
    app.symrate_idx = app.host.make_state<int>(0);

    // 命令行：auto=接收+频谱页；autom=+监测页；autos=+全频段；autocap=+抓包页；
    // selftest/selftestsweep=自测模式（跑 N 秒→断言→写 selftest-report.txt→退出码）
    bool auto_start = false;
    bool selftest = false;
    bool device_ok = false;
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
        if (wcscmp(cmd_line, L"autocap") == 0) {
            auto_start = true;
            app.page.set(2);
        }
        if (wcscmp(cmd_line, L"autosc") == 0) {
            // 压测组合：接收 + 全频段扫描 + 实时抓包页（demod 高负载路径）
            auto_start = true;
            app.sweep_on.set(1);
            app.page.set(2);
        }
        if (wcscmp(cmd_line, L"selftest") == 0) {
            auto_start = true;
            selftest = true;
        }
        if (wcscmp(cmd_line, L"selftestsweep") == 0) {
            auto_start = true;
            selftest = true;
            app.sweep_on.set(1);
        }
    }
    if (auto_start) {
        std::string err;
        if (!app.radio.open(&err)) {
            app.status.set(L"打开失败: " + widen(err));
        } else {
            apply_radio(app);
            if (app.radio.start_rx(&rx_trampoline_ui, &app, &err)) {
                app.running.set(true);
                device_ok = true;
                if (app.sweep_on.get() == 1) set_sweep_live(app, true);
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

    // M6：自测看门狗——收集指标、断言、写报告、发 WM_QUIT
    if (selftest) {
        const DWORD ui_tid = GetCurrentThreadId();
        int seconds = app.sweep_on.get() == 1 ? 8 : 6;
        // 浸润测试：HACKRFTOOL_SOAK=秒数 覆盖默认时长（覆盖"一段时间后才崩"的尺度）
        char soak[16] = {};
        if (GetEnvironmentVariableA("HACKRFTOOL_SOAK", soak, sizeof soak) > 0) {
            const int s = std::atoi(soak);
            if (s > 0 && s <= 900) seconds = s;
        }
        const char* report_path =
            app.sweep_on.get() == 1 ? "selftest-report-sweep.txt" : "selftest-report-single.txt";
        std::thread([&app, ui_tid, seconds, device_ok, report_path] {
            Sleep(500);
            if (app.build_count.load() < 3 && device_ok) {
                // 渲染循环疑似被遮挡停摆：跨线程激活自己窗口（等价用户点任务栏）
                if (HWND h = FindWindowW(nullptr, L"HackRFTool")) {
                    keybd_event(VK_MENU, 0, 0, 0);
                    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
                    ShowWindow(h, SW_RESTORE);
                    SetForegroundWindow(h);
                }
            }
            Sleep(seconds * 1000);
            const unsigned builds = app.build_count.load();
            const unsigned frames = app.frame.seq;
            const unsigned wf = app.waterfall.seq();
            const unsigned wfs = app.waterfall_sweep.seq();
            const unsigned bursts = unsigned(app.live.bursts().size());
            const unsigned esb = app.esb_hits.load();
            const bool hard_ok = device_ok && builds >= 10 && frames >= 50;
            if (std::FILE* rp = std::fopen(report_path, "w")) {
                std::fprintf(rp, "HackRFTool selftest\n模式: %s  时长: %ds\n",
                             app.sweep_on.get() == 1 ? "全频段" : "单窗", seconds);
                std::fprintf(rp, "设备: %s\n", device_ok ? "OK" : "未找到");
                std::fprintf(rp,
                             "builds=%u frames=%u wf_single=%u wf_sweep=%u "
                             "bursts=%u esb_hits=%u\n",
                             builds, frames, wf, wfs, bursts, esb);
                std::fprintf(rp, "硬断言 builds>=10: %s  frames>=50: %s\n",
                             builds >= 10 ? "PASS" : "FAIL",
                             frames >= 50 ? "PASS" : "FAIL");
                std::fprintf(rp, "软指标: bursts=%u wf_sweep轮=%u esb=%u（仅记录）\n",
                             bursts, wfs, esb);
                std::fprintf(rp, "结果: %s\n",
                             !device_ok ? "SKIP（无设备）" : (hard_ok ? "PASS" : "FAIL"));
                std::fclose(rp);
            }
            PostThreadMessage(ui_tid, WM_QUIT, 0, 0);
        }).detach();
    }

    const int exit_code = app.host.run();
    app.sweep_live.store(0);   // 收扫描线程（detach 线程须先令其退出再析构 App）
    Sleep(80);

    if (selftest) {
        // 退出码：无设备 42（CTest SKIP），硬断言失败 1，通过 0
        const char* report_path =
            app.sweep_on.get() == 1 ? "selftest-report-sweep.txt" : "selftest-report-single.txt";
        std::FILE* rp = std::fopen(report_path, "r");
        bool device_ok_read = false, pass = true;
        if (rp != nullptr) {
            char line[256];
            while (std::fgets(line, sizeof line, rp) != nullptr) {
                if (std::strstr(line, "设备: OK") != nullptr) device_ok_read = true;
                if (std::strstr(line, "结果: FAIL") != nullptr) pass = false;
                if (std::strstr(line, "SKIP") != nullptr) device_ok_read = false;
            }
            std::fclose(rp);
        }
        if (!device_ok_read) return 42;
        return pass ? 0 : 1;
    }
    return exit_code;
}
