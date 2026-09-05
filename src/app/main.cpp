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
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <flux/flux.hpp>
#include <flux/Components.hpp>

#include "dsp/analyzer.hpp"
#include "dsp/apt.hpp"
#include "app/settings.hpp"
#include "app/telemetry.hpp"
#include "dsp/channel_monitor.hpp"
#include "dsp/esb.hpp"
#include "dsp/fm.hpp"
#include "dsp/gfsk.hpp"
#include "dsp/live_bursts.hpp"
#include "dsp/panorama.hpp"
#include "dsp/sigdb.hpp"
#include "dsp/waterfall.hpp"
#include "audio/waveout.hpp"
#include "radio/hackrf.hpp"
#include "radio/iq_recorder.hpp"
#include "ui/apt_view.hpp"
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

void tune_to(struct App& app, double mhz);   // 统一调谐（定义在 ids 枚举后）
void on_spectrum_click(struct App& app);    // 频谱点击调谐（同上）

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
    int page = 0;                 // 0=频谱 1=监测 2=抓包 3=收音 4=云图
    int sweep_on = 0;             // 0=单窗 1=全频段
    int rate_index = 4;           // kRatesMsps 下标（默认 20 Msps）
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

    // ---- 收音机（#53）----
    std::atomic<bool> fm_on{false};        // fm 解调线程运行
    std::atomic<bool> fm_scan{false};      // 扫台进行中
    std::atomic<float> fm_pilot{0.0f};     // 导频相关（立体声判定，UI 读）
    std::atomic<float> fm_peak{0.0f};      // 音频峰值表
    std::atomic<bool> squelch_open{false}; // 静噪门（峰均差判据，UI 心跳写）
    // 人声强度（#55g）：fm 线程算（300-3.4k 带通 RMS），UI 心跳拷贝绘制
    hackrftool::dsp::VoiceLevelMeter voice_meter;
    std::mutex voice_mtx;
    std::vector<float> voice_hist;        // dBFS，20Hz 推送，600 点=30s
    unsigned voice_seq = 0;
    float voice_cur = -120.0f;
    int voice_dec = 0;                    // 10ms 块 ÷5 = 20Hz
    // 音频频谱（#57）：fm 线程 feed mono，seq 变化时拷贝给 UI
    hackrftool::dsp::AudioSpectrumMeter audio_spec;
    std::mutex spec_mtx;
    std::vector<float> spec_db, spec_peak;
    unsigned spec_seq = 0, spec_seen = 0;   // seen=fm 线程私有比较位
    int vol = 80;                          // 音量 0..100（waveout 外存，供设置持久化）
    // 设置持久化（#57）：序列化比对变化才写盘，3s 节流
    std::string last_settings;
    unsigned long long last_set_ms = 0;
    // 信号库非模态弹窗（#62）：任意页快速选台
    HWND sigdb_wnd = nullptr;
    HWND sigdb_list = nullptr;
    unsigned sigdb_stamp = 0;   // 上次刷新的库版本（signals 大小+首条频率）
    // 数据面遥测（#60）：UI 状态变更快照 + DSP 1Hz 节流
    std::string last_ui_state;
    unsigned long long last_dsp_ms = 0;
    bool afc_on = true;                    // AFC 自动频率微调（收音页）
    HWND check_afc = nullptr;
    int fm_bw = 0;                         // 收听带宽：0=±120k 1=±80k 2=±50k
    bool stereo_opt = true;                // 立体声选项（不勾=强制单声，B4）
    int spec_zoom_idx = 0;                // 频谱 X 轴缩放档（×1/2/4/8，3c）
    int spec_y_idx = 0;                   // 频谱 Y 轴动态档（0 全/1 中/2 细，#63）
    HWND check_stereo = nullptr;
    HWND combo_bw = nullptr;
    unsigned long long afc_pause_until = 0;   // 显式调谐后 AFC 暂停期限
    float squelch_gain = 0.0f;             // 静噪增益平滑（fm 线程私有）
    double radio_mhz = 98.0;               // 收音机页当前频率
    std::vector<double> stations;          // 扫台结果（MHz）
    hackrftool::audio::WaveOut waveout;
    std::unique_ptr<hackrftool::dsp::FmReceiver> fm_rx;   // 采样率切换时重建
    // SPSC 字节环：rx 线程写 → fm 线程读（满则覆盖旧数据，保实时）
    struct SpscRing {
        std::vector<std::int8_t> buf = std::vector<std::int8_t>(4 << 20);
        std::atomic<std::size_t> w{0}, r{0};
        void write(const std::int8_t* p, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) {
                buf[w.load(std::memory_order_relaxed)] = p[i];
                w.store((w.load(std::memory_order_relaxed) + 1) % buf.size(),
                        std::memory_order_release);
            }
        }
        std::size_t read(std::int8_t* p, std::size_t n) {
            std::size_t got = 0;
            while (got < n) {
                const std::size_t rr = r.load(std::memory_order_relaxed);
                if (rr == w.load(std::memory_order_acquire)) break;
                p[got++] = buf[rr];
                r.store((rr + 1) % buf.size(), std::memory_order_release);
            }
            return got;
        }
        void clear() {
            r.store(w.load(std::memory_order_relaxed));
        }
    } iq_ring;
    HWND edit_radio = nullptr;      // 收音机页频率
    HWND combo_stations = nullptr;  // 扫台结果
    HWND track_vol = nullptr;
    HWND check_mute = nullptr;
    HWND lbl_vol = nullptr;
    HWND combo_sat = nullptr;       // 云图页卫星预设（#54 接收调谐）
    // ---- 信号库与选择化交互（#55）----
    std::vector<hackrftool::dsp::SignalEntry> signals;   // 扫描累积（启动加载）
    int sig_cat = 0;          // 筛选：0=电台 1=卫星 2=ISM 3=全部
    int sig_online = 1;       // 筛选：1=仅在线 0=全部
    int sig_sort = 0;         // 排序：0=强度 1=频率
    hackrftool::ui::SpectrumGeom spec_geom;   // 频谱点击换算几何（paint 每帧回写）
    HWND combo_sigcat = nullptr, combo_online = nullptr, combo_sort = nullptr;
    int audio_dev = -1;   // 输出设备（-1=系统默认；WaveOut::enum_devices 下标）
    HWND combo_audio = nullptr;
    bool amp = false;            // 板载功放（+14 dB，弱信号/室内天线场景）
    bool gains_pinned = false;   // 增益来自设置恢复（ensure_fm 不再覆盖为广播默认）
    bool settings_hold = false;  // 自测模式禁写设置（防污染用户参数）
    HWND check_amp = nullptr;

    // ---- 云图（#54）----
    std::atomic<bool> apt_on{false};           // APT 解码启用（记录勾选+云图页+接收中）
    hackrftool::dsp::AptDecoder apt;           // fm 线程 feed，UI 线程快照
    hackrftool::ui::AptView apt_view;          // 原生 GDI 云图窗（云图页覆盖内容区）
    std::uint64_t apt_seen_lines = 0;          // 上次刷到视图的行数
    HWND check_apt = nullptr;                  // 「记录」勾选

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
    std::vector<CtlSlot> row_radio;     // 仅收音机页（#53）
    std::vector<CtlSlot> row_radio2;    // 收音机页第二行（筛选/音频选项，B4）
    std::vector<CtlSlot> row_weather;   // 仅云图页（#54）
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

constexpr double kRatesMsps[5] = {2.0, 8.0, 10.0, 16.0, 20.0};

double clamp_center(double mhz) noexcept { return std::clamp(mhz, 2400.0, 2483.5); }

// 频谱窗半宽（MHz）随采样率（#53 前 20 Msps 硬编码，低速档频标会错位）
double half_bw_mhz(const App& app) noexcept { return kRatesMsps[size_t(app.rate_index)] / 2.0; }

// 收音机/卫星段频率（不受 2.4G 限幅约束）
double clamp_radio(double mhz) noexcept { return std::clamp(mhz, 24.0, 1800.0); }

std::size_t mhz_to_bin(double mhz, double center_mhz, double bw_mhz) noexcept {
    const double t = (mhz - (center_mhz - bw_mhz / 2.0)) / bw_mhz;   // 0..1
    return static_cast<std::size_t>(std::clamp(t, 0.0, 1.0) * 255.0 + 0.5);
}

void rx_trampoline_ui(const std::int8_t* iq, std::size_t bytes, void* ctx) {
    auto* app = static_cast<App*>(ctx);
    app->analyzer.feed(iq, bytes);
    app->live.write(iq, bytes);
    if (app->recorder.recording()) app->recorder.write(iq, bytes);
    if (app->fm_on.load()) app->iq_ring.write(iq, bytes);
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
    cfg.amp = app.amp;
    return cfg;
}

// 运行中重配（采样率/增益/频率）：停流→配置→重开。流中 hackrf_set_sample_rate
// 返回成功但数据流不切换（真机实测解调全噪，#55c rx_check 铁证）
void reconfigure_rx(App& app, const hackrftool::radio::RadioConfig& cfg) {
    hackrftool::log::log_telemetry(
        hackrftool::log::Level::info, "RADIO", "reconfig",
        {{"rate_msps", std::to_string(cfg.sample_rate_hz / 1e6)},
         {"center_mhz", std::to_string(cfg.center_hz / 1e6)},
         {"lna", std::to_string(cfg.lna_gain_db)},
         {"vga", std::to_string(cfg.vga_gain_db)}});
    if (!app.running) {
        std::string err;
        if (!app.radio.apply(cfg, &err)) app.status = L"配置失败: " + widen(err);
        return;
    }
    app.radio.stop_rx();
    std::string err;
    if (!app.radio.apply(cfg, &err)) {
        app.status = L"配置失败: " + widen(err);
    }
    if (!app.radio.start_rx(&rx_trampoline_ui, &app, &err))
        app.status = L"重启接收失败: " + widen(err);
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

void ensure_fm(App& app, bool on);   // 收音机音频链开关（定义在收音机节）
void reconfigure_rx(App& app, const hackrftool::radio::RadioConfig& cfg);
void update_apt_on(App& app);        // APT 解码开关（定义在收音机节）

void toggle_rx(App& app) {
    if (app.running) {
        set_sweep_live(app, false);
        ensure_fm(app, false);
        update_apt_on(app);
        if (app.recorder.recording()) app.recorder.stop();
        app.radio.stop_rx();
        app.running = false;
        app.status = L"已停止";
        hackrftool::log::log_telemetry(hackrftool::log::Level::info, "LIFE",
                                       "rx.stop", {});
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
        hackrftool::log::log_telemetry(hackrftool::log::Level::error, "LIFE",
                                       "rx.start.fail", {{"err", err}});
        return;
    }
    app.running = true;
    hackrftool::log::log_telemetry(
        hackrftool::log::Level::info, "LIFE", "rx.start",
        {{"center_mhz", std::to_string(app.center_mhz)},
         {"rate_msps", std::to_string(kRatesMsps[size_t(app.rate_index)])},
         {"lna", std::to_string(app.lna)},
         {"vga", std::to_string(app.vga)},
         {"src", "toolbar"}});
    if (app.sweep_on == 1) set_sweep_live(app, true);
    if (app.page >= 3) ensure_fm(app, true);   // 收音/云图页直接起音频链
    update_apt_on(app);
}

// 应用中心频率（工具栏「应用频率」）：读当前页频率框（频谱页/收音机页）
void apply_center_freq(App& app) {
    wchar_t buf[32] = {};
    hackrftool::log::log_telemetry(hackrftool::log::Level::info, "UI",
                                   "applyfreq", {{"page", std::to_string(app.page)}});
    if (app.page == 3) {
        // 收音机页：调谐 FM 广播段（87.5–108），监测 bin 语义不适用
        GetWindowTextW(app.edit_radio, buf, 32);
        const double mhz = clamp_radio(std::wcstod(buf, nullptr));
        if (app.running) {
            hackrftool::radio::RadioConfig cfg = current_radio_cfg(app);
            cfg.center_hz = mhz * 1e6;
            std::string err;
            if (!app.radio.apply(cfg, &err)) app.status = L"配置失败: " + widen(err);
        }
        app.radio_mhz = mhz;
        app.center_mhz = mhz;   // 频谱窗/状态栏跟随
        SetWindowTextW(app.edit_radio, wd1(mhz).c_str());
        app.status = L"收音机调谐 " + wd1(mhz) + L" MHz";
        return;
    }
    GetWindowTextW(app.edit_freq, buf, 32);
    const double mhz = clamp_center(std::wcstod(buf, nullptr));
    const bool relock = app.mon_lock_mhz > 0.0 && !app.auto_track;
    if (app.running) {
        hackrftool::radio::RadioConfig cfg = current_radio_cfg(app);
        cfg.center_hz = mhz * 1e6;
        std::string err;
        if (!app.radio.apply(cfg, &err)) app.status = L"配置失败: " + widen(err);
    }
    if (relock)
        app.monitor.set_fixed_bin(mhz_to_bin(app.mon_lock_mhz, mhz, 2.0 * half_bw_mhz(app)));
    app.center_mhz = mhz;
    SetWindowTextW(app.edit_freq, wd1(mhz).c_str());
}

void monitor_lock(App& app) {
    wchar_t buf[32] = {};
    GetWindowTextW(app.edit_mon, buf, 32);
    const double mhz = clamp_center(std::wcstod(buf, nullptr));
    app.mon_lock_mhz = mhz;   // 记住目标频率，中心变化时重算 bin（M6）
    app.monitor.set_fixed_bin(mhz_to_bin(mhz, app.center_mhz, 2.0 * half_bw_mhz(app)));
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

// ---- 收音机（#53）：fm 解调线程 / 音频开关 / 自动扫台 ------------------------

// fm 线程：SPSC 环拉 IQ → FmReceiver → waveOut（音频回调在本线程执行）
void fm_audio_cb(const float* l, const float* r, std::size_t n, void* ctx) {
    auto* app = static_cast<App*>(ctx);
    // 静噪：门开→1、门闭→0.03（近哑音）；τ≈50ms 平滑防爆音
    const float target = app->squelch_open.load() ? 1.0f : 0.03f;
    app->squelch_gain += 0.2f * (target - app->squelch_gain);
    float gl[480], gr[480];
    const std::size_t m = std::min<std::size_t>(n, 480);
    for (std::size_t i = 0; i < m; ++i) {
        gl[i] = l[i] * app->squelch_gain;
        gr[i] = r[i] * app->squelch_gain;
    }
    app->waveout.write(gl, gr, m);
    if (n > m) app->waveout.write(l + m, r + m, n - m);   // n 恒为 480，保险
    // 人声强度：对实际输出（静噪后）检测，每 5 块（50ms）入史
    const float db = app->voice_meter.feed(gl, m);
    if (++app->voice_dec >= 5) {
        app->voice_dec = 0;
        std::lock_guard<std::mutex> g(app->voice_mtx);
        app->voice_hist.push_back(db);
        if (app->voice_hist.size() > 600) app->voice_hist.erase(app->voice_hist.begin());
        app->voice_cur = db;
        ++app->voice_seq;
    }
    // 音频频谱：静噪前取 (L+R)/2——静噪会掐掉底噪谱形；seq 变化才拷贝
    {
        float mono[480];
        const std::size_t k = std::min<std::size_t>(n, 480);
        for (std::size_t i = 0; i < k; ++i) mono[i] = (l[i] + r[i]) * 0.5f;
        app->audio_spec.feed(mono, k);
        if (app->audio_spec.seq() != app->spec_seen) {
            app->spec_seen = app->audio_spec.seq();
            std::lock_guard<std::mutex> g(app->spec_mtx);
            app->spec_db = app->audio_spec.spectrum_db();
            app->spec_peak = app->audio_spec.peak_db();
            app->spec_seq = app->spec_seen;
        }
    }
    if (app->apt_on.load()) app->apt.feed(l, n);   // APT：单声道取 L
    app->fm_pilot.store(app->fm_rx ? app->fm_rx->pilot_level() : 0.0f);
    app->fm_peak.store(app->fm_rx ? app->fm_rx->audio_peak() : 0.0f);
}

void fm_loop(App& app) {
    std::vector<std::int8_t> tmp(65536);
    // 诊断（#55d 临时）：HACKRFTOOL_FMDUMP=1 时把 fm 线程实收的前 8MB 落盘
    char dump_env[8] = {};
    bool dumping = GetEnvironmentVariableA("HACKRFTOOL_FMDUMP", dump_env,
                                           sizeof dump_env) > 0 &&
                   dump_env[0] == '1';
    std::FILE* dump = nullptr;
    std::size_t dumped = 0;
    if (dumping) {
        dump = _wfopen(exe_dir_path("fm-ring-dump.cs8").c_str(), L"wb");
        app.status = L"FM 环形数据采集中（8MB）…";
    }
    while (app.fm_on.load()) {
        const std::size_t n = app.iq_ring.read(tmp.data(), tmp.size());
        if (n == 0) {
            Sleep(4);
            continue;
        }
        if (dump != nullptr) {
            std::fwrite(tmp.data(), 1, n, dump);
            dumped += n;
            if (dumped >= 8 << 20) {   // 滚动覆盖：始终保留最近 8MB
                std::fclose(dump);
                dump = _wfopen(exe_dir_path("fm-ring-dump.cs8").c_str(), L"wb");
                dumped = 0;
            }
        }
        if (app.fm_rx) app.fm_rx->feed(tmp.data(), n);
    }
    if (dump != nullptr) std::fclose(dump);
}

// 开/关收音机音频链（进入收音页 + 接收中 → 开；离开/停止 → 关）。
// 采样率自动切 2 Msps（窄带任务省算力，FmReceiver 按当前率重建）
void ensure_fm(App& app, bool on) {
    // B3：全频段扫描期间频率每秒轮换 5 段，解调音频只剩跳变噪声——
    // 拒开音频链并提示（扫描关闭后进页/重新操作即恢复）
    if (on && app.sweep_on == 1) {
        app.status = L"全频段扫描中，收音/云图暂不可用（先退出全频段）";
        return;
    }
    const bool was = app.fm_on.load();
    if (on == was) {
        if (on && !app.waveout.running()) (void)app.waveout.start(app.audio_dev);
        return;
    }
    if (on) {
        // 广播接收默认增益（对照 SDRSharp 可用设置：LNA 40/amp on）；
        // 用户手动调过（≥32）或设置恢复过则不动
        if (app.lna < 32 && !app.gains_pinned) {
            app.lna = 40;
            app.vga = std::max(app.vga, 32u);
            app.amp = true;
            SendMessageW(app.track_lna, TBM_SETPOS, TRUE, LPARAM(40));
            SendMessageW(app.track_vga, TBM_SETPOS, TRUE, LPARAM(app.vga));
            if (app.check_amp != nullptr)
                SendMessageW(app.check_amp, BM_SETCHECK, BST_CHECKED, 0);
            SetWindowTextW(app.lbl_lna, L"LNA 40");
            SetWindowTextW(app.lbl_vga, (L"VGA " + std::to_wstring(app.vga)).c_str());
            if (app.running) {
                hackrftool::radio::RadioConfig cfg = current_radio_cfg(app);
                std::string err;
                if (!app.radio.apply(cfg, &err))
                    app.status = L"配置失败: " + widen(err);
            }
        }
        if (app.rate_index != 0) {
            app.rate_index = 0;
            hackrftool::radio::RadioConfig cfg = current_radio_cfg(app);
            reconfigure_rx(app, cfg);
            SendMessageW(app.combo_rate, CB_SETCURSEL, 0, 0);
        }
        app.iq_ring.clear();
        static const double kFmBw[3] = {120e3, 80e3, 50e3};
        app.fm_rx = std::make_unique<hackrftool::dsp::FmReceiver>(
            kRatesMsps[size_t(app.rate_index)] * 1e6, 80.0,
            !app.stereo_opt, kFmBw[size_t(app.fm_bw)]);
        app.fm_rx->set_audio_callback(&fm_audio_cb, &app);
        const bool ok = app.waveout.start(app.audio_dev);
        if (!ok) app.status = L"音频设备打开失败（检查扬声器/默认设备）";
        app.fm_on.store(true);
        hackrftool::log::log_telemetry(
            hackrftool::log::Level::info, "AUDIO", "fm.on",
            {{"rate_msps", std::to_string(kRatesMsps[size_t(app.rate_index)])},
             {"bw_idx", std::to_string(app.fm_bw)},
             {"stereo", app.stereo_opt ? "1" : "0"},
             {"dev", std::to_string(app.audio_dev)}});
        std::thread(fm_loop, std::ref(app)).detach();
    } else {
        app.fm_on.store(false);
        hackrftool::log::log_telemetry(hackrftool::log::Level::info, "AUDIO",
                                       "fm.off", {});
        app.waveout.stop();
        Sleep(30);   // 等线程退出读环
        app.fm_rx.reset();
    }
}

// 统一扫描线程（#55 泛化）：按类别扫频段，命中并入信号库（强度/在线），
// 结束落盘并回最强台（仅电台段）
void scan_stations_loop(App& app, int cat, double return_mhz) {
    using hackrftool::dsp::SigCat;
    double lo, hi, step;
    switch (cat) {
    case 0: lo = 87.5; hi = 108.0; step = 0.1; break;      // FM 广播
    case 1: lo = 137.0; hi = 138.0; step = 0.05; break;    // NOAA 卫星
    case 2: lo = 2400.0; hi = 2483.5; step = 1.0; break;   // 2.4G ISM
    default: lo = 87.5; hi = 108.0; step = 0.1; break;
    }
    const SigCat band_cat = cat == 0   ? SigCat::radio
                            : cat == 1 ? SigCat::sat
                                       : SigCat::ism;
    app.waveout.set_mute(true);
    // 该段旧条目全部离线（保留待验证），命中重新置在线
    for (auto& e : app.signals)
        if (hackrftool::dsp::band_of(e.mhz).cat == band_cat) e.online = false;
    // 本次命中先局部收集，扫完后聚类（相邻 <0.15MHz 同台取最强）再入库
    // ——直接大容差 merge 会链式漂移（98.1→98.2→98.3 被滚动合并）
    std::vector<std::pair<double, float>> hits;
    for (double f = lo; f <= hi && app.fm_scan.load(); f += step) {
        (void)app.radio.set_center_hz(f * 1e6);
        app.iq_ring.clear();
        // 丢弃旧频残帧：analyzer 帧与跳频不同步，驻留首帧可能仍是上一频点
        // 的视场（真机实测：台被记到 +0.1MHz 名下、点开全是噪）。等两帧
        // 新频帧（帧率 ~25fps）再取样
        unsigned last_seq = app.analyzer.snapshot().seq;
        for (int w = 0; w < 2; ++w) {
            for (int t = 0; t < 20 && app.analyzer.snapshot().seq == last_seq;
                 ++t)
                Sleep(10);
            last_seq = app.analyzer.snapshot().seq;
        }
        const auto frame = app.analyzer.snapshot();
        if (frame.db.empty()) continue;
        float sum = 0.0f, mx = -999.0f;
        for (const float v : frame.db) {
            sum += v;
            mx = std::max(mx, v);
        }
        const float avg = sum / float(frame.db.size());
        // 阈值收紧：峰均差 >10dB 才算台（6dB 时高增益下噪声包大量误报）
        if (mx > -42.0f && mx - avg > 10.0f) hits.emplace_back(f, mx);
    }
    std::sort(hits.begin(), hits.end());
    double best = 0.0;
    float best_db = -999.0f;
    for (std::size_t i = 0; i < hits.size(); ++i) {
        const double f = hits[i].first;
        const float db = hits[i].second;
        // 跳过与前一命中同簇的弱频点（0.15MHz 内取最强）
        if (i > 0 && f - hits[i - 1].first < 0.15) {
            if (db <= hits[i - 1].second) continue;
        }
        hackrftool::dsp::merge_scan(app.signals, f, db, 0.05);
        if (band_cat == SigCat::radio && db > best_db) {
            best_db = db;
            best = f;
        }
    }
    (void)hackrftool::dsp::save_signals(
        exe_dir_path("signals.tsv").c_str(), app.signals);
    // 回频：电台段回最强台（找得到时），其余回原频
    const double back =
        (band_cat == SigCat::radio && best > 0.0) ? best : return_mhz;
    hackrftool::log::log_telemetry(hackrftool::log::Level::info, "SCAN",
                                   "done",
                                   {{"hits", std::to_string(hits.size())},
                                    {"best", std::to_string(back)}});
    (void)app.radio.set_center_hz(back * 1e6);
    app.radio_mhz = back;
    app.center_mhz = back;
    wchar_t buf[32];
    swprintf(buf, 32, L"%.4g", back);
    SetWindowTextW(app.edit_radio, buf);
    app.waveout.set_mute(false);
    app.fm_scan.store(false);
    // 汇总该段命中数
    const auto idx = hackrftool::dsp::filter_signals(app.signals, band_cat,
                                                     true, false);
    app.status = L"扫描完成：" + std::to_wstring(idx.size()) + L" 个在线信号" +
                 (band_cat == SigCat::radio && best > 0.0
                      ? L"，已调谐最强台"
                      : (idx.empty() ? L"（未检出，检查天线/增益）" : L""));
}

void start_scan(App& app) {
    if (app.fm_scan.load()) return;
    if (!app.running) {
        app.status = L"请先开始接收再扫描";
        return;
    }
    if (app.sweep_on == 1) {
        app.status = L"扫描需先退出全频段模式";
        return;
    }
    app.fm_scan.store(true);
    static const wchar_t* kNames[3] = {L"FM 广播", L"NOAA 卫星", L"2.4G ISM"};
    const int cat = app.sig_cat <= 2 ? app.sig_cat : 0;
    hackrftool::log::log_telemetry(
        hackrftool::log::Level::info, "SCAN", "start",
        {{"cat", std::to_string(cat)}});
    app.status = std::wstring(L"扫频中：") + kNames[cat] + L"…";
    std::thread(scan_stations_loop, std::ref(app), cat, app.center_mhz).detach();
}

// 随机收听/收看（#55）：当前筛选条件下的在线条目随机调谐
void random_listen(App& app) {
    using hackrftool::dsp::SigCat;
    const SigCat cat = app.sig_cat == 0   ? SigCat::radio
                       : app.sig_cat == 1 ? SigCat::sat
                       : app.sig_cat == 2 ? SigCat::ism
                                          : SigCat::all;
    const long i = hackrftool::dsp::random_pick(
        app.signals, cat, unsigned(GetTickCount() & 0x7fffffffu));
    if (i < 0) {
        app.status = L"无在线信号可选——先「扫描电台」积累信号库";
        return;
    }
    tune_to(app, app.signals[size_t(i)].mhz);
    app.status += L"（随机）";
}

// APT 解码开关：云图页 + 接收中 + 音频链在跑 + 「记录」勾选
void update_apt_on(App& app) {
    const bool on = app.page == 4 && app.running && app.fm_on.load() &&
                    app.check_apt != nullptr &&
                    SendMessageW(app.check_apt, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (on && !app.apt_on.load()) {
        app.apt_seen_lines = 0;
        app.status = L"APT 记录中（卫星过境时云图逐行累积）";
    }
    app.apt_on.store(on);
}

void save_apt_dialog(App& app) {
    hackrftool::dsp::AptImage snap;
    app.apt.snapshot(snap);
    if (snap.rows == 0) {
        app.status = L"尚无云图可保存（先记录卫星过境）";
        return;
    }
    wchar_t path[MAX_PATH] = L"apt-cloud.png";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app.main_wnd;
    ofn.lpstrFilter = L"PNG 图像 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;
    app.status = hackrftool::ui::save_apt_png(path, snap)
                     ? L"云图已保存: " + std::wstring(path)
                     : L"保存失败（PNG 编码错误）";
}


// ---- 内容区各页（纯显示；全部控制在顶部工具栏） ------------------------------

flux::ElementPtr spectrum_display(App& app, const flux::Palette& pal) {
    flux::Props page_p;
    page_p.direction = flux::Direction::column;
    page_p.align = flux::Align::stretch;
    page_p.gap = 8.0f;
    page_p.flex_grow = 1.0f;
    auto page_el = flux::view(std::move(page_p));
    const double half = half_bw_mhz(app);   // 窗宽随采样率（#53：低速档不再错位）
    if (app.sweep_on == 0) {
        // 3c：X 轴缩放（×1/×2/×4/×8——256bin 基础上 8 倍仍余 32bin 连线；
        // 更窄需降采样率）+ 左右平移（步长=显示半窗）；瀑布保持全窗（时基不变）
        static const double kZooms[4] = {1.0, 2.0, 4.0, 8.0};
        const double zoom = kZooms[size_t(app.spec_zoom_idx)];
        const double vhalf = half / zoom;
        {
            flux::Props bar_p;
            bar_p.direction = flux::Direction::row;
            bar_p.align = flux::Align::center;
            bar_p.gap = 8.0f;
            auto bar = flux::view(std::move(bar_p));
            bar->children.push_back(flux::ui::caption(pal, L"X 轴缩放", {}));
            bar->children.push_back(flux::ui::segmented(
                pal, {L"×1", L"×2", L"×4", L"×8"}, app.spec_zoom_idx,
                [&app](int i) { app.spec_zoom_idx = i; }, {}));
            bar->children.push_back(flux::ui::caption(pal, L"Y 轴", {}));
            bar->children.push_back(flux::ui::segmented(
                pal, {L"100dB", L"60dB", L"40dB"}, app.spec_y_idx,
                [&app](int i) { app.spec_y_idx = i; }, {}));
            bar->children.push_back(flux::ui::icon_button(
                pal, flux::IconKind::chevron_left, L"左移半窗",
                [&app, vhalf] { tune_to(app, app.center_mhz - vhalf); }));
            bar->children.push_back(flux::ui::icon_button(
                pal, flux::IconKind::chevron_right, L"右移半窗",
                [&app, vhalf] { tune_to(app, app.center_mhz + vhalf); }));
            wchar_t zt[48];
            swprintf(zt, 48, L"显示 %.3f–%.3f MHz（步进 %.1f M）",
                     app.center_mhz - vhalf, app.center_mhz + vhalf, vhalf);
            bar->children.push_back(flux::ui::caption(pal, zt, {}));
            page_el->children.push_back(std::move(bar));
        }
        // 显示窗切片（spectrum_view 语义：db 覆盖 [lo,hi]——切片免改组件）
        std::vector<float> sub, subpk;
        const double vlo = app.center_mhz - vhalf, vhi = app.center_mhz + vhalf;
        if (!app.frame.db.empty()) {
            const double fbw = 2.0 * half / double(app.frame.db.size());
            for (std::size_t i = 0; i < app.frame.db.size(); ++i) {
                const double f = app.center_mhz - half + double(i) * fbw + fbw / 2.0;
                if (f >= vlo && f <= vhi) {
                    sub.push_back(app.frame.db[i]);
                    subpk.push_back(app.frame.peak.empty() ? app.frame.db[i]
                                                           : app.frame.peak[i]);
                }
            }
        }
        // 刻度粒度自适应窗宽（0.2/0.5/1/2/5/10 M 六档）
        const double vw = 2.0 * vhalf;
        const double step = vw > 40.0    ? 10.0
                            : vw > 16.0  ? 5.0
                            : vw > 6.0   ? 2.0
                            : vw > 2.4   ? 1.0
                            : vw > 0.8   ? 0.5
                                         : 0.2;
        std::vector<hackrftool::ui::SpectrumTick> ticks;
        for (double f = std::ceil(vlo / step) * step; f <= vhi + 1e-9;
             f += step) {
            wchar_t lab[16];
            swprintf(lab, 16, L"%.6g", f);
            ticks.push_back({f, lab});
        }
        static const float kYFloor[3] = {-100.0f, -60.0f, -40.0f};
        page_el->children.push_back(hackrftool::ui::spectrum_view(
            pal, sub, subpk, vlo, vhi, ticks, app.frame.seq, &app.spec_geom,
            [&app] { on_spectrum_click(app); },
            kYFloor[size_t(app.spec_y_idx)]));
        page_el->children.push_back(
            hackrftool::ui::waterfall_view(pal, app.waterfall, app.waterfall.seq()));
    } else {
        page_el->children.push_back(flux::ui::caption(
            pal,
            L"全景扫描中 2400–2483.5 MHz（5 段每秒轮换拼接，点击强频段=专门收听）",
            {}));
        const std::vector<hackrftool::ui::SpectrumTick> ticks = {
            {2400.0, L"2400"}, {2420.0, L"2420"}, {2440.0, L"2440"},
            {2460.0, L"2460"}, {2480.0, L"2480"},
        };
        page_el->children.push_back(hackrftool::ui::spectrum_view(
            pal, app.pano.panorama(), {}, kSweepLoMhz, kSweepHiMhz, ticks,
            app.pano.seq(), &app.spec_geom, [&app] { on_spectrum_click(app); }));
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
        hackrftool::log::log_telemetry(
            hackrftool::log::Level::info, "LIFE", "record.stop",
            {{"bytes", std::to_string(app.recorder.bytes_written())}});
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
        hackrftool::log::log_telemetry(hackrftool::log::Level::error, "LIFE",
                                       "record.fail", {});
        return;
    }
    app.status = L"录制中…";
    hackrftool::log::log_telemetry(hackrftool::log::Level::info, "LIFE",
                                   "record.start", {});
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

// ---- 收音机页（#53/#55 重设计）：信号库列表 + 筛选 + 点击即听 ----------------

// 信号行文本：频率 + 强度条 + dB + 在线点 + 频段名
std::wstring signal_row_text(const hackrftool::dsp::SignalEntry& e) {
    wchar_t head[96];
    swprintf(head, 96, L"%8.3f MHz  ", e.mhz);
    std::wstring t = head;
    const int bars = std::clamp(int((e.db + 100.0f) * 16.0f / 60.0f), 0, 16);
    t += L"[";
    for (int i = 0; i < 16; ++i) t += (i < bars) ? L"█" : L"·";
    t += L"] ";
    swprintf(head, 96, L"%5.0f dB  %s  %s", e.db,
             e.online ? L"●在线" : L"○离线",
             hackrftool::dsp::band_of(e.mhz).name);
    return t + head;
}

flux::ElementPtr radio_display(App& app, const flux::Palette& pal) {
    flux::Props page_p;
    page_p.direction = flux::Direction::column;
    page_p.align = flux::Align::stretch;
    page_p.gap = 8.0f;
    page_p.flex_grow = 1.0f;
    auto page_el = flux::view(std::move(page_p));

    // 频率大字 + 立体声徽章 + 收听提示
    flux::Props head_p;
    head_p.direction = flux::Direction::row;
    head_p.align = flux::Align::center;
    head_p.gap = 12.0f;
    auto head = flux::view(std::move(head_p));
    flux::Props freq_p;
    freq_p.bold = true;
    freq_p.font_size_pt = 24.0f;
    freq_p.flex_grow = 1.0f;
    freq_p.text_align = flux::Align::start;
    head->children.push_back(
        flux::label(wd1(app.radio_mhz) + L" MHz", std::move(freq_p)));
    // 信号状态灯（B4）：绿=接收正常（判有台）、红=空频点——颜色即语义，
    // 不再跳动；峰值与导频改为数值读数（稳定不闪，替代原 24 格电平条）
    head->children.push_back(flux::ui::badge(
        pal,
        app.squelch_open.load() ? flux::ui::BadgeKind::success
                                : flux::ui::BadgeKind::danger,
        app.squelch_open.load() ? L"● 信号正常" : L"● 无信号"));
    {
        wchar_t buf[48];
        swprintf(buf, 48, L"导频 %.3f｜峰值 %+.1f dB",
                 app.fm_pilot.load(),
                 20.0f * std::log10(std::max(app.fm_peak.load(), 1e-6f)));
        head->children.push_back(flux::ui::caption(pal, buf, {}));
    }
    page_el->children.push_back(std::move(head));

    // 人声强度滚动波形（30s 历史，人声越强越高=信号越好）
    {
        std::lock_guard<std::mutex> g(app.voice_mtx);
        page_el->children.push_back(hackrftool::ui::audio_level_strip(
            pal, app.voice_hist, app.voice_cur, app.voice_seq));
    }

    // 音频频谱 0–24 kHz（#57）：实时谱+峰保持+19k 导频参考线
    {
        std::lock_guard<std::mutex> g(app.spec_mtx);
        page_el->children.push_back(hackrftool::ui::audio_spectrum_strip(
            pal, app.spec_db, app.spec_peak, app.spec_seq));
    }

    // 信号库列表（按筛选条件）：点击行=调谐（#55 零手动输入）
    using hackrftool::dsp::SigCat;
    const SigCat cat = app.sig_cat == 0   ? SigCat::radio
                       : app.sig_cat == 1 ? SigCat::sat
                       : app.sig_cat == 2 ? SigCat::ism
                                          : SigCat::all;
    const auto idx = hackrftool::dsp::filter_signals(
        app.signals, cat, app.sig_online == 1, app.sig_sort == 0);
    flux::Props tip_p;
    tip_p.text_align = flux::Align::start;
    page_el->children.push_back(flux::ui::caption(
        pal, idx.empty()
                 ? L"信号库为空：点工具栏「扫描电台」积累列表；列表行可直接点击收听"
                 : L"信号库（" + std::to_wstring(idx.size()) +
                       L" 条，按当前筛选）——点击任意行即调谐收听/收看",
        std::move(tip_p)));
    flux::Props list_p;
    list_p.direction = flux::Direction::column;
    list_p.gap = 2.0f;
    auto list = flux::view(std::move(list_p));
    const std::size_t n_rows = std::min<std::size_t>(idx.size(), 48);
    for (std::size_t k = 0; k < n_rows; ++k) {
        const auto& e = app.signals[idx[k]];
        flux::Props row_p;
        row_p.text_align = flux::Align::start;
        row_p.font_size_pt = 12.0f;
        row_p.bold = e.online;
        row_p.text_color = e.online ? pal.text : pal.text_secondary;
        // 点行=调谐（lambda 拷贝频率值，不引用向量内存——列表会增长）
        const double f = e.mhz;
        row_p.on_click = [&app, f] { tune_to(app, f); };
        list->children.push_back(flux::label(signal_row_text(e), std::move(row_p)));
    }
    flux::Props scroll_p;
    scroll_p.flex_grow = 1.0f;
    page_el->children.push_back(
        flux::scroll_view(std::move(list), std::move(scroll_p)));

    // 频谱（点击信号=调谐，#55）
    const double half = half_bw_mhz(app);
    if (!app.frame.db.empty()) {
        page_el->children.push_back(hackrftool::ui::spectrum_view(
            pal, app.frame.db, app.frame.peak, app.center_mhz - half,
            app.center_mhz + half, {}, app.frame.seq, &app.spec_geom,
            [&app] { on_spectrum_click(app); }));
    }
    return page_el;
}

// ---- 云图页（#54）：原生 GDI 云图窗覆盖内容区（此 WinFlux 占位被覆盖） ------

flux::ElementPtr weather_display(App& app, const flux::Palette& pal) {
    (void)app;   // 云图显示由原生 apt_view 承担（WinFlux 占位被覆盖）
    flux::Props page_p;
    page_p.flex_grow = 1.0f;
    auto page_el = flux::view(std::move(page_p));
    (void)pal;
    return page_el;   // 实际显示由 apt_view（原生窗）承担
}

// ---- 内容区根（WinFlux，每帧重建） -----------------------------------------

void sync_chrome(App& app);   // 定义在原生骨架节（工具栏态 + 状态栏文本）
void save_settings_tick(App& app);   // 设置缓存心跳保存（#57，定义见设置节）
void sigdb_refresh(App& app);   // 信号库弹窗刷新（#62，定义见弹窗节）
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
    save_settings_tick(app);   // 设置缓存（#57）：3s 节流、变化才写盘
    if (app.sigdb_wnd != nullptr) sigdb_refresh(app);   // 弹窗 2Hz 刷新
    // 遥测（#60）：UI 状态变更即记（非周期轮询——变化才是事件）；
    // DSP 数据 1Hz 快照（峰值/静噪/导频/人声/在线台——分析信号质量用）
    {
        std::string st = std::to_string(app.page) + "|" +
                         std::to_string(app.center_mhz) + "|" +
                         std::to_string(app.rate_index) + "|" +
                         std::to_string(app.lna) + "," +
                         std::to_string(app.vga) + "|" +
                         (app.amp ? "1" : "0") + "|" +
                         (app.afc_on ? "1" : "0") + "|" +
                         (app.stereo_opt ? "1" : "0") + "|" +
                         std::to_string(app.fm_bw) + "|" +
                         std::to_string(app.vol);
        if (st != app.last_ui_state) {
            app.last_ui_state = st;
            hackrftool::log::log_telemetry(
                hackrftool::log::Level::info, "UI", "state",
                {{"page", std::to_string(app.page)},
                 {"center", std::to_string(app.center_mhz)},
                 {"rate", std::to_string(app.rate_index)},
                 {"gain", std::to_string(app.lna) + "/" + std::to_string(app.vga)},
                 {"amp", app.amp ? "1" : "0"},
                 {"afc", app.afc_on ? "1" : "0"},
                 {"stereo", app.stereo_opt ? "1" : "0"},
                 {"bw", std::to_string(app.fm_bw)},
                 {"vol", std::to_string(app.vol)}});
        }
        if (app.fm_on.load() && now - app.last_dsp_ms >= 1000) {
            app.last_dsp_ms = now;
            char pk[16], pl[16], vc[16];
            float mx = -999.0f, sum = 0.0f;
            for (const float v : app.frame.db) {
                mx = std::max(mx, v);
                sum += v;
            }
            snprintf(pk, 16, "%.1f", mx);
            snprintf(pl, 16, "%.3f", app.fm_pilot.load());
            snprintf(vc, 16, "%.1f", app.voice_cur);
            hackrftool::log::log_telemetry(
                hackrftool::log::Level::info, "DSP", "fm",
                {{"peak_db", pk},
                 {"pilot", pl},
                 {"voice_db", vc},
                 {"sq", app.squelch_open.load() ? "1" : "0"},
                 {"avg_db", std::to_string(
                                sum / float(std::max<std::size_t>(
                                           app.frame.db.size(), 1)))}});
            if (app.apt_on.load()) {
                // APT 链路诊断（#61）：云图排查三要素——子载波幅度/行同步率/
                // 累计行数。过境判读：sub>0.3 且 sync≈2/s 且 lines 递增=链路
                // 通；sub 高 sync=0=行同步问题；sub 低=无信号/频偏
                char sb[16];
                snprintf(sb, 16, "%.3f", app.apt.subcarrier_level());
                hackrftool::log::log_telemetry(
                    hackrftool::log::Level::info, "APT", "diag",
                    {{"sub", sb},
                     {"sync_ps", std::to_string(app.apt.sync_per_sec())},
                     {"lines", std::to_string(app.apt.lines())},
                     {"synced", app.apt.synced() ? "1" : "0"}});
            }
        }
    }

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

    // AFC（收音页、非扫描）：偏置台自动吸附回中心（107.1→107.0 类修正）
    if (app.fm_on.load() && app.afc_on && app.page == 3 &&
        !app.fm_scan.load() && !app.frame.db.empty() &&
        GetTickCount64() > app.afc_pause_until) {
        const double corr = hackrftool::dsp::afc_correction(
            app.frame.db, 2.0 * half_bw_mhz(app), 10.0f, 0.03, 0.4);
        if (corr != 0.0) {
            const double want = app.center_mhz + corr;
            app.center_mhz = want;
            app.radio_mhz = want;
            if (app.running) (void)app.radio.set_center_hz(want * 1e6);
            wchar_t buf[32];
            swprintf(buf, 32, L"%.6g", want);
            SetWindowTextW(app.edit_radio, buf);
        }
    }

    // 静噪判据（收音链在跑时）：当前帧峰均差 >8dB 视为带内有台
    if (app.fm_on.load()) {
        if (!app.frame.db.empty()) {
            float sum = 0.0f, mx = -999.0f;
            for (const float v : app.frame.db) {
                sum += v;
                mx = std::max(mx, v);
            }
            const bool open = (mx - sum / float(app.frame.db.size())) > 8.0f;
            app.squelch_open.store(open);
        }
    }

    // 云图页：行数增长 → 刷新原生云图窗（2 行/s，代价可忽略）
    if (app.page == 4 && app.apt.lines() != app.apt_seen_lines) {
        app.apt_seen_lines = app.apt.lines();
        app.apt_view.refresh(app.apt);
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
    case 3: content->children.push_back(radio_display(app, pal)); break;
    case 4: content->children.push_back(weather_display(app, pal)); break;
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
    IDC_PAGE3,
    IDC_PAGE4,
    IDC_SWEEP,
    IDC_RECORD,
    IDC_LOCK,
    IDC_EXPORT,
    IDC_CLEAR,
    IDC_APPLYFREQ,
    IDC_SCAN,
    IDC_EDIT_FREQ,
    IDC_EDIT_MON,
    IDC_EDIT_RADIO,
    IDC_COMBO_RATE,
    IDC_COMBO_SYM,
    IDC_COMBO_STATION,
    IDC_COMBO_SAT,
    IDC_TRACK_LNA,
    IDC_TRACK_VGA,
    IDC_TRACK_THR,
    IDC_TRACK_BURST,
    IDC_TRACK_VOL,
    IDC_CHECK_AUTOTRACK,
    IDC_CHECK_MUTE,
    IDC_CHECK_APT,
    IDC_SAVEPNG,
    IDC_RANDOM,
    IDC_COMBO_SIGCAT,
    IDC_COMBO_ONLINE,
    IDC_COMBO_SORT,
    IDC_COMBO_AUDIO,
    IDC_CHECK_AMP,
    IDC_CHECK_AFC,
    IDC_COMBO_BW,
    IDC_CHECK_STEREO,
    IDC_SIGDB,
};

// ---- 统一调谐与页面默认频率（#55） ------------------------------------------

// 按目标频率自动分流：中心/状态栏统一走这里（收音机页语义 radio_mhz 同步）
void tune_to(App& app, double mhz) {
    using hackrftool::dsp::band_of;
    using hackrftool::dsp::SigCat;
    const SigCat cat = band_of(mhz).cat;
    if (cat == SigCat::sat || cat == SigCat::radio) app.radio_mhz = mhz;
    app.center_mhz = mhz;
    app.afc_pause_until = GetTickCount64() + 5000;   // 显式选择后 AFC 让位 5s
    if (app.running) (void)app.radio.set_center_hz(mhz * 1e6);
    wchar_t buf[32];
    swprintf(buf, 32, L"%.4g", mhz);
    if (app.page == 3) SetWindowTextW(app.edit_radio, buf);
    else SetWindowTextW(app.edit_freq, buf);
    app.status = std::wstring(L"已调谐 ") + buf + L" MHz（" + band_of(mhz).name +
                 L"）";
    hackrftool::log::log_telemetry(hackrftool::log::Level::info, "RADIO",
                                   "tune",
                                   {{"mhz", std::to_string(mhz)},
                                    {"band", std::to_string(int(cat))}});
}

// 进入页面时若中心不在该页频段 → 落到场景默认值（#55：不同场景默认中心）
void apply_page_default(App& app) {
    using hackrftool::dsp::SigCat;
    SigCat want;
    switch (app.page) {
    case 3: want = SigCat::radio; break;
    case 4: want = SigCat::sat; break;
    default: want = SigCat::ism; break;
    }
    if (!hackrftool::dsp::in_band(want, app.center_mhz))
        tune_to(app, hackrftool::dsp::default_center(want));
}

// ---- 设置持久化（#57）：界面参数全部缓存 settings.tsv，启动恢复 ----
// App 字段是唯一事实源（各控件 handler 已把值写回 App），capture 只读字段。
[[nodiscard]] hackrftool::app::Settings capture_settings(const App& app) {
    hackrftool::app::Settings s;
    s.page = app.page;
    s.center_mhz = app.center_mhz;
    s.radio_mhz = app.radio_mhz;
    s.rate_index = app.rate_index;
    s.lna = app.lna;
    s.vga = app.vga;
    s.amp = app.amp;
    s.auto_track = app.auto_track;
    s.afc_on = app.afc_on;
    s.stereo_opt = app.stereo_opt;
    s.spec_zoom_idx = app.spec_zoom_idx;
    s.threshold = app.threshold;
    s.burst_thr = app.burst_thr;
    s.symrate_idx = app.symrate_idx;
    s.fm_bw = app.fm_bw;
    s.audio_dev = app.audio_dev;
    s.vol = app.vol;
    s.sig_cat = app.sig_cat;
    s.sig_online = app.sig_online;
    s.sig_sort = app.sig_sort;
    return s;
}

// 恢复：写 App 字段 + 同步原生控件（控件已创建后调用）
void restore_settings(App& app, const hackrftool::app::Settings& s) {
    app.page = s.page;
    app.center_mhz = s.center_mhz;
    app.radio_mhz = s.radio_mhz;
    app.rate_index = s.rate_index;
    app.lna = s.lna;
    app.vga = s.vga;
    app.amp = s.amp;
    app.auto_track = s.auto_track;
    app.afc_on = s.afc_on;
    app.stereo_opt = s.stereo_opt;
    app.spec_zoom_idx = s.spec_zoom_idx;
    app.threshold = s.threshold;
    app.burst_thr = s.burst_thr;
    app.symrate_idx = s.symrate_idx;
    app.fm_bw = s.fm_bw;
    app.audio_dev = s.audio_dev;
    app.vol = s.vol;
    app.sig_cat = s.sig_cat;
    app.sig_online = s.sig_online;
    app.sig_sort = s.sig_sort;
    SetWindowTextW(app.edit_freq, wd1(app.center_mhz).c_str());
    SetWindowTextW(app.edit_radio, wd1(app.radio_mhz).c_str());
    SendMessageW(app.combo_rate, CB_SETCURSEL, app.rate_index, 0);
    SendMessageW(app.combo_symrate, CB_SETCURSEL, app.symrate_idx, 0);
    SendMessageW(app.track_lna, TBM_SETPOS, TRUE, LPARAM(app.lna));
    SendMessageW(app.track_vga, TBM_SETPOS, TRUE, LPARAM(app.vga));
    SetWindowTextW(app.lbl_lna, (L"LNA " + std::to_wstring(app.lna)).c_str());
    SetWindowTextW(app.lbl_vga, (L"VGA " + std::to_wstring(app.vga)).c_str());
    SendMessageW(app.check_amp, BM_SETCHECK,
                 app.amp ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(app.check_autotrack, BM_SETCHECK,
                 app.auto_track ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(app.check_afc, BM_SETCHECK,
                 app.afc_on ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(app.check_stereo, BM_SETCHECK,
                 app.stereo_opt ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(app.track_threshold, TBM_SETPOS, TRUE, LPARAM(int(app.threshold)));
    SendMessageW(app.track_burst, TBM_SETPOS, TRUE, LPARAM(int(app.burst_thr)));
    SetWindowTextW(app.lbl_thr, (std::to_wstring(int(app.threshold)) + L" dB").c_str());
    SetWindowTextW(app.lbl_burst, (std::to_wstring(int(app.burst_thr)) + L" dB").c_str());
    SendMessageW(app.combo_bw, CB_SETCURSEL, app.fm_bw, 0);
    SendMessageW(app.combo_sigcat, CB_SETCURSEL, app.sig_cat, 0);
    SendMessageW(app.combo_online, CB_SETCURSEL, app.sig_online, 0);
    SendMessageW(app.combo_sort, CB_SETCURSEL, app.sig_sort, 0);
    if (app.combo_audio != nullptr) {
        // 设备列表开机可能变：越界回退系统默认（combo 下标 = audio_dev+1）
        const int cnt = int(SendMessageW(app.combo_audio, CB_GETCOUNT, 0, 0));
        if (app.audio_dev >= cnt - 1) app.audio_dev = -1;
        SendMessageW(app.combo_audio, CB_SETCURSEL, app.audio_dev + 1, 0);
    }
    SendMessageW(app.track_vol, TBM_SETPOS, TRUE, LPARAM(app.vol));
    SetWindowTextW(app.lbl_vol, (L"音量 " + std::to_wstring(app.vol)).c_str());
    app.waveout.set_volume(float(app.vol) / 100.0f);
    app.gains_pinned = true;   // 用户上次手调的增益优先于广播默认
    app.last_settings = hackrftool::app::serialize(capture_settings(app));
}

[[nodiscard]] std::string read_text_file(const std::wstring& path) {
    std::FILE* f = _wfopen(path.c_str(), L"rb");
    if (f == nullptr) return {};
    std::string buf;
    char tmp[512];
    for (size_t g; (g = fread(tmp, 1, sizeof tmp, f)) > 0;) buf.append(tmp, g);
    fclose(f);
    return buf;
}

void write_text_file(const std::wstring& path, const std::string& text) {
    std::FILE* f = _wfopen(path.c_str(), L"wb");
    if (f == nullptr) return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

// 心跳节流保存（build 调用）：序列化比对变化才写盘
void save_settings_tick(App& app) {
    if (app.settings_hold) return;
    const auto now = GetTickCount64();
    if (now - app.last_set_ms < 3000) return;
    app.last_set_ms = now;
    const std::string s = hackrftool::app::serialize(capture_settings(app));
    if (s == app.last_settings) return;
    app.last_settings = s;
    write_text_file(exe_dir_path("settings.tsv"), s);
}

// 频谱点击调谐（#55：点信号即调谐；全景点击=跳去专门收听该段）。
// 频率由 spec_geom（paint 每帧回写）与 host.last_click_pos() 换算。
void on_spectrum_click(App& app) {
    const auto [cx, cy] = app.host.last_click_pos();
    const auto& g = app.spec_geom;
    const double t =
        std::clamp(double(cx - g.x) / double(std::max(g.w, 1.0f)), 0.0, 1.0);
    const double mhz = g.lo_mhz + t * (g.hi_mhz - g.lo_mhz);
    if (app.sweep_on == 1) {
        // 全频段全景：退出扫描、单窗居中点击频率——"点强频段专门收听"
        const LRESULT st = SendMessageW(app.toolbar, TB_GETSTATE, IDC_SWEEP, 0);
        if ((st & TBSTATE_CHECKED) != 0) {
            set_sweep_live(app, false);
            app.sweep_on = 0;
        }
        tune_to(app, std::clamp(mhz, 2400.0, 2483.5));
        app.status += L"（已从全景跳转单窗）";
        return;
    }
    if (app.page == 3) {
        // 收音页：峰值吸附——点击位置 ±0.15MHz 内最强显著峰即台中心
        //（手点像素必偏几十 kHz，吸附后精确对台；配合 5s AFC 暂停=所点即所得）
        const double click_off = mhz - app.center_mhz;
        const double off = hackrftool::dsp::peak_snap(
            app.frame.db, 2.0 * half_bw_mhz(app), click_off, 0.15, 8.0f);
        tune_to(app, app.center_mhz + off);
        return;
    }
    const bool relock = app.mon_lock_mhz > 0.0 && !app.auto_track;
    tune_to(app, mhz);
    if (relock)
        app.monitor.set_fixed_bin(mhz_to_bin(
            app.mon_lock_mhz, app.center_mhz, 2.0 * half_bw_mhz(app)));
}

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

void radio(HDC dc) {
    // 喇叭：方箱 + 锥形声波
    HPEN p = CreatePen(PS_SOLID, 2, RGB(180, 60, 120));
    const HGDIOBJ old = SelectObject(dc, p);
    HBRUSH b = CreateSolidBrush(RGB(180, 60, 120));
    RECT rc{4, 10, 9, 15};
    FillRect(dc, &rc, b);
    DeleteObject(b);
    POINT pts[3] = {{9, 10}, {13, 6}, {13, 19}};
    const HGDIOBJ ob = SelectObject(dc, b = CreateSolidBrush(RGB(180, 60, 120)));
    Polygon(dc, pts, 3);
    DeleteObject(b);
    SelectObject(dc, ob);
    MoveToEx(dc, 15, 8, nullptr);
    Arc(dc, 13, 5, 20, 19, 15, 6, 15, 18);
    SelectObject(dc, old);
    DeleteObject(p);
}

void satellite(HDC dc) {
    // 卫星：本体 + 两翼太阳板 + 下行波束
    HPEN p = CreatePen(PS_SOLID, 2, RGB(60, 130, 190));
    const HGDIOBJ old = SelectObject(dc, p);
    Rectangle(dc, 9, 8, 15, 15);
    MoveToEx(dc, 4, 10, nullptr);
    LineTo(dc, 9, 10);
    MoveToEx(dc, 4, 13, nullptr);
    LineTo(dc, 9, 13);
    MoveToEx(dc, 15, 10, nullptr);
    LineTo(dc, 20, 10);
    MoveToEx(dc, 15, 13, nullptr);
    LineTo(dc, 20, 13);
    MoveToEx(dc, 12, 15, nullptr);
    LineTo(dc, 8, 20);
    SelectObject(dc, old);
    DeleteObject(p);
}

void dice(HDC dc) {
    // 骰子（随机收听）：方框 + 五点
    HPEN p = CreatePen(PS_SOLID, 2, RGB(150, 90, 160));
    const HGDIOBJ old = SelectObject(dc, p);
    Rectangle(dc, 5, 5, 19, 19);
    HBRUSH b = CreateSolidBrush(RGB(150, 90, 160));
    const POINT pts[5] = {{8, 8}, {16, 8}, {12, 12}, {8, 16}, {16, 16}};
    for (const POINT pt : pts) {
        RECT rc{pt.x - 1, pt.y - 1, pt.x + 2, pt.y + 2};
        FillRect(dc, &rc, b);
    }
    DeleteObject(b);
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
    ICON_RADIO,
    ICON_SAT,
    ICON_DICE,
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
        icon::radio,       icon::satellite,    icon::dice,
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
        tb_btn(ICON_RADIO, IDC_PAGE3, BTNS_CHECK | BTNS_GROUP | BTNS_AUTOSIZE, L"收音"),
        tb_btn(ICON_SAT, IDC_PAGE4, BTNS_CHECK | BTNS_GROUP | BTNS_AUTOSIZE, L"云图"),
        tb_btn(0, 0, BTNS_SEP, nullptr),
        tb_btn(ICON_SWEEP, IDC_SWEEP, BTNS_CHECK | BTNS_AUTOSIZE, L"全频段"),
        tb_btn(ICON_REC, IDC_RECORD, BTNS_CHECK | BTNS_AUTOSIZE, L"录制 IQ"),
        tb_btn(0, 0, BTNS_SEP, nullptr),
        tb_btn(ICON_LOCK, IDC_LOCK, BTNS_AUTOSIZE, L"锁定"),
        tb_btn(ICON_EXPORT, IDC_EXPORT, BTNS_AUTOSIZE, L"导出 CSV"),
        tb_btn(ICON_CLEAR, IDC_CLEAR, BTNS_AUTOSIZE, L"清空"),
        tb_btn(ICON_APPLY, IDC_APPLYFREQ, BTNS_AUTOSIZE, L"应用频率"),
        tb_btn(ICON_SAT, IDC_SCAN, BTNS_AUTOSIZE, L"扫描信号"),
        tb_btn(ICON_DICE, IDC_RANDOM, BTNS_AUTOSIZE, L"随机收听"),
        tb_btn(ICON_SAT, IDC_SIGDB, BTNS_CHECK | BTNS_AUTOSIZE, L"信号库"),
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
    for (const wchar_t* r : {L"2", L"8", L"10", L"16", L"20"})
        SendMessageW(app.combo_rate, CB_ADDSTRING, 0, LPARAM(r));
    SendMessageW(app.combo_rate, CB_SETCURSEL, WPARAM(app.rate_index), 0);
    slot(app.row_common, app.combo_rate, 64, true);

    app.check_amp = make_ctl(app, WC_BUTTONW, L"功放", BS_AUTOCHECKBOX | WS_TABSTOP,
                             0, IDC_CHECK_AMP);
    slot(app.row_common, app.check_amp, 52);
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

    // 收音机页（#53/#55）：频段类别/在线/排序筛选 + 频率（后备）+ 音量/静音
    slot(app.row_radio,
         make_ctl(app, WC_STATICW, L"带宽", SS_LEFT | SS_CENTERIMAGE, 0, 0), 36);
    app.combo_bw = make_ctl(app, WC_COMBOBOXW, nullptr,
                            CBS_DROPDOWNLIST | WS_TABSTOP, 0, IDC_COMBO_BW);
    for (const wchar_t* b : {L"±120k 广播", L"±80k", L"±50k 窄带"})
        SendMessageW(app.combo_bw, CB_ADDSTRING, 0, LPARAM(b));
    SendMessageW(app.combo_bw, CB_SETCURSEL, 0, 0);
    slot(app.row_radio, app.combo_bw, 84, true);
    slot(app.row_radio,
         make_ctl(app, WC_STATICW, L"类别", SS_LEFT | SS_CENTERIMAGE, 0, 0), 36);
    app.combo_sigcat = make_ctl(app, WC_COMBOBOXW, nullptr,
                                CBS_DROPDOWNLIST | WS_TABSTOP, 0, IDC_COMBO_SIGCAT);
    for (const wchar_t* s : {L"FM 广播", L"NOAA 卫星", L"2.4G ISM", L"全部"})
        SendMessageW(app.combo_sigcat, CB_ADDSTRING, 0, LPARAM(s));
    SendMessageW(app.combo_sigcat, CB_SETCURSEL, 0, 0);
    slot(app.row_radio2, app.combo_sigcat, 88, true);   // 第二行：筛选组（B4）
    app.combo_online = make_ctl(app, WC_COMBOBOXW, nullptr,
                                CBS_DROPDOWNLIST | WS_TABSTOP, 0, IDC_COMBO_ONLINE);
    for (const wchar_t* s : {L"仅在线", L"全部"})
        SendMessageW(app.combo_online, CB_ADDSTRING, 0, LPARAM(s));
    SendMessageW(app.combo_online, CB_SETCURSEL, 0, 0);
    slot(app.row_radio2, app.combo_online, 66, true);
    app.combo_sort = make_ctl(app, WC_COMBOBOXW, nullptr,
                              CBS_DROPDOWNLIST | WS_TABSTOP, 0, IDC_COMBO_SORT);
    for (const wchar_t* s : {L"按强度", L"按频率"})
        SendMessageW(app.combo_sort, CB_ADDSTRING, 0, LPARAM(s));
    SendMessageW(app.combo_sort, CB_SETCURSEL, 0, 0);
    slot(app.row_radio2, app.combo_sort, 66, true);
    slot(app.row_radio2,
         make_ctl(app, WC_STATICW, L"输出", SS_LEFT | SS_CENTERIMAGE, 0, 0), 34);
    app.combo_audio = make_ctl(app, WC_COMBOBOXW, nullptr,
                               CBS_DROPDOWNLIST | WS_TABSTOP, 0, IDC_COMBO_AUDIO);
    SendMessageW(app.combo_audio, CB_ADDSTRING, 0, LPARAM(L"系统默认"));
    for (const auto& d : hackrftool::audio::WaveOut::enum_devices())
        SendMessageW(app.combo_audio, CB_ADDSTRING, 0, LPARAM(d.c_str()));
    SendMessageW(app.combo_audio, CB_SETCURSEL, 0, 0);
    slot(app.row_radio2, app.combo_audio, 150, true);
    slot(app.row_radio,
         make_ctl(app, WC_STATICW, L"频率 MHz", SS_LEFT | SS_CENTERIMAGE, 0, 0), 60);
    app.edit_radio = make_ctl(app, WC_EDITW, L"98.0",
                              ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 0,
                              IDC_EDIT_RADIO);
    slot(app.row_radio, app.edit_radio, 72);
    app.track_vol = make_ctl(app, TRACKBAR_CLASSW, nullptr,
                             TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 0, IDC_TRACK_VOL);
    SendMessageW(app.track_vol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(app.track_vol, TBM_SETPOS, TRUE, 80);
    slot(app.row_radio, app.track_vol, 100);
    app.lbl_vol = make_ctl(app, WC_STATICW, L"音量 80", SS_LEFT | SS_CENTERIMAGE, 0, 0);
    slot(app.row_radio, app.lbl_vol, 56);
    app.check_mute = make_ctl(app, WC_BUTTONW, L"静音", BS_AUTOCHECKBOX | WS_TABSTOP,
                              0, IDC_CHECK_MUTE);
    slot(app.row_radio, app.check_mute, 52);
    app.check_afc = make_ctl(app, WC_BUTTONW, L"自动微调", BS_AUTOCHECKBOX | WS_TABSTOP,
                             0, IDC_CHECK_AFC);
    SendMessageW(app.check_afc, BM_SETCHECK, BST_CHECKED, 0);
    slot(app.row_radio2, app.check_afc, 76);
    // 立体声选项（B4）：不勾=强制单声（弱导频/噪声场景更稳）
    app.check_stereo = make_ctl(app, WC_BUTTONW, L"立体声",
                                BS_AUTOCHECKBOX | WS_TABSTOP, 0, IDC_CHECK_STEREO);
    SendMessageW(app.check_stereo, BM_SETCHECK, BST_CHECKED, 0);
    slot(app.row_radio2, app.check_stereo, 62);
    // 「扫描电台」为动作按钮，归工具栏行 1（布局契约：按钮在工具栏）

    // 云图页（#54）：卫星预设 + 记录 + 保存
    slot(app.row_weather,
         make_ctl(app, WC_STATICW, L"卫星", SS_LEFT | SS_CENTERIMAGE, 0, 0), 40);
    app.combo_sat = make_ctl(app, WC_COMBOBOXW, nullptr,
                             CBS_DROPDOWNLIST | WS_TABSTOP, 0, IDC_COMBO_SAT);
    for (const wchar_t* s : {L"NOAA 19 · 137.100", L"NOAA 15 · 137.620",
                             L"NOAA 18 · 137.9125"})
        SendMessageW(app.combo_sat, CB_ADDSTRING, 0, LPARAM(s));
    SendMessageW(app.combo_sat, CB_SETCURSEL, 1, 0);   // 默认 NOAA 15
    slot(app.row_weather, app.combo_sat, 130, true);
    app.check_apt = make_ctl(app, WC_BUTTONW, L"记录", BS_AUTOCHECKBOX | WS_TABSTOP,
                             0, IDC_CHECK_APT);
    SendMessageW(app.check_apt, BM_SETCHECK, BST_CHECKED, 0);   // 默认记录
    slot(app.row_weather, app.check_apt, 52);
    HWND btn_save = make_ctl(app, WC_BUTTONW, L"保存 PNG", BS_PUSHBUTTON | WS_TABSTOP,
                             0, IDC_SAVEPNG);
    slot(app.row_weather, btn_save, 84);
}

void create_statusbar(App& app) {
    app.statusbar = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr,
                                    WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0,
                                    0, app.main_wnd, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
}

// 内容区目标矩形（layout 与几何看门狗共用——判定与摆放必须同一算法）
// 设置行行数（host_target 与 layout 共用——几何看门狗铁律：同一算法）
int settings_rows(const App& app) { return app.page == 3 ? 2 : 1; }

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
    out->top = (tb.bottom - tb.top) + row_h * settings_rows(app);
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
    const int row_y = tb_h + (34 * s - ctl_h) / 2;
    const auto place = [&](std::vector<App::CtlSlot>& v, int line) {
        int px = 8 * s;
        const int py = row_y + line * 34 * s;
        for (const auto& c : v) {
            const int ch = c.drop ? 130 * s : ctl_h;   // 组合框高度含下拉列表
            MoveWindow(c.h, px, py, c.w * s, ch, FALSE);
            px += c.w * s + 6 * s;
        }
    };
    place(app.row_common, 0);
    const bool mon = app.page == 1, cap = app.page == 2;
    const bool rad = app.page == 3, wx = app.page == 4;
    for (const auto& c : app.row_monitor)
        ShowWindow(c.h, mon ? SW_SHOW : SW_HIDE);
    for (const auto& c : app.row_capture)
        ShowWindow(c.h, cap ? SW_SHOW : SW_HIDE);
    for (const auto& c : app.row_radio)
        ShowWindow(c.h, rad ? SW_SHOW : SW_HIDE);
    for (const auto& c : app.row_radio2)
        ShowWindow(c.h, rad ? SW_SHOW : SW_HIDE);
    for (const auto& c : app.row_weather)
        ShowWindow(c.h, wx ? SW_SHOW : SW_HIDE);
    if (mon) place(app.row_monitor, 0);
    if (cap) place(app.row_capture, 0);
    if (rad) {
        place(app.row_radio, 0);
        place(app.row_radio2, 1);   // 收音页第二行：筛选/音频选项（B4）
    }
    if (wx) place(app.row_weather, 0);

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
    if (host_target(app, &ht)) {
        MoveWindow(app.host.hwnd(), ht.left, ht.top, ht.right - ht.left,
                   ht.bottom - ht.top, FALSE);
        // APT 云图窗覆盖内容区（云图页；原生 GDI 位图显示，WinFlux 无位图接口）
        if (app.apt_view.hwnd() != nullptr) {
            MoveWindow(app.apt_view.hwnd(), ht.left, ht.top, ht.right - ht.left,
                       ht.bottom - ht.top, FALSE);
            ShowWindow(app.apt_view.hwnd(), app.page == 4 ? SW_SHOW : SW_HIDE);
        }
    }
}

// ---- 信号库非模态弹窗（#62）：任意页快速选台，双击行=调谐 -------------
// 数据源=app.signals + 当前筛选（收音页下拉的同一组条件）；2Hz 心跳刷新，
// 库内容变化（扫描落库）才重建行。非模态：主窗与弹窗可同时操作。
// （不进匿名 namespace：build 心跳的前向声明需同一函数）

void sigdb_refresh(App& app) {
    if (app.sigdb_list == nullptr) return;
    using hackrftool::dsp::SigCat;
    const SigCat cat = app.sig_cat == 0   ? SigCat::radio
                       : app.sig_cat == 1 ? SigCat::sat
                       : app.sig_cat == 2 ? SigCat::ism
                                          : SigCat::other;
    const auto rows = hackrftool::dsp::filter_signals(   // 返回下标集
        app.signals, cat, app.sig_online != 0, app.sig_sort == 1);
    const unsigned stamp =
        unsigned(rows.size() * 31 +
                 (rows.empty() ? 0
                               : unsigned(app.signals[rows[0]].mhz * 7)));
    if (stamp == app.sigdb_stamp) return;   // 无变化不重建（防闪+省 CPU）
    app.sigdb_stamp = stamp;
    SendMessageW(app.sigdb_list, LVM_DELETEALLITEMS, 0, 0);
    wchar_t buf[64];
    int shown = 0;
    for (const std::size_t ri : rows) {
        if (shown++ >= 500) break;
        const auto& e = app.signals[ri];
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        swprintf(buf, 64, L"%.4g", e.mhz);
        it.pszText = buf;
        it.iItem = shown - 1;
        const int idx = int(SendMessageW(app.sigdb_list, LVM_INSERTITEMW, 0,
                                         LPARAM(&it)));
        swprintf(buf, 64, L"%.1f dB", e.db);
        it.iItem = idx; it.iSubItem = 1; it.pszText = buf;
        SendMessageW(app.sigdb_list, LVM_SETITEMW, 0, LPARAM(&it));
        wcscpy(buf, e.online ? L"● 在线" : L"○ 离线");
        it.iSubItem = 2;
        SendMessageW(app.sigdb_list, LVM_SETITEMW, 0, LPARAM(&it));
        wcscpy(buf, hackrftool::dsp::band_of(e.mhz).name);
        it.iSubItem = 3;
        SendMessageW(app.sigdb_list, LVM_SETITEMW, 0, LPARAM(&it));
    }
}

LRESULT CALLBACK sigdb_wndproc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(wnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NOTIFY: {
        const auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->idFrom == 5001 && nm->code == NM_DBLCLK) {
            const auto* di =
                reinterpret_cast<const NMITEMACTIVATE*>(nm);
            wchar_t f[32];
            LVITEMW it{};
            it.mask = LVIF_TEXT;
            it.iItem = di->iItem;
            it.pszText = f;
            it.cchTextMax = 32;
            if (SendMessageW(app->sigdb_list, LVM_GETITEMTEXTW,
                             WPARAM(di->iItem), LPARAM(&it)) > 0) {
                const double mhz = std::wcstod(f, nullptr);
                tune_to(*app, mhz);
                hackrftool::log::log_telemetry(
                    hackrftool::log::Level::info, "SIGDB", "pick",
                    {{"mhz", std::to_string(mhz)}});
            }
        }
        return 0;
    }
    case WM_SIZE:
        if (app != nullptr && app->sigdb_list != nullptr)
            MoveWindow(app->sigdb_list, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_DESTROY:
        if (app != nullptr) {
            app->sigdb_wnd = nullptr;
            app->sigdb_list = nullptr;
        }
        return 0;
    default:
        return DefWindowProcW(wnd, msg, wp, lp);
    }
}

void sigdb_toggle(App& app) {
    if (app.sigdb_wnd != nullptr) {   // 已开 → 关
        DestroyWindow(app.sigdb_wnd);
        hackrftool::log::log_telemetry(hackrftool::log::Level::info, "SIGDB",
                                       "close", {});
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = sigdb_wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"HackRFSigdb";
        RegisterClassW(&wc);
        registered = true;
    }
    HWND wnd = CreateWindowW(L"HackRFSigdb", L"信号库（双击=调谐）",
                             WS_OVERLAPPEDWINDOW, 80, 120, 420, 420,
                             app.main_wnd, nullptr, GetModuleHandleW(nullptr),
                             nullptr);
    if (wnd == nullptr) return;
    SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, nullptr,
                              WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                              0, 0, 400, 400, wnd,
                              reinterpret_cast<HMENU>(5001),
                              GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT |
                                              LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<LPWSTR>(L"频率 MHz");
    col.cx = 90;
    SendMessageW(lv, LVM_INSERTCOLUMNW, 0, LPARAM(&col));
    col.pszText = const_cast<LPWSTR>(L"强度");
    col.cx = 70;
    SendMessageW(lv, LVM_INSERTCOLUMNW, 1, LPARAM(&col));
    col.pszText = const_cast<LPWSTR>(L"在线");
    col.cx = 70;
    SendMessageW(lv, LVM_INSERTCOLUMNW, 2, LPARAM(&col));
    col.pszText = const_cast<LPWSTR>(L"频段");
    col.cx = 120;
    SendMessageW(lv, LVM_INSERTCOLUMNW, 3, LPARAM(&col));
    app.sigdb_wnd = wnd;
    app.sigdb_list = lv;
    app.sigdb_stamp = 0;
    ShowWindow(wnd, SW_SHOW);
    hackrftool::log::log_telemetry(hackrftool::log::Level::info, "SIGDB",
                                   "open", {{"entries",
                                             std::to_string(app.signals.size())}});
    sigdb_refresh(app);
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
            const int ids[5] = {IDC_PAGE0, IDC_PAGE1, IDC_PAGE2, IDC_PAGE3,
                                IDC_PAGE4};
            for (int i = 0; i < 5; ++i)
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
    // 全量点击/命令事件（#59）：入口统一记录。code 白名单防编辑框通知
    // 刷屏——EN_CHANGE/EN_UPDATE 由程序自身写控件触发（AFC 每 tick 写
    // 频率框），不是用户操作；0=按钮/菜单/工具栏，1=CBN_SELCHANGE
    if (code == 0 || code == 1)
        hackrftool::log::log_telemetry(
            hackrftool::log::Level::info, "UI", "cmd",
            {{"id", std::to_string(id)}, {"code", std::to_string(code)}});
    switch (id) {
    case IDC_STARTSTOP: toggle_rx(app); break;
    case IDC_PAGE0:
    case IDC_PAGE1:
    case IDC_PAGE2:
        app.page = id - IDC_PAGE0;
        ensure_fm(app, false);
        apply_page_default(app);   // 场景默认频率（#55）
        layout(app);
        break;
    case IDC_PAGE3:
    case IDC_PAGE4:
        app.page = id - IDC_PAGE0;
        ensure_fm(app, app.running);
        update_apt_on(app);
        apply_page_default(app);
        layout(app);
        break;
    case IDC_SWEEP: {
        const LRESULT st = SendMessageW(app.toolbar, TB_GETSTATE, IDC_SWEEP, 0);
        const bool on = (st & TBSTATE_CHECKED) != 0;
        if (on) ensure_fm(app, false);   // B3：开扫描先停音频链（跳频噪声无意义）
        if (on && app.rate_index != 4) {
            // 全频段扫描按 20 Msps 窗设计（5 段×17.5 MHz），强制顶档
            app.rate_index = 4;
            reconfigure_rx(app, current_radio_cfg(app));
            SendMessageW(app.combo_rate, CB_SETCURSEL, 4, 0);
        }
        set_sweep_live(app, on && app.running);
        app.sweep_on = on ? 1 : 0;
        hackrftool::log::log_telemetry(hackrftool::log::Level::info, "RADIO",
                                       "sweep", {{"on", on ? "1" : "0"}});
        break;
    }
    case IDC_RECORD: record_toggle(app); break;
    case IDC_LOCK: monitor_lock(app); break;
    case IDC_EXPORT: export_csv_dialog(app); break;
    case IDC_CLEAR: clear_bursts(app); break;
    case IDC_APPLYFREQ: apply_center_freq(app); break;
    case IDC_SCAN: start_scan(app); break;
    case IDC_RANDOM: random_listen(app); break;
    case IDC_SIGDB: sigdb_toggle(app); break;
    case IDC_COMBO_SIGCAT:
        if (code == CBN_SELCHANGE) {
            const int i = int(SendMessageW(app.combo_sigcat, CB_GETCURSEL, 0, 0));
            if (i >= 0) {
                app.sig_cat = i;
                app.status = L"筛选已切换（列表即时生效）";
            }
        }
        break;
    case IDC_COMBO_ONLINE:
        if (code == CBN_SELCHANGE)
            app.sig_online = int(SendMessageW(app.combo_online, CB_GETCURSEL, 0, 0));
        break;
    case IDC_COMBO_SORT:
        if (code == CBN_SELCHANGE)
            app.sig_sort = int(SendMessageW(app.combo_sort, CB_GETCURSEL, 0, 0));
        break;
    case IDC_COMBO_AUDIO:
        if (code == CBN_SELCHANGE && app.fm_on.load()) {
            // 切设备 = 重开音频链（设备在 open 时绑定）
            app.audio_dev =
                int(SendMessageW(app.combo_audio, CB_GETCURSEL, 0, 0)) - 1;
            ensure_fm(app, false);
            Sleep(50);
            ensure_fm(app, true);
        } else if (code == CBN_SELCHANGE) {
            app.audio_dev =
                int(SendMessageW(app.combo_audio, CB_GETCURSEL, 0, 0)) - 1;
        }
        break;
    case IDC_COMBO_SAT:
        if (code == CBN_SELCHANGE) {
            // NOAA 19/15/18 下行频率（#54 起配合 APT 解码）
            static const double kSatMhz[3] = {137.100, 137.620, 137.9125};
            const int i = int(SendMessageW(app.combo_sat, CB_GETCURSEL, 0, 0));
            if (i >= 0 && i < 3 && app.running) {
                (void)app.radio.set_center_hz(kSatMhz[i] * 1e6);
                app.center_mhz = kSatMhz[i];
                app.radio_mhz = kSatMhz[i];
                app.status = L"已调谐 " + wd1(kSatMhz[i]) + L" MHz（NOAA APT）";
            }
        }
        break;
    case IDC_CHECK_MUTE:
        if (code == BN_CLICKED)
            app.waveout.set_mute(SendMessageW(app.check_mute, BM_GETCHECK, 0, 0) ==
                                 BST_CHECKED);
        break;
    case IDC_CHECK_APT:
        if (code == BN_CLICKED) update_apt_on(app);
        break;
    case IDC_CHECK_AFC:
        if (code == BN_CLICKED)
            app.afc_on = SendMessageW(app.check_afc, BM_GETCHECK, 0, 0) == BST_CHECKED;
        break;
    case IDC_CHECK_STEREO:
        if (code == BN_CLICKED) {
            app.stereo_opt =
                SendMessageW(app.check_stereo, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (app.fm_rx)
                app.fm_rx->set_force_mono(!app.stereo_opt);   // 运行中热切换
            app.status = app.stereo_opt ? L"立体声解码开启"
                                        : L"强制单声道（弱导频场景更稳）";
        }
        break;
    case IDC_COMBO_BW:
        if (code == CBN_SELCHANGE) {
            const int i = int(SendMessageW(app.combo_bw, CB_GETCURSEL, 0, 0));
            if (i >= 0 && i < 3) {
                app.fm_bw = i;
                static const double kFmBw2[3] = {120e3, 80e3, 50e3};
                if (app.fm_rx) app.fm_rx->set_bandwidth(kFmBw2[size_t(i)]);
                app.status = i == 0 ? L"带宽 ±120k（FM 广播）"
                           : i == 1 ? L"带宽 ±80k"
                                    : L"带宽 ±50k（窄带/卫星 APT）";
            }
        }
        break;
    case IDC_CHECK_AMP:
        if (code == BN_CLICKED) {
            app.amp = SendMessageW(app.check_amp, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (app.running) {
                hackrftool::radio::RadioConfig cfg = current_radio_cfg(app);
                std::string err;
                if (!app.radio.apply(cfg, &err))
                    app.status = L"配置失败: " + widen(err);
            }
        }
        break;
    case IDC_SAVEPNG:
        if (code == BN_CLICKED) save_apt_dialog(app);
        break;
    case IDC_COMBO_RATE:
        if (code == CBN_SELCHANGE) {
            const int i = int(SendMessageW(app.combo_rate, CB_GETCURSEL, 0, 0));
            if (app.sweep_on == 1 && i != 4) {
                // B2：全景拼接按 20Msps 5 段设计，低档率频标全错——扫描中锁定
                SendMessageW(app.combo_rate, CB_SETCURSEL, 4, 0);
                app.status = L"全频段扫描期间采样率锁定 20 Msps（先退出全频段）";
                break;
            }
            if (i >= 0 && i < 5) {
                app.rate_index = i;
                reconfigure_rx(app, current_radio_cfg(app));
                // 收音链运行中换采样率：按新率重建解调器（带宽档保持——
                // B1：漏传第 4 参会静默回 ±120k，与带宽下拉显示不一致）
                if (app.fm_on.load()) {
                    app.iq_ring.clear();
                    static const double kBw[3] = {120e3, 80e3, 50e3};
                    app.fm_rx = std::make_unique<hackrftool::dsp::FmReceiver>(
                        kRatesMsps[size_t(i)] * 1e6, 80.0, !app.stereo_opt,
                        kBw[size_t(app.fm_bw)]);
                    app.fm_rx->set_audio_callback(&fm_audio_cb, &app);
                }
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
    const int ctl_id = ctl != nullptr ? GetDlgCtrlID(ctl) : 0;
    hackrftool::log::log_telemetry(hackrftool::log::Level::info, "UI",
                                   "scroll", {{"ctl", std::to_string(ctl_id)}});
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
    } else if (ctl == app.track_vol) {
        const int v = int(SendMessageW(ctl, TBM_GETPOS, 0, 0));
        app.vol = v;
        app.waveout.set_volume(float(v) / 100.0f);
        SetWindowTextW(app.lbl_vol, (L"音量 " + std::to_wstring(v)).c_str());
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
            for (const auto* v : {&app->row_common, &app->row_monitor,
                                  &app->row_capture, &app->row_radio,
                                  &app->row_radio2, &app->row_weather})
                for (const auto& c : *v)
                    if (c.h != nullptr) SendMessageW(c.h, WM_SETFONT,
                                                     WPARAM(app->font), TRUE);
            layout(*app);
        }
        return 0;
    }
    case WM_CLOSE: DestroyWindow(wnd); return 0;
    case WM_DESTROY:
        if (app != nullptr && !app->settings_hold)   // 设置缓存（#57）：退出兜底落盘
            write_text_file(exe_dir_path("settings.tsv"),
                            hackrftool::app::serialize(capture_settings(*app)));
        hackrftool::log::log_telemetry(
            hackrftool::log::Level::info, "LIFE", "app.stop",
            {{"events", std::to_string(
                           hackrftool::log::Logger::instance().count())}});
        hackrftool::log::Logger::instance().close();
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd_line, int) {
    SetUnhandledExceptionFilter(write_crash_dump);   // 崩溃自动落 dump

    // 遥测日志（#59）：全量 UI/事件/数据记录——脚本断言与问题分析用
    hackrftool::log::Logger::instance().open(
        exe_dir_path("hackrftool.jsonl"));
    hackrftool::log::log_telemetry(hackrftool::log::Level::info, "LIFE",
                                   "app.start", {{"build", __DATE__ " " __TIME__}});

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
    // 信号库加载（上次扫描结果开机即用，#55）
    app.signals = hackrftool::dsp::load_signals(
        exe_dir_path("signals.tsv").c_str());

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
        if (wcscmp(cmd_line, L"autoradio") == 0) {
            auto_start = true;
            app.page = 3;
            app.center_mhz = 98.0;
        }
        if (wcscmp(cmd_line, L"autowx") == 0) {
            auto_start = true;
            app.page = 4;
            app.center_mhz = 137.620;
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
            app.settings_hold = true;   // 自测不写设置（防污染用户参数）
        }
        if (wcscmp(cmd_line, L"selftestsweep") == 0) {
            auto_start = true;
            selftest = true;
            app.settings_hold = true;
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
    // 设置恢复（#57）：仅交互启动加载——命令行自测模式断言依赖出厂默认。
    // 在控件创建后、auto_start 配置前：restore 只写字段/控件，设备侧由
    // auto_start 的 apply_radio 或用户点「开始」时按 App 字段生效。
    if (!auto_start && !selftest) {
        if (auto st = hackrftool::app::deserialize(
                read_text_file(exe_dir_path("settings.tsv")))) {
            restore_settings(app, *st);
            auto_start = true;   // 打开即回到上次工作状态（含自动开始接收）
        }
    }
    // APT 云图原生窗（云图页覆盖内容区；位于 WinFlux host 之上）
    hackrftool::ui::AptView::register_class(instance);
    (void)app.apt_view.create(app.main_wnd, instance);

    if (auto_start) {
        std::string err;
        if (!app.radio.open(&err)) {
            device_open_failed(app, err);
        } else {
            apply_radio(app);
            if (app.radio.start_rx(&rx_trampoline_ui, &app, &err)) {
                app.running = true;
                device_ok = true;
                hackrftool::log::log_telemetry(
                    hackrftool::log::Level::info, "LIFE", "rx.start",
                    {{"center_mhz", std::to_string(app.center_mhz)},
                     {"rate_msps", std::to_string(kRatesMsps[size_t(app.rate_index)])},
                     {"lna", std::to_string(app.lna)},
                     {"vga", std::to_string(app.vga)}});
                if (app.sweep_on == 1) set_sweep_live(app, true);
                if (app.page >= 3) ensure_fm(app, true);
                update_apt_on(app);
            } else {
                app.status = L"启动接收失败: " + widen(err);
                hackrftool::log::log_telemetry(hackrftool::log::Level::error,
                                               "LIFE", "rx.start.fail",
                                               {{"err", err}});
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
                // 遥测证据（#60）：事件总数 + 关键事件计数——报告自带日志摘要
                {
                    auto& lg = hackrftool::log::Logger::instance();
                    std::fprintf(rp, "遥测: events=%zu ui_cmd=%zu tune=%zu\n",
                                 lg.count(), lg.count_event("UI", "cmd"),
                                 lg.count_event("RADIO", "tune"));
                    for (const auto& e : lg.tail(5))
                        std::fprintf(rp, "  [%s/%s] %s\n", e.cat.c_str(),
                                     e.event.c_str(),
                                     e.kv.empty() ? "" : e.kv[0].first.c_str());
                }
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
