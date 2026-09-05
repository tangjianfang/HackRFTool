// HackRFTool —— 2.4G 频谱检测工具
// 骨架（#52）：原生 Win32 主窗口 = 顶部工具栏（两行：图标按钮行 + 设置行）
// + 中间内容区（WinFlux Host 重父化为子窗口，D2D/DComp 渲染图表）
// + 底部原生状态栏。全部按钮/设置/图标状态在工具栏，内容区纯显示。
#include <windows.h>
#include <dbghelp.h>
#include <minidumpapiset.h>

#include <commctrl.h>
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
#include <vector>

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
#include "ui/status_text.hpp"
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

// 崩溃时自动写 minidump 到 dumps\crash-<pid>.dmp（无需管理员权限）。
// 生成后用 cdb -z dumps\crash-xxx.dmp -c "!analyze -v; k; q" 符号化定位。
// flags 取数据段+间接引用内存（含环形缓冲/波形快照），不全量以免上 GB。
LONG WINAPI write_crash_dump(EXCEPTION_POINTERS* e) {
    CreateDirectoryW(L"dumps", nullptr);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"dumps\\crash-%lu.dmp", GetCurrentProcessId());
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = e;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          MINIDUMP_TYPE(MiniDumpWithDataSegs |
                                        MiniDumpWithIndirectlyReferencedMemory),
                          &mei, nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_CONTINUE_SEARCH;   // 仍走系统默认崩溃流程（事件日志）
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
    std::wstring rec_path;                                     // 当前录制文件（sidecar 用）
    std::map<unsigned long long, std::wstring> row_cache;      // 突发行文本（按 start_sample）
    std::wstring last_esb;              // 最新 ESB 关键帧描述（地址+载荷 hex，横幅用）
    unsigned demod_budget = 0;          // 每 build 解调预算（防突发风暴卡帧）

    // ---- 控件值（#52 起由原生控件持有；内容区每帧重建，普通字段即够） ----
    bool running = false;
    int page = 0;                 // 0=频谱 1=信道监测 2=实时抓包
    int sweep_on = 0;             // 0=单窗 1=全频段
    int rate_index = 3;           // kRatesMsps 下标
    double center_mhz = 2450.0;
    unsigned lna = 16;            // 8..40，步进 8（默认 16/16：32/30 时域饱和，M3 教训）
    unsigned vga = 16;            // 2..62，步进 2
    double threshold = -70.0;     // 活动阈值 dB
    double burst_thr = -40.0;     // 突发检测阈值 dB
    int symrate_idx = 0;          // 0=1 Mbps 1=2 Mbps
    bool auto_track = true;
    double mon_lock_mhz = 0.0;    // 手动锁定频率（中心变化时重算 bin）
    std::wstring status = L"未开始（点击「开始」）";
    hackrftool::dsp::SpectrumFrame frame;   // 本帧快照（build 前拉取）

    // M4：全频段扫描（跳频在专职后台线程，UI 只读以下原子量）
    std::atomic<int> sweep_live{0};        // 1 = 扫描线程正在跳频
    std::atomic<std::size_t> seg_idx{4};   // 从 4 起步，首次跳频即到段 0
    std::atomic<unsigned long long> hop_ms{0};
    // M6：自测指标（selftest 模式汇总断言用）
    std::atomic<unsigned> build_count{0};
    std::atomic<unsigned> esb_hits{0};

    // ---- 原生骨架 HWND（#52） ----
    HWND main_wnd = nullptr;
    HWND toolbar = nullptr;
    HWND statusbar = nullptr;
    HWND edit_freq = nullptr;      // 中心频率（频谱页设置行）
    HWND edit_mon = nullptr;       // 目标频率（监测页设置行）
    HWND combo_rate = nullptr;     // 采样率
    HWND combo_symrate = nullptr;  // 解调符号率（抓包页）
    HWND track_lna = nullptr;
    HWND track_vga = nullptr;
    HWND track_threshold = nullptr;
    HWND track_burst = nullptr;
    HWND check_autotrack = nullptr;
    HWND lbl_lna = nullptr, lbl_vga = nullptr, lbl_thr = nullptr, lbl_burst = nullptr;
    HIMAGELIST images = nullptr;   // 工具栏图标
    HFONT font = nullptr;          // 原生控件消息字体（DPI 缩放）
    // 设置行槽位：创建顺序即摆放顺序；w 为逻辑宽度（layout 时按 DPI 缩放）。
    // drop=true 表示组合框——MoveWindow 高度须含下拉列表（Win32 契约），
    // 否则 CBS_DROPDOWNLIST 只剩 ~0 行可展开。
    struct CtlSlot {
        HWND h;
        int w;
        bool drop = false;
    };
    std::vector<CtlSlot> row_common;    // 所有页可见
    std::vector<CtlSlot> row_monitor;   // 仅监测页
    std::vector<CtlSlot> row_capture;   // 仅抓包页
    // 工具栏状态同步缓存（避免每帧重复 TB_SETSTATE 消息）
    int sync_run = -1, sync_page = -1, sync_sweep = -1, sync_rec = -1;
    std::wstring sync_sb[6];   // 状态栏分段文本缓存（相同文本不重发 SB_SETTEXT）
    DWORD last_sb_ms = 0;     // 状态栏上次文本刷新时刻（4Hz 节流）
    // 拖拽拉伸进行中（WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE）：内容区冻结旧尺寸，
    // 松手一次性重铺——WinFlux DComp 每次收到 WM_SIZE 都销毁重建渲染表面，
    // 拖拽中每步重建=持续闪白（上游行为，本仓库侧绕开）。
    // sizing_enter_ms/last_size_ms：EXITSIZEMOVE 可能丢失（拖拽被打断时），
    // 冻结态会永久卡死（用户实测）——窗口尺寸停变 500ms 即解冻（见看门狗），
    // 连续拖动仍冻结防闪，停顿/丢消息一帧内自愈铺满。
    bool live_sizing = false;
    unsigned long long sizing_enter_ms = 0;
    unsigned long long last_size_ms = 0;
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

// 报告文件落在 exe 旁边而非进程 CWD（CTest/脚本从任意目录拉起时路径确定）
std::wstring exe_dir_path(const char* name) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    const auto slash = dir.find_last_of(L'\\');
    return dir.substr(0, slash + 1) + widen(name);
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

// 从当前控件值读出完整配置
hackrftool::radio::RadioConfig current_radio_cfg(App& app) {
    hackrftool::radio::RadioConfig cfg;
    cfg.center_hz = app.center_mhz * 1e6;
    cfg.sample_rate_hz = kRatesMsps[size_t(app.rate_index)] * 1e6;
    cfg.lna_gain_db = app.lna;
    cfg.vga_gain_db = app.vga;
    return cfg;
}

void apply_radio(App& app) {
    hackrftool::radio::RadioConfig cfg = current_radio_cfg(app);
    std::string err;
    if (!app.radio.apply(cfg, &err)) {
        app.status = L"配置失败: " + widen(err);
        return;
    }
    app.status = L"接收中";
}

// 设备打开失败的统一提示：两大最常见原因直接给用户（多实例占用 / 固件已知
// bug 需软复位——README 2.1）。
void device_open_failed(App& app, const std::string& err) {
    app.status = L"打开失败: " + widen(err) +
                 L" ｜ 排查：① 是否已有 HackRFTool 在运行（独占设备）"
                 L"② 设备重插后先 hackrf_spiflash -R 软复位";
}

void toggle_rx(App& app) {
    if (app.running) {
        set_sweep_live(app, false);
        if (app.recorder.recording()) app.recorder.stop();
        app.radio.stop_rx();
        app.running = false;
        app.status = L"已停止";
        return;
    }
    std::string err;
    if (!app.radio.open(&err)) {
        device_open_failed(app, err);
        return;
    }
    apply_radio(app);
    if (!app.radio.start_rx(&rx_trampoline_ui, &app, &err)) {
        app.status = L"启动接收失败: " + widen(err);
        return;
    }
    app.running = true;
    if (app.sweep_on == 1) set_sweep_live(app, true);
}

// 应用中心频率（工具栏「应用频率」）：读取设置行 EDIT，重算监测锁定 bin
void apply_center_freq(App& app) {
    wchar_t buf[32] = {};
    GetWindowTextW(app.edit_freq, buf, 32);
    const double mhz = clamp_center(std::wcstod(buf, nullptr));
    const bool relock = app.mon_lock_mhz > 0.0 && !app.auto_track;
    if (app.running) {
        hackrftool::radio::RadioConfig cfg = current_radio_cfg(app);
        cfg.center_hz = mhz * 1e6;
        std::string err;
        if (!app.radio.apply(cfg, &err)) app.status = L"配置失败: " + widen(err);
    }
    if (relock) app.monitor.set_fixed_bin(mhz_to_bin(app.mon_lock_mhz, mhz));
    app.center_mhz = mhz;
    SetWindowTextW(app.edit_freq, wd1(mhz).c_str());   // 回写规范化文本
}

void monitor_lock(App& app) {
    wchar_t buf[32] = {};
    GetWindowTextW(app.edit_mon, buf, 32);
    const double mhz = clamp_center(std::wcstod(buf, nullptr));
    app.mon_lock_mhz = mhz;   // 记住目标频率，中心变化时重算 bin（M6）
    app.monitor.set_fixed_bin(mhz_to_bin(mhz, app.center_mhz));
    app.monitor.set_mode(hackrftool::dsp::ChannelMonitor::Mode::fixed_bin);
    app.status = L"监测已锁定 " + wd1(mhz) + L" MHz（bin " +
                 std::to_wstring(app.monitor.tracked_bin()) + L"）";
    app.auto_track = false;
    if (app.check_autotrack != nullptr)
        SendMessageW(app.check_autotrack, BM_SETCHECK, BST_UNCHECKED, 0);
}

void export_csv_dialog(App& app) {
    wchar_t path[MAX_PATH] = L"hackrftool-monitor.csv";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app.main_wnd;
    ofn.lpstrFilter = L"CSV 文件 (*.csv)\0*.csv\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;
    app.status = app.monitor.export_csv(path) ? L"已导出: " + std::wstring(path)
                                              : L"导出失败（无法写文件）";
}

void clear_bursts(App& app) {
    app.live.clear();
    app.row_cache.clear();
    app.esb_hits.store(0);
    app.last_esb.clear();
}

// ---- 内容区三页（纯显示；全部控制在顶部工具栏） ------------------------------

flux::ElementPtr spectrum_display(App& app, const flux::Palette& pal) {
    flux::Props page_p;
    page_p.direction = flux::Direction::column;
    page_p.align = flux::Align::stretch;
    page_p.gap = 8.0f;
    page_p.flex_grow = 1.0f;
    auto page_el = flux::view(std::move(page_p));
    if (app.sweep_on == 0) {
        const std::vector<hackrftool::ui::SpectrumTick> ticks = {
            {app.center_mhz - 10.0, L"-10M"},
            {app.center_mhz, L"中心"},
            {app.center_mhz + 10.0, L"+10M"},
        };
        page_el->children.push_back(hackrftool::ui::spectrum_view(
            pal, app.frame.db, app.frame.peak, app.center_mhz - 10.0,
            app.center_mhz + 10.0, ticks, app.frame.seq));
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

flux::ElementPtr monitor_display(App& app, const flux::Palette& pal) {
    // 统计行
    const auto st = app.monitor.stats(float(app.threshold));
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
    page_el->children.push_back(hackrftool::ui::rssi_strip(
        pal, app.monitor.series(), float(app.threshold), app.frame.seq));
    page_el->children.push_back(std::move(stats_el));
    if (app.sweep_on == 1) {
        // T1.3：扫描模式下监测语义说明（M2 遗留）
        flux::Props note_p;
        note_p.text_align = flux::Align::start;
        page_el->children.push_back(flux::ui::caption(
            pal, L"注：全频段扫描模式下，监测跟踪当前驻留段（5 段约每秒轮换）",
            std::move(note_p)));
    }
    return page_el;
}

void record_toggle(App& app) {
    if (app.recorder.recording()) {
        app.recorder.stop();
        // 参数 sidecar：按停止时配置写 <path>.txt
        hackrftool::radio::SidecarInfo info;
        info.center_hz = app.center_mhz * 1e6;
        info.sample_rate_hz = kRatesMsps[size_t(app.rate_index)] * 1e6;
        info.lna_db = app.lna;
        info.vga_db = app.vga;
        (void)hackrftool::radio::write_capture_sidecar(app.rec_path, info);
        app.status = L"录制完成: " + std::to_wstring(app.recorder.bytes_written()) +
                     L" 字节（参数已写 .txt）";
        return;
    }
    wchar_t path[MAX_PATH] = L"hackrftool-iq.cs8";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app.main_wnd;
    ofn.lpstrFilter = L"IQ 采集 (*.cs8)\0*.cs8\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"cs8";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;
    app.rec_path = path;
    if (!app.recorder.start(path)) {
        app.status = L"无法创建录制文件";
        return;
    }
    app.status = L"录制中…";
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
        // 每 build 解调预算 2 条：突发风暴时（自测实测每秒数十条）逐帧全量
        // 解调会让 build 长时间阻塞、动画掉帧；预算耗尽的行先显示头部，
        // 不写缓存，后续 build 预算恢复后补全 hex/ESB
        if (app.demod_budget == 0) return text;
        --app.demod_budget;
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
                sample_rate_hz, app.symrate_idx == 1 ? 2e6 : 1e6, 160e3);
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
                // 关键信号横幅：最新帧地址 + 全量载荷 hex（实时解析数据显示）
                const auto& fr = frames.front();
                app.last_esb = L"addr:";
                for (const unsigned char a : fr.address) {
                    wchar_t ab[8];
                    swprintf(ab, 8, L"%02X", a);
                    app.last_esb += ab;
                }
                app.last_esb +=
                    L"  载荷[" + std::to_wstring(fr.payload.size()) + L"] " +
                    widen(hackrftool::dsp::hex_dump(fr.payload));
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

flux::ElementPtr capture_display(App& app, const flux::Palette& pal) {
    const double fs_hz = kRatesMsps[size_t(app.rate_index)] * 1e6;

    // 关键信号横幅：ESB 帧计数 + 最新解出帧的地址/载荷实时显示（M5 教训：
    // ESB✓ 埋在 60 行文本里无法一眼识别关键信号）
    flux::Props key_p;
    key_p.direction = flux::Direction::row;
    key_p.align = flux::Align::center;
    key_p.gap = 8.0f;
    auto key = flux::view(std::move(key_p));
    const unsigned esb_total = app.esb_hits.load();
    if (esb_total > 0) {
        key->children.push_back(
            flux::ui::badge(pal, flux::ui::BadgeKind::success, L"ESB 关键信号"));
        flux::Props key_text_p;
        key_text_p.text_align = flux::Align::start;
        key->children.push_back(flux::ui::caption(
            pal, L"共 " + std::to_wstring(esb_total) + L" 帧 · 最新 " + app.last_esb,
            std::move(key_text_p)));
    } else {
        key->children.push_back(
            flux::ui::badge(pal, flux::ui::BadgeKind::neutral, L"未检出 ESB"));
        flux::Props key_text_p;
        key_text_p.text_align = flux::Align::start;
        key->children.push_back(flux::ui::caption(
            pal, L"检测到 nRF24 兼容帧时在此实时显示地址与载荷", std::move(key_text_p)));
    }

    // 概览：计数加粗独立成段，操作提示弱化为次级说明（关键信息不再埋没）
    flux::Props count_p;
    count_p.text_align = flux::Align::start;
    count_p.bold = true;
    auto count_el = flux::label(
        L"突发总数 " + std::to_wstring(app.live.bursts().size()), std::move(count_p));
    flux::Props hint_p;
    hint_p.text_align = flux::Align::start;
    auto hint_el = flux::ui::caption(
        pal, L"新在上；ESB✓ = 识别出 nRF24 兼容帧；若时长都接近 100ms，说明增益过高"
             L"整段连片，请下调 LNA/VGA",
        std::move(hint_p));

    // 列表（最新 60 条）：视口必须 flex_grow 占满剩余高度，否则塌 0 不渲染
    flux::Props list_p;
    list_p.direction = flux::Direction::column;
    list_p.gap = 2.0f;
    auto list = flux::view(std::move(list_p));
    const auto& bursts = app.live.bursts();
    const std::size_t n_rows = std::min<std::size_t>(bursts.size(), 60);
    for (std::size_t k = 0; k < n_rows; ++k) {
        const auto& b = bursts[bursts.size() - 1 - k];   // 新→旧
        const std::wstring row_text = burst_row_text(app, b, fs_hz);
        flux::Props row_p;
        row_p.text_align = flux::Align::start;
        row_p.font_size_pt = 12.0f;
        // 关键信号行（识别出 ESB 帧）绿色加粗，普通行次级色弱化
        const bool key_row = row_text.find(L"ESB✓") != std::wstring::npos;
        row_p.bold = key_row;
        row_p.text_color = key_row ? pal.success : pal.text_secondary;
        list->children.push_back(flux::label(row_text, std::move(row_p)));
    }
    flux::Props page_p;
    page_p.direction = flux::Direction::column;
    page_p.align = flux::Align::stretch;
    page_p.gap = 8.0f;
    page_p.flex_grow = 1.0f;
    auto page_el = flux::view(std::move(page_p));
    page_el->children.push_back(std::move(key));
    page_el->children.push_back(std::move(count_el));
    page_el->children.push_back(std::move(hint_el));
    // 视口 flex_grow 占满剩余高度，否则列内视口高度塌 0 导致列表不渲染
    flux::Props scroll_p;
    scroll_p.flex_grow = 1.0f;
    page_el->children.push_back(flux::scroll_view(std::move(list), std::move(scroll_p)));
    return page_el;
}

// ---- 内容区根（WinFlux，每帧重建） -----------------------------------------

void sync_chrome(App& app);   // 定义在原生骨架节（工具栏态 + 状态栏文本）
void layout(App& app);        // 定义在原生骨架节（几何摆放；看门狗共用）
bool host_target(App& app, RECT* out);   // 内容区目标矩形

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
        if (app.sweep_on == 0) {
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
    if (app.running) app.live.refresh(float(app.burst_thr));
    app.demod_budget = 2;   // 本帧解调配额（build 内突发行文本生成消耗）

    const flux::Palette& pal = app.host.palette();

    // 原生骨架状态同步（工具栏按钮态 + 状态栏分段文本；心跳驱动，零定时器）
    sync_chrome(app);

    // 几何看门狗：不信任任何消息配对（EXITSIZEMOVE/WM_SIZE 均可能丢失——
    // 用户实测拖拽后卡死在小尺寸），每心跳核对内容区实际/目标矩形，
    // 漂移立即 layout 校正，一帧内自愈。拖拽停顿 500ms 亦在此解冻。
    if (app.live_sizing) {
        if (GetTickCount64() - app.last_size_ms >= 500) app.live_sizing = false;
    }
    if (!app.live_sizing) {
        RECT want, have;
        if (host_target(app, &want)) {
            GetWindowRect(app.host.hwnd(), &have);
            POINT tl{have.left, have.top};
            ScreenToClient(app.main_wnd, &tl);
            if (tl.x != want.left || tl.y != want.top ||
                (have.right - have.left) != (want.right - want.left) ||
                (have.bottom - have.top) != (want.bottom - want.top))
                layout(app);
        }
    }

    flux::Props content_p;
    content_p.direction = flux::Direction::column;
    content_p.align = flux::Align::stretch;   // 子元素横向撑满（否则纯 paint 卡片宽度塌 0）
    content_p.flex_grow = 1.0f;
    content_p.background = pal.background;
    content_p.padding = flux::EdgeInsets{12.0f, 12.0f, 12.0f, 12.0f};
    auto content = flux::view(std::move(content_p));
    switch (app.page) {
    case 1: content->children.push_back(monitor_display(app, pal)); break;
    case 2: content->children.push_back(capture_display(app, pal)); break;
    default: content->children.push_back(spectrum_display(app, pal)); break;
    }
    return content;
}

// ---- 原生 Win32 骨架：工具栏 / 设置行 / 状态栏 -------------------------------

// 命令/控件 ID
enum : int {
    IDC_STARTSTOP = 100,
    IDC_PAGE0,
    IDC_PAGE1,
    IDC_PAGE2,
    IDC_SWEEP,
    IDC_RECORD,
    IDC_LOCK,
    IDC_EXPORT,
    IDC_CLEAR,
    IDC_APPLYFREQ,
    IDC_EDIT_FREQ,
    IDC_EDIT_MON,
    IDC_COMBO_RATE,
    IDC_COMBO_SYM,
    IDC_TRACK_LNA,
    IDC_TRACK_VGA,
    IDC_TRACK_THR,
    IDC_TRACK_BURST,
    IDC_CHECK_AUTOTRACK,
};

constexpr int kIconSize = 24;

// 32bpp DIB 图标：洋红 → 透明（GDI 不写 alpha，逐像素换算）
HBITMAP make_icon(void (*paint)(HDC)) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kIconSize;
    bi.bmiHeader.biHeight = -kIconSize;   // 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp == nullptr || bits == nullptr) return bmp;
    HDC dc = CreateCompatibleDC(nullptr);
    const HGDIOBJ old = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(255, 0, 255));
    RECT rc{0, 0, kIconSize, kIconSize};
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    paint(dc);
    SelectObject(dc, old);
    DeleteDC(dc);
    auto* px = static_cast<DWORD*>(bits);
    for (int i = 0; i < kIconSize * kIconSize; ++i) {
        const COLORREF c = px[i] & 0x00FFFFFF;
        const bool key = (c & 0x00FFFFFF) == 0x00FF00FF;
        px[i] = (key ? 0u : 0xFF000000u) | c;
    }
    return bmp;
}

namespace icon {

void play(HDC dc) {
    HBRUSH b = CreateSolidBrush(RGB(46, 160, 67));
    POINT pts[3] = {{8, 6}, {8, 18}, {18, 12}};
    const HGDIOBJ old = SelectObject(dc, b);
    Polygon(dc, pts, 3);
    SelectObject(dc, old);
    DeleteObject(b);
}

void stop(HDC dc) {
    HBRUSH b = CreateSolidBrush(RGB(235, 72, 48));
    RECT rc{7, 7, 17, 17};
    FillRect(dc, &rc, b);
    DeleteObject(b);
}

void page_spectrum(HDC dc) {
    HPEN p = CreatePen(PS_SOLID, 2, RGB(60, 120, 200));
    const HGDIOBJ old = SelectObject(dc, p);
    POINT pts[6] = {{4, 13}, {7, 8}, {10, 15}, {13, 7}, {16, 13}, {20, 10}};
    Polyline(dc, pts, 6);
    SelectObject(dc, old);
    DeleteObject(p);
}

void page_monitor(HDC dc) {
    HPEN p = CreatePen(PS_SOLID, 2, RGB(160, 120, 40));
    const HGDIOBJ old = SelectObject(dc, p);
    POINT pts[7] = {{4, 16}, {8, 16}, {10, 6}, {13, 19}, {15, 16}, {18, 16}, {20, 16}};
    Polyline(dc, pts, 7);
    SelectObject(dc, old);
    DeleteObject(p);
}

void page_capture(HDC dc) {
    HPEN p = CreatePen(PS_SOLID, 2, RGB(120, 90, 170));
    const HGDIOBJ old = SelectObject(dc, p);
    MoveToEx(dc, 6, 7, nullptr);
    LineTo(dc, 6, 17);
    MoveToEx(dc, 18, 7, nullptr);
    LineTo(dc, 18, 17);
    for (int y = 9; y <= 15; y += 3) {
        MoveToEx(dc, 9, y, nullptr);
        LineTo(dc, 15, y);
    }
    SelectObject(dc, old);
    DeleteObject(p);
}

void sweep(HDC dc) {
    HPEN p = CreatePen(PS_SOLID, 2, RGB(200, 120, 30));
    const HGDIOBJ old = SelectObject(dc, p);
    Ellipse(dc, 5, 5, 19, 19);
    MoveToEx(dc, 12, 12, nullptr);
    LineTo(dc, 18, 7);
    SelectObject(dc, old);
    DeleteObject(p);
}

void record(HDC dc) {
    HPEN p = CreatePen(PS_SOLID, 2, RGB(120, 120, 120));
    const HGDIOBJ old = SelectObject(dc, p);
    Ellipse(dc, 7, 7, 17, 17);
    SelectObject(dc, old);
    DeleteObject(p);
}

void record_on(HDC dc) {
    HBRUSH b = CreateSolidBrush(RGB(220, 40, 40));
    const HGDIOBJ old = SelectObject(dc, b);
    Ellipse(dc, 7, 7, 17, 17);
    SelectObject(dc, old);
    DeleteObject(b);
}

void lock(HDC dc) {
    HPEN p = CreatePen(PS_SOLID, 2, RGB(90, 90, 110));
    const HGDIOBJ old = SelectObject(dc, p);
    Arc(dc, 8, 3, 16, 12, 8, 8, 16, 8);
    SelectObject(dc, old);
    DeleteObject(p);
    HBRUSH b = CreateSolidBrush(RGB(90, 90, 110));
    RECT rc{7, 11, 17, 19};
    FillRect(dc, &rc, b);
    DeleteObject(b);
}

void export_csv(HDC dc) {
    HPEN p = CreatePen(PS_SOLID, 2, RGB(40, 130, 110));
    const HGDIOBJ old = SelectObject(dc, p);
    Rectangle(dc, 5, 10, 14, 19);
    MoveToEx(dc, 12, 14, nullptr);
    LineTo(dc, 20, 6);
    MoveToEx(dc, 20, 6, nullptr);
    LineTo(dc, 16, 6);
    MoveToEx(dc, 20, 6, nullptr);
    LineTo(dc, 20, 10);
    SelectObject(dc, old);
    DeleteObject(p);
}

void clear(HDC dc) {
    HPEN p = CreatePen(PS_SOLID, 2, RGB(170, 70, 70));
    const HGDIOBJ old = SelectObject(dc, p);
    MoveToEx(dc, 7, 7, nullptr);
    LineTo(dc, 17, 17);
    MoveToEx(dc, 17, 7, nullptr);
    LineTo(dc, 7, 17);
    SelectObject(dc, old);
    DeleteObject(p);
}

void apply_freq(HDC dc) {
    HPEN p = CreatePen(PS_SOLID, 2, RGB(46, 160, 67));
    const HGDIOBJ old = SelectObject(dc, p);
    POINT pts[3] = {{6, 13}, {10, 17}, {18, 7}};
    Polyline(dc, pts, 3);
    SelectObject(dc, old);
    DeleteObject(p);
}

} // namespace icon

// 图标索引表（工具栏位图号 = 此处顺序）
enum : int {
    ICON_PLAY = 0,
    ICON_STOP,
    ICON_PAGE0,
    ICON_PAGE1,
    ICON_PAGE2,
    ICON_SWEEP,
    ICON_REC,
    ICON_REC_ON,
    ICON_LOCK,
    ICON_EXPORT,
    ICON_CLEAR,
    ICON_APPLY,
    ICON_COUNT,
};

void create_native_font(App& app) {
    const UINT dpi = app.main_wnd != nullptr ? GetDpiForWindow(app.main_wnd)
                                             : GetDpiForSystem();
    NONCLIENTMETRICSW nm{};
    nm.cbSize = sizeof(nm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(nm), &nm, 0);
    nm.lfMessageFont.lfHeight =
        -MulDiv(9, int(dpi), 96);   // 9pt 消息字体，DPI 缩放
    if (app.font != nullptr) DeleteObject(app.font);
    app.font = CreateFontIndirectW(&nm.lfMessageFont);
}

HWND make_ctl(App& app, const wchar_t* cls, const wchar_t* text, DWORD style,
              DWORD ex_style, int id) {
    // WS_VISIBLE 缺省打开：通用行控件恒显，页上下文控件由 layout 显隐
    HWND h = CreateWindowExW(ex_style, cls, text,
                             WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             app.main_wnd, reinterpret_cast<HMENU>(INT_PTR(id)),
                             GetModuleHandleW(nullptr), nullptr);
    if (h != nullptr && app.font != nullptr) SendMessageW(h, WM_SETFONT,
                                                          WPARAM(app.font), TRUE);
    return h;
}

TBBUTTON tb_btn(int bmp, int cmd, BYTE style, const wchar_t* text) {
    TBBUTTON b{};
    b.iBitmap = bmp;
    b.idCommand = cmd;
    b.fsState = TBSTATE_ENABLED;
    b.fsStyle = style;
    b.iString = reinterpret_cast<INT_PTR>(text);
    return b;
}

void create_toolbar(App& app) {
    app.images = ImageList_Create(kIconSize, kIconSize, ILC_COLOR32, ICON_COUNT, 0);
    void (*painters[])(HDC) = {
        icon::play,        icon::stop,         icon::page_spectrum, icon::page_monitor,
        icon::page_capture, icon::sweep,       icon::record,        icon::record_on,
        icon::lock,        icon::export_csv,   icon::clear,         icon::apply_freq,
    };
    static_assert(sizeof(painters) / sizeof(painters[0]) == ICON_COUNT,
                  "图标表与枚举不一致");
    for (const auto p : painters) {
        HBITMAP bmp = make_icon(p);
        ImageList_Add(app.images, bmp, nullptr);
        DeleteObject(bmp);
    }

    app.toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
                                  // 不用 TBSTYLE_FLAT：透明工具栏不画自己的
                                  // 背景、靠父窗口补画——我们的父窗不擦背景
                                  //（反闪烁），热跟踪重绘时补画缺失=黑底
                                  //（用户实测鼠标划过变黑，像素采样 86% dark）
                                  WS_CHILD | WS_VISIBLE | TBSTYLE_TOOLTIPS |
                                      CCS_NODIVIDER | CCS_NOPARENTALIGN,
                                  0, 0, 0, 0, app.main_wnd, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    SendMessageW(app.toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(app.toolbar, TB_SETBITMAPSIZE, 0,
                 MAKELPARAM(kIconSize, kIconSize));
    SendMessageW(app.toolbar, TB_SETIMAGELIST, 0, LPARAM(app.images));
    SendMessageW(app.toolbar, TB_SETPADDING, 0, MAKELPARAM(10, 12));

    const TBBUTTON btns[] = {
        tb_btn(ICON_PLAY, IDC_STARTSTOP, BTNS_AUTOSIZE, L"开始"),
        tb_btn(0, 0, BTNS_SEP, nullptr),
        tb_btn(ICON_PAGE0, IDC_PAGE0, BTNS_CHECK | BTNS_GROUP | BTNS_AUTOSIZE, L"频谱"),
        tb_btn(ICON_PAGE1, IDC_PAGE1, BTNS_CHECK | BTNS_GROUP | BTNS_AUTOSIZE, L"监测"),
        tb_btn(ICON_PAGE2, IDC_PAGE2, BTNS_CHECK | BTNS_GROUP | BTNS_AUTOSIZE, L"抓包"),
        tb_btn(0, 0, BTNS_SEP, nullptr),
        tb_btn(ICON_SWEEP, IDC_SWEEP, BTNS_CHECK | BTNS_AUTOSIZE, L"全频段"),
        tb_btn(ICON_REC, IDC_RECORD, BTNS_CHECK | BTNS_AUTOSIZE, L"录制 IQ"),
        tb_btn(0, 0, BTNS_SEP, nullptr),
        tb_btn(ICON_LOCK, IDC_LOCK, BTNS_AUTOSIZE, L"锁定"),
        tb_btn(ICON_EXPORT, IDC_EXPORT, BTNS_AUTOSIZE, L"导出 CSV"),
        tb_btn(ICON_CLEAR, IDC_CLEAR, BTNS_AUTOSIZE, L"清空"),
        tb_btn(ICON_APPLY, IDC_APPLYFREQ, BTNS_AUTOSIZE, L"应用频率"),
    };
    SendMessageW(app.toolbar, TB_ADDBUTTONS, WPARAM(sizeof(btns) / sizeof(btns[0])),
                 LPARAM(btns));
    SendMessageW(app.toolbar, TB_AUTOSIZE, 0, 0);
}

void create_settings_row(App& app) {
    auto slot = [](std::vector<App::CtlSlot>& v, HWND h, int w, bool drop = false) {
        v.push_back({h, w, drop});
    };

    // 通用：中心频率 / 采样率 / LNA / VGA
    slot(app.row_common,
         make_ctl(app, WC_STATICW, L"中心频率", SS_LEFT | SS_CENTERIMAGE, 0, 0), 52);
    app.edit_freq = make_ctl(app, WC_EDITW, L"2450",
                             ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 0,
                             IDC_EDIT_FREQ);
    slot(app.row_common, app.edit_freq, 80);
    slot(app.row_common,
         make_ctl(app, WC_STATICW, L"采样率", SS_LEFT | SS_CENTERIMAGE, 0, 0), 52);
    app.combo_rate = make_ctl(app, WC_COMBOBOXW, nullptr,
                              CBS_DROPDOWNLIST | WS_TABSTOP, 0, IDC_COMBO_RATE);
    for (const wchar_t* r : {L"8", L"10", L"16", L"20"})
        SendMessageW(app.combo_rate, CB_ADDSTRING, 0, LPARAM(r));
    SendMessageW(app.combo_rate, CB_SETCURSEL, WPARAM(app.rate_index), 0);
    slot(app.row_common, app.combo_rate, 64, true);

    app.track_lna = make_ctl(app, TRACKBAR_CLASSW, nullptr,
                             TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 0, IDC_TRACK_LNA);
    SendMessageW(app.track_lna, TBM_SETRANGE, TRUE, MAKELPARAM(8, 40));
    SendMessageW(app.track_lna, TBM_SETTICFREQ, 8, 0);
    SendMessageW(app.track_lna, TBM_SETPOS, TRUE, LPARAM(app.lna));
    slot(app.row_common, app.track_lna, 128);
    app.lbl_lna = make_ctl(app, WC_STATICW, L"LNA 16", SS_LEFT | SS_CENTERIMAGE, 0, 0);
    slot(app.row_common, app.lbl_lna, 52);

    app.track_vga = make_ctl(app, TRACKBAR_CLASSW, nullptr,
                             TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 0, IDC_TRACK_VGA);
    SendMessageW(app.track_vga, TBM_SETRANGE, TRUE, MAKELPARAM(2, 62));
    SendMessageW(app.track_vga, TBM_SETTICFREQ, 2, 0);
    SendMessageW(app.track_vga, TBM_SETPOS, TRUE, LPARAM(app.vga));
    slot(app.row_common, app.track_vga, 128);
    app.lbl_vga = make_ctl(app, WC_STATICW, L"VGA 16", SS_LEFT | SS_CENTERIMAGE, 0, 0);
    slot(app.row_common, app.lbl_vga, 52);

    // 监测页：自动跟踪 / 目标频率 / 活动阈值
    app.check_autotrack = make_ctl(app, WC_BUTTONW, L"自动跟踪最强",
                                   BS_AUTOCHECKBOX | WS_TABSTOP, 0,
                                   IDC_CHECK_AUTOTRACK);
    SendMessageW(app.check_autotrack, BM_SETCHECK, BST_CHECKED, 0);
    slot(app.row_monitor, app.check_autotrack, 96);
    slot(app.row_monitor,
         make_ctl(app, WC_STATICW, L"目标频率", SS_LEFT | SS_CENTERIMAGE, 0, 0), 56);
    app.edit_mon = make_ctl(app, WC_EDITW, L"2450",
                            ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 0, IDC_EDIT_MON);
    slot(app.row_monitor, app.edit_mon, 80);
    slot(app.row_monitor,
         make_ctl(app, WC_STATICW, L"活动阈值", SS_LEFT | SS_CENTERIMAGE, 0, 0), 60);
    app.track_threshold = make_ctl(app, TRACKBAR_CLASSW, nullptr,
                                   TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 0,
                                   IDC_TRACK_THR);
    SendMessageW(app.track_threshold, TBM_SETRANGE, TRUE, MAKELPARAM(-100, -40));
    SendMessageW(app.track_threshold, TBM_SETTICFREQ, 10, 0);
    SendMessageW(app.track_threshold, TBM_SETPOS, TRUE, LPARAM(int(app.threshold)));
    slot(app.row_monitor, app.track_threshold, 120);
    app.lbl_thr = make_ctl(app, WC_STATICW, L"-70 dB", SS_LEFT | SS_CENTERIMAGE, 0, 0);
    slot(app.row_monitor, app.lbl_thr, 44);

    // 抓包页：突发阈值 / 符号率
    slot(app.row_capture,
         make_ctl(app, WC_STATICW, L"突发阈值", SS_LEFT | SS_CENTERIMAGE, 0, 0), 60);
    app.track_burst = make_ctl(app, TRACKBAR_CLASSW, nullptr,
                               TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 0,
                               IDC_TRACK_BURST);
    SendMessageW(app.track_burst, TBM_SETRANGE, TRUE, MAKELPARAM(-60, -20));
    SendMessageW(app.track_burst, TBM_SETTICFREQ, 10, 0);
    SendMessageW(app.track_burst, TBM_SETPOS, TRUE, LPARAM(int(app.burst_thr)));
    slot(app.row_capture, app.track_burst, 120);
    app.lbl_burst = make_ctl(app, WC_STATICW, L"-40 dB", SS_LEFT | SS_CENTERIMAGE, 0, 0);
    slot(app.row_capture, app.lbl_burst, 44);
    slot(app.row_capture,
         make_ctl(app, WC_STATICW, L"符号率", SS_LEFT | SS_CENTERIMAGE, 0, 0), 52);
    app.combo_symrate = make_ctl(app, WC_COMBOBOXW, nullptr,
                                 CBS_DROPDOWNLIST | WS_TABSTOP, 0, IDC_COMBO_SYM);
    for (const wchar_t* r : {L"1 Mbps", L"2 Mbps"})
        SendMessageW(app.combo_symrate, CB_ADDSTRING, 0, LPARAM(r));
    SendMessageW(app.combo_symrate, CB_SETCURSEL, WPARAM(app.symrate_idx), 0);
    slot(app.row_capture, app.combo_symrate, 80, true);
}

void create_statusbar(App& app) {
    app.statusbar = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
                                    WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0,
                                    0, app.main_wnd, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
}

// 内容区目标矩形（layout 与几何看门狗共用——判定与摆放必须同一算法）
bool host_target(App& app, RECT* out) {
    if (app.main_wnd == nullptr || app.toolbar == nullptr ||
        app.statusbar == nullptr || app.host.hwnd() == nullptr)
        return false;
    RECT rc, tb, sb;
    GetClientRect(app.main_wnd, &rc);
    GetWindowRect(app.toolbar, &tb);
    GetWindowRect(app.statusbar, &sb);
    const int s = int(GetDpiForWindow(app.main_wnd)) / 96;
    const int row_h = 34 * s;
    out->left = 0;
    out->top = (tb.bottom - tb.top) + row_h;
    out->right = rc.right;
    out->bottom = std::max(rc.bottom - (sb.bottom - sb.top), out->top);
    return true;
}

// 摆放工具栏/设置行/状态栏/内容区（WM_SIZE、WM_DPICHANGED、页切换共用）。
// 拖拽进行中（live_sizing）冻结全部子控件几何（防闪烁，见 App 注释），
// 但超过 10s 视为 EXITSIZEMOVE 丢失，强制解冻——卡死比闪烁更不可接受。
void layout(App& app) {
    if (app.main_wnd == nullptr || app.toolbar == nullptr) return;
    if (app.live_sizing) {
        // 拖拽中窗口尺寸停变 500ms（停手未松或 EXITSIZEMOVE 丢失）即解冻：
        // 卡死不可接受，停顿铺满一次也无闪（一次性重绘）
        if (GetTickCount64() - app.last_size_ms < 500) return;
        app.live_sizing = false;
    }
    const int scale = app.main_wnd != nullptr ? GetDpiForWindow(app.main_wnd) : 96;
    const int s = scale / 96;   // 整数倍缩放足够（100%/125%→1，150%→1 近似；
                                // 控件尺寸不追求逐 DPI 精确，布局稳定优先）
    RECT rc;
    GetClientRect(app.main_wnd, &rc);
    const int w = rc.right, h = rc.bottom;

    // 工具栏
    SendMessageW(app.toolbar, WM_SIZE, 0, 0);
    RECT tb{};
    GetWindowRect(app.toolbar, &tb);
    const int tb_h = tb.bottom - tb.top;
    MoveWindow(app.toolbar, 0, 0, w, tb_h, TRUE);

    // 设置行（单行，创建顺序摆放；页上下文控件按当前页显隐）。
    // bRepaint=FALSE：拉伸时每步 WM_SIZE 重摆全部控件，带重画会连闪；
    // 控件自身收到 WM_PAINT 补画即可。
    const int ctl_h = 24 * s;
    int x = 8 * s;
    const int row_y = tb_h + (34 * s - ctl_h) / 2;
    const auto place = [&](std::vector<App::CtlSlot>& v) {
        for (const auto& c : v) {
            const int ch = c.drop ? 130 * s : ctl_h;   // 组合框高度含下拉列表
            MoveWindow(c.h, x, row_y, c.w * s, ch, FALSE);
            x += c.w * s + 6 * s;
        }
    };
    place(app.row_common);
    const bool mon = app.page == 1, cap = app.page == 2;
    for (const auto& c : app.row_monitor)
        ShowWindow(c.h, mon ? SW_SHOW : SW_HIDE);
    for (const auto& c : app.row_capture)
        ShowWindow(c.h, cap ? SW_SHOW : SW_HIDE);
    if (mon) place(app.row_monitor);
    if (cap) place(app.row_capture);

    // 状态栏（先自适应高，再贴底 + 分段右缘）
    SendMessageW(app.statusbar, WM_SIZE, 0, 0);
    RECT sb{};
    GetWindowRect(app.statusbar, &sb);
    const int sb_h = sb.bottom - sb.top;
    MoveWindow(app.statusbar, 0, h - sb_h, w, sb_h, TRUE);
    int p_esb = 72 * s, p_rec = 88 * s, p_frame = 72 * s, p_rg = 170 * s,
        p_freq = 170 * s;
    int edges[6];
    edges[5] = w;
    edges[4] = w - p_esb;
    edges[3] = edges[4] - p_rec;
    edges[2] = edges[3] - p_frame;
    edges[1] = edges[2] - p_rg;
    edges[0] = edges[1] - p_freq;
    for (int i = 0; i < 5; ++i)
        if (edges[i] < 40 * s) edges[i] = 40 * s + i * 4;   // 极窄窗口防倒挂
    for (int i = 1; i < 6; ++i)
        if (edges[i] <= edges[i - 1]) edges[i] = edges[i - 1] + 4;
    SendMessageW(app.statusbar, SB_SETPARTS, 6, LPARAM(edges));

    // 内容区（WinFlux 子窗口）填满剩余（目标矩形与看门狗共用同一算法）
    RECT ht;
    if (host_target(app, &ht))
        MoveWindow(app.host.hwnd(), ht.left, ht.top, ht.right - ht.left,
                   ht.bottom - ht.top, FALSE);
}

// 工具栏按钮态 + 状态栏文本（build 心跳驱动；缓存避免每帧消息风暴）。
// 状态栏文本 4Hz 节流（帧号每帧都变，40Hz 重绘无意义且是拖拽外的高频
// 重绘源）；拖拽进行中整体跳过（与 layout 冻结配套，拖拽期零子窗重绘）。
void sync_chrome(App& app) {
    if (app.live_sizing) return;
    const DWORD now = GetTickCount();
    const bool sb_due = now - app.last_sb_ms >= 250;
    if (app.toolbar != nullptr) {
        if (app.sync_run != int(app.running)) {
            TBBUTTONINFO bi{};
            bi.cbSize = sizeof(bi);
            bi.dwMask = TBIF_TEXT | TBIF_IMAGE;
            bi.pszText = const_cast<LPWSTR>(app.running ? L"停止" : L"开始");
            bi.iImage = app.running ? ICON_STOP : ICON_PLAY;
            SendMessageW(app.toolbar, TB_SETBUTTONINFO, IDC_STARTSTOP,
                         LPARAM(&bi));
            app.sync_run = int(app.running);
        }
        if (app.sync_page != app.page) {
            const int ids[3] = {IDC_PAGE0, IDC_PAGE1, IDC_PAGE2};
            for (int i = 0; i < 3; ++i)
                SendMessageW(app.toolbar, TB_SETSTATE, ids[i],
                             LPARAM(TBSTATE_ENABLED |
                                    (app.page == i ? TBSTATE_CHECKED : 0)));
            app.sync_page = app.page;
        }
        if (app.sync_sweep != app.sweep_on) {
            SendMessageW(app.toolbar, TB_SETSTATE, IDC_SWEEP,
                         LPARAM(TBSTATE_ENABLED |
                                (app.sweep_on == 1 ? TBSTATE_CHECKED : 0)));
            app.sync_sweep = app.sweep_on;
        }
        const bool rec = app.recorder.recording();
        if (app.sync_rec != int(rec)) {
            TBBUTTONINFO bi{};
            bi.cbSize = sizeof(bi);
            bi.dwMask = TBIF_TEXT | TBIF_IMAGE | TBIF_STATE;
            bi.fsState = TBSTATE_ENABLED | (rec ? TBSTATE_CHECKED : 0);
            bi.pszText = const_cast<LPWSTR>(rec ? L"■ 停录" : L"● 录制 IQ");
            bi.iImage = rec ? ICON_REC_ON : ICON_REC;
            SendMessageW(app.toolbar, TB_SETBUTTONINFO, IDC_RECORD, LPARAM(&bi));
            app.sync_rec = int(rec);
        }
    }

    if (app.statusbar != nullptr && sb_due) {
        app.last_sb_ms = now;
        // 分段 0：基础状态（接收中时由动态段首词接续，避免与分段 1 重复）
        std::wstring parts[6];
        parts[0] = app.running ? L"接收中" : app.status;
        hackrftool::ui::StatusInfo si;
        si.running = app.running;
        si.sweep = app.sweep_on == 1;
        si.recording = app.recorder.recording();
        si.seg_idx = int(app.seg_idx.load());
        si.seg_center_mhz = seg_center_mhz(app.seg_idx.load());
        si.center_mhz = app.center_mhz;
        si.rate_msps = unsigned(kRatesMsps[size_t(app.rate_index)]);
        si.lna_db = app.lna;
        si.vga_db = app.vga;
        si.has_frame = !app.frame.db.empty();
        si.frame_seq = app.frame.seq;
        si.rec_bytes = app.recorder.bytes_written();
        si.esb_hits = app.esb_hits.load();
        hackrftool::ui::status_parts(si, parts + 1);
        for (int i = 0; i < 6; ++i) {
            if (parts[i] != app.sync_sb[i]) {   // 文本未变不重发（防重绘叠加闪烁）
                app.sync_sb[i] = parts[i];
                SendMessageW(app.statusbar, SB_SETTEXT, i, LPARAM(parts[i].c_str()));
            }
        }
    }
}

// ---- 命令路由 ----------------------------------------------------------------

void on_command(App& app, int id, int code, HWND from) {
    switch (id) {
    case IDC_STARTSTOP: toggle_rx(app); break;
    case IDC_PAGE0: app.page = 0; layout(app); break;
    case IDC_PAGE1: app.page = 1; layout(app); break;
    case IDC_PAGE2: app.page = 2; layout(app); break;
    case IDC_SWEEP: {
        const LRESULT st = SendMessageW(app.toolbar, TB_GETSTATE, IDC_SWEEP, 0);
        const bool on = (st & TBSTATE_CHECKED) != 0;
        set_sweep_live(app, on && app.running);
        app.sweep_on = on ? 1 : 0;
        break;
    }
    case IDC_RECORD: record_toggle(app); break;
    case IDC_LOCK: monitor_lock(app); break;
    case IDC_EXPORT: export_csv_dialog(app); break;
    case IDC_CLEAR: clear_bursts(app); break;
    case IDC_APPLYFREQ: apply_center_freq(app); break;
    case IDC_COMBO_RATE:
        if (code == CBN_SELCHANGE) {
            const int i = int(SendMessageW(app.combo_rate, CB_GETCURSEL, 0, 0));
            if (i >= 0 && i < 4) {
                if (app.running) {
                    hackrftool::radio::RadioConfig cfg = current_radio_cfg(app);
                    cfg.sample_rate_hz = kRatesMsps[size_t(i)] * 1e6;
                    std::string err;
                    if (!app.radio.apply(cfg, &err))
                        app.status = L"配置失败: " + widen(err);
                }
                app.rate_index = i;
            }
        }
        break;
    case IDC_COMBO_SYM:
        if (code == CBN_SELCHANGE) {
            const int i = int(SendMessageW(app.combo_symrate, CB_GETCURSEL, 0, 0));
            if (i >= 0) app.symrate_idx = i;
        }
        break;
    case IDC_CHECK_AUTOTRACK:
        if (code == BN_CLICKED) {
            app.auto_track =
                SendMessageW(app.check_autotrack, BM_GETCHECK, 0, 0) == BST_CHECKED;
            app.monitor.set_mode(app.auto_track
                                     ? hackrftool::dsp::ChannelMonitor::Mode::auto_peak
                                     : hackrftool::dsp::ChannelMonitor::Mode::fixed_bin);
        }
        break;
    default: (void)from; break;
    }
}

void on_hscroll(App& app, HWND ctl) {
    if (ctl == app.track_lna) {
        int v = int(SendMessageW(ctl, TBM_GETPOS, 0, 0));
        v = std::clamp((v + 4) / 8 * 8, 8, 40);
        if (app.running) {
            hackrftool::radio::RadioConfig cfg = current_radio_cfg(app);
            cfg.lna_gain_db = unsigned(v);
            std::string err;
            if (!app.radio.apply(cfg, &err)) app.status = L"配置失败: " + widen(err);
        }
        app.lna = unsigned(v);
        SendMessageW(ctl, TBM_SETPOS, TRUE, LPARAM(v));
        SetWindowTextW(app.lbl_lna, (L"LNA " + std::to_wstring(v)).c_str());
    } else if (ctl == app.track_vga) {
        int v = int(SendMessageW(ctl, TBM_GETPOS, 0, 0));
        v = std::clamp((v + 1) / 2 * 2, 2, 62);
        if (app.running) {
            hackrftool::radio::RadioConfig cfg = current_radio_cfg(app);
            cfg.vga_gain_db = unsigned(v);
            std::string err;
            if (!app.radio.apply(cfg, &err)) app.status = L"配置失败: " + widen(err);
        }
        app.vga = unsigned(v);
        SendMessageW(ctl, TBM_SETPOS, TRUE, LPARAM(v));
        SetWindowTextW(app.lbl_vga, (L"VGA " + std::to_wstring(v)).c_str());
    } else if (ctl == app.track_threshold) {
        const int v = int(SendMessageW(ctl, TBM_GETPOS, 0, 0));
        app.threshold = double(v);
        SetWindowTextW(app.lbl_thr, (std::to_wstring(v) + L" dB").c_str());
    } else if (ctl == app.track_burst) {
        const int v = int(SendMessageW(ctl, TBM_GETPOS, 0, 0));
        app.burst_thr = double(v);
        SetWindowTextW(app.lbl_burst, (std::to_wstring(v) + L" dB").c_str());
    }
}

LRESULT CALLBACK main_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;   // 不擦除（类画刷=nullptr）：新暴露区域由 WM_PAINT 增量填充
    case WM_PRINTCLIENT: {
        // 透明子控件（如工具栏）重绘时向父窗口请求背景——必须补画，
        // 否则 DC 残留=黑底（TBSTYLE_FLAT 教训，双保险保留）
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT rc;
        GetClientRect(wnd, &rc);
        FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(wnd, &ps);
        // 只填无效区（WS_CLIPCHILDREN 已把子控件区域裁出）：设置行底色
        FillRect(ps.hdc, &ps.rcPaint, GetSysColorBrush(COLOR_BTNFACE));
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_COMMAND:
        if (app != nullptr)
            on_command(*app, LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
        return 0;
    case WM_HSCROLL:
        if (app != nullptr) on_hscroll(*app, reinterpret_cast<HWND>(lp));
        return 0;
    case WM_NOTIFY: {
        const auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->code == TTN_GETDISPINFOW) {
            auto* di =
                reinterpret_cast<NMTTDISPINFOW*>(const_cast<NMHDR*>(nm));
            switch (int(nm->idFrom)) {
            case IDC_STARTSTOP: di->lpszText = const_cast<LPWSTR>(
                L"开始/停止接收（HackRF 为独占设备）"); break;
            case IDC_SWEEP: di->lpszText =
                const_cast<LPWSTR>(L"全频段扫描（2400–2483.5 MHz 分 5 段轮换）"); break;
            case IDC_RECORD: di->lpszText =
                const_cast<LPWSTR>(L"录制原始 IQ 到 .cs8 文件（停止时写参数 sidecar）"); break;
            case IDC_LOCK: di->lpszText =
                const_cast<LPWSTR>(L"按目标频率锁定监测 bin"); break;
            default: break;
            }
        }
        return 0;
    }
    case WM_ENTERSIZEMOVE:
        if (app != nullptr) {
            app->live_sizing = true;
            app->sizing_enter_ms = GetTickCount64();
        }
        return 0;
    case WM_EXITSIZEMOVE:
        if (app != nullptr) {
            app->live_sizing = false;
            layout(*app);          // 拖拽期间全部冻结，此处一次性精确重铺
            app->last_sb_ms = 0;   // 状态栏立即补一拍（拖拽期间被节流+跳过）
            sync_chrome(*app);
        }
        return 0;
    case WM_SIZE:
        if (app != nullptr) {
            app->last_size_ms = GetTickCount64();
            layout(*app);
        }
        return 0;
    case WM_DPICHANGED: {
        const auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left,
                     r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (app != nullptr) {
            create_native_font(*app);
            for (const auto* v : {&app->row_common, &app->row_monitor, &app->row_capture})
                for (const auto& c : *v)
                    if (c.h != nullptr) SendMessageW(c.h, WM_SETFONT,
                                                     WPARAM(app->font), TRUE);
            layout(*app);
        }
        return 0;
    }
    case WM_CLOSE: DestroyWindow(wnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd_line, int) {
    SetUnhandledExceptionFilter(write_crash_dump);   // 崩溃自动落 dump

    // 单实例守卫：HackRF 是独占设备，第二个实例只会得到 "HackRF not found"，
    // 且多实例互踢会把 USB 流状态搞 wedge。已有实例时把它的窗口带到前台、
    // 提示后退出。
    const HANDLE single = CreateMutexW(nullptr, TRUE, L"HackRFTool-SingleInstance");
    struct MutexGuard {
        HANDLE h;
        ~MutexGuard() {
            if (h != nullptr) {
                ReleaseMutex(h);
                CloseHandle(h);
            }
        }
    } mutex_guard{single};
    if (single != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        // 自测模式被已有实例拦截：静默返回 42（CTest 记 SKIP）——弹窗会
        // 让无人值守的 ctest 永久挂起（实测：残留实例+守卫弹窗=selftest 卡死）
        if (cmd_line != nullptr && (wcscmp(cmd_line, L"selftest") == 0 ||
                                    wcscmp(cmd_line, L"selftestsweep") == 0))
            return 42;
        if (HWND prev = FindWindowW(nullptr, L"HackRFTool")) {
            ShowWindow(prev, SW_RESTORE);
            SetForegroundWindow(prev);
        }
        MessageBoxW(nullptr,
                    L"HackRFTool 已在运行（已为您切换到该窗口）。\n"
                    L"HackRF 为独占设备，请勿重复启动。",
                    L"HackRFTool", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // DPI 感知必须先于任何窗口创建（检查员实锤：重构时误删导致整窗
    // DWM 位图模糊、WM_DPICHANGED 成死代码）。清单不重复声明避免冲突。
    flux::enable_per_monitor_dpi_v2();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    App app;

    // 命令行：auto=接收+频谱页；autom=+监测页；autos=+全频段；autocap=+抓包页；
    // autosc=+全频段+抓包页；selftest/selftestsweep=自测模式（跑 N 秒→断言→
    // 写 selftest-report.txt→退出码）。selfclick 崩溃复现已退役（L4 根因已修）。
    bool auto_start = false;
    bool selftest = false;
    bool device_ok = false;
    if (cmd_line != nullptr) {
        if (wcscmp(cmd_line, L"auto") == 0) auto_start = true;
        if (wcscmp(cmd_line, L"autom") == 0) {
            auto_start = true;
            app.page = 1;
        }
        if (wcscmp(cmd_line, L"autos") == 0) {
            auto_start = true;
            app.sweep_on = 1;
        }
        if (wcscmp(cmd_line, L"autocap") == 0) {
            auto_start = true;
            app.page = 2;
        }
        if (wcscmp(cmd_line, L"autosc") == 0) {
            // 压测组合：接收 + 全频段扫描 + 实时抓包页（demod 高负载路径）
            auto_start = true;
            app.sweep_on = 1;
            app.page = 2;
        }
        if (wcscmp(cmd_line, L"selftest") == 0) {
            auto_start = true;
            selftest = true;
        }
        if (wcscmp(cmd_line, L"selftestsweep") == 0) {
            auto_start = true;
            selftest = true;
            app.sweep_on = 1;
        }
    }

    // 主窗口（原生骨架）+ 工具栏 + 设置行 + 状态栏。
    // 类样式禁用 CS_HREDRAW/CS_VREDRAW（拉伸时全窗失效重画=闪烁元凶），
    // 背景改由 WM_PAINT 只填无效区。禁用 WS_EX_COMPOSITED：与 DComp
    // 子窗口冲突（MSDN：不可用于含 D3D 子窗口的窗口），实测把工具栏
    // 背景搞成黑色。
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &main_wndproc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"HackRFToolMain";
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    app.main_wnd = CreateWindowExW(
        0, L"HackRFToolMain", L"HackRFTool",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        MulDiv(1200, dpi, 96), MulDiv(860, dpi, 96), nullptr, nullptr, instance,
        &app);
    SetWindowLongPtrW(app.main_wnd, GWLP_USERDATA, LONG_PTR(&app));
    create_native_font(app);
    create_toolbar(app);
    create_settings_row(app);
    create_statusbar(app);

    if (auto_start) {
        std::string err;
        if (!app.radio.open(&err)) {
            device_open_failed(app, err);
        } else {
            apply_radio(app);
            if (app.radio.start_rx(&rx_trampoline_ui, &app, &err)) {
                app.running = true;
                device_ok = true;
                if (app.sweep_on == 1) set_sweep_live(app, true);
            } else {
                app.status = L"启动接收失败: " + widen(err);
            }
        }
    }

    // 内容区：WinFlux Host 建为顶层后立即重父化为子窗口（保留 D2D/DComp
    // 合成渲染管线；消息驱动与窗口层级无关，外层泵照常分发）
    app.host.set_root_builder([&app] { return build(app); });
    flux::Host::Config cfg;
    cfg.title = L"HackRFTool";
    cfg.width = 1200;
    cfg.height = 860;
    if (!app.host.create(cfg, instance)) return 1;
    ShowWindow(app.host.hwnd(), SW_HIDE);
    LONG_PTR style = GetWindowLongPtrW(app.host.hwnd(), GWL_STYLE);
    SetWindowLongPtrW(app.host.hwnd(), GWL_STYLE,
                      (style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME |
                                 WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU |
                                 WS_DLGFRAME)) |
                          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS);
    SetWindowLongPtrW(app.host.hwnd(), GWL_EXSTYLE, 0);
    SetParent(app.host.hwnd(), app.main_wnd);
    // 内容区沉到兄弟 Z 序底部：拖拽冻结期间旧尺寸可能溢出内容带，
    // 工具栏/状态栏须能盖在它上面（否则溢出部分遮住状态栏）
    SetWindowPos(app.host.hwnd(), HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    layout(app);
    sync_chrome(app);   // 命令行预设页/模式 → 工具栏态立即就位（缓存初始化）
    ShowWindow(app.main_wnd, SW_SHOW);
    UpdateWindow(app.main_wnd);

    // M6：自测看门狗——收集指标、断言、写报告、发 WM_QUIT
    if (selftest) {
        const DWORD ui_tid = GetCurrentThreadId();
        int seconds = app.sweep_on == 1 ? 8 : 6;
        // 浸润测试：HACKRFTOOL_SOAK=秒数 覆盖默认时长（覆盖"一段时间后才崩"的尺度）
        char soak[16] = {};
        if (GetEnvironmentVariableA("HACKRFTOOL_SOAK", soak, sizeof soak) > 0) {
            const int sec = std::atoi(soak);
            if (sec > 0 && sec <= 900) seconds = sec;
        }
        const std::wstring report_path = exe_dir_path(
            app.sweep_on == 1 ? "selftest-report-sweep.txt" : "selftest-report-single.txt");
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
            if (std::FILE* rp = _wfopen(report_path.c_str(), L"w")) {
                std::fprintf(rp, "HackRFTool selftest\n模式: %s  时长: %ds\n",
                             app.sweep_on == 1 ? "全频段" : "单窗", seconds);
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

    // 外层消息泵（Host::run 不再使用；WM_QUIT 可来自主窗口或自测线程）
    MSG msg;
    int exit_code = 0;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    exit_code = int(msg.wParam);
    app.sweep_live.store(0);   // 收扫描线程（detach 线程须先令其退出再析构 App）
    Sleep(80);

    if (selftest) {
        // 退出码：无设备 42（CTest SKIP），硬断言失败 1，通过 0
        const std::wstring report_path = exe_dir_path(
            app.sweep_on == 1 ? "selftest-report-sweep.txt" : "selftest-report-single.txt");
        std::FILE* rp = _wfopen(report_path.c_str(), L"r");
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
