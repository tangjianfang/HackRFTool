# M2 信道监测（F2 稳定性分析）· 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** v0.2——在「信道监测」页对选定信道长期驻留监测：RSSI-时间曲线、均值/峰值/方差/占空比统计、CSV 导出；顺带清理 M1 三处瑕疵。

**Architecture:** 新增纯 C++ `ChannelMonitor`（环形样本缓冲 + 统计 + CSV，TDD）；UI 用 WinFlux `tabs` 分「频谱 / 信道监测」两页，监测页 RSSI 条带图走既有 `Props::paint` 模式；CSV 路径用 Win32 `GetSaveFileNameW`（WinFlux 无文件对话框组件）。监测数据源复用 M1 的 `SpectrumFrame`（UI 线程 build 时 push，无锁）。

**Tech Stack:** 同 M1（C++20/WinFlux/libhackrf 动态加载/CMake VS2022），新增链接 `Comdlg32`。

## Global Constraints

- 沿用 M1 全部约束（`/W4` 零告警、每任务一提交、中文文案、`/utf-8`）
- ChannelMonitor 仅 UI 线程调用（与 WaterfallModel 同契约，无锁）
- 样本容量 3600（约 5 分钟 @12fps）；时间戳用 `GetTickCount64()` 相对秒
- 手动锁定：bin = clamp(round((目标MHz − (中心−10)) / 20 × 256), 0, 255)
- M1 已知瑕疵清理：删瀑布左上角覆盖标签、删 `App::build_count`；分段间距过挤接受现状（记录）
- 验证识别次数预算：**1 次**（真机监测页截图）；DSP 全靠 ctest

---

### Task 1: ChannelMonitor（TDD）

**Files:**
- Create: `src/dsp/channel_monitor.hpp`
- Create: `src/dsp/channel_monitor.cpp`
- Modify: `tests/dsp_test.cpp`、`CMakeLists.txt`（测试目标加源文件）

**Interfaces:**
- Consumes: `SpectrumFrame`
- Produces:
  - `struct MonitorSample { double elapsed_s; float db; };`
  - `class ChannelMonitor`：`enum class Mode { auto_peak, fixed_bin };`
    构造 `explicit ChannelMonitor(std::size_t capacity = 3600);`
    `void set_mode(Mode)` / `Mode mode() const`
    `void set_fixed_bin(std::size_t)`（内部 clamp 到 255）
    `std::size_t tracked_bin() const`（上一帧实际 bin）
    `void push(const SpectrumFrame&)`（auto 模式取 argmax；记录 elapsed_s 与 db）
    `std::vector<MonitorSample> series() const`（旧→新拷贝）
    `struct Stats { float mean, peak, variance, duty; std::size_t count; };`
    `Stats stats(float threshold_db) const`
    `bool export_csv(const std::wstring& path) const`（表头 `elapsed_s,db`）

- [ ] **Step 1: 失败测试**（tests/dsp_test.cpp 加 `#include "dsp/channel_monitor.hpp"`）

```cpp
static hackrftool::dsp::SpectrumFrame frame_with(std::size_t n, std::size_t peak_bin,
                                                 float peak_db, float floor_db) {
    hackrftool::dsp::SpectrumFrame f;
    f.db.assign(n, floor_db);
    f.peak = f.db;
    if (peak_bin < n) f.db[peak_bin] = peak_db;
    f.seq = 1;
    return f;
}

static void test_monitor_auto_peak_tracks() {
    hackrftool::dsp::ChannelMonitor mon(8);
    mon.set_mode(hackrftool::dsp::ChannelMonitor::Mode::auto_peak);
    mon.push(frame_with(256, 10, -40.0f, -90.0f));
    check(mon.tracked_bin() == 10, "自动跟踪 argmax bin");
    mon.push(frame_with(256, 200, -35.0f, -90.0f));
    check(mon.tracked_bin() == 200, "峰移动后跟踪新 bin");
    const auto s = mon.series();
    check(s.size() == 2 && s[1].db == -35.0f, "序列记录 RSSI");
}

static void test_monitor_fixed_bin_and_clamp() {
    hackrftool::dsp::ChannelMonitor mon(8);
    mon.set_mode(hackrftool::dsp::ChannelMonitor::Mode::fixed_bin);
    mon.set_fixed_bin(999);   // 越界 → clamp 255
    mon.push(frame_with(256, 10, -40.0f, -90.0f));
    check(mon.tracked_bin() == 255, "fixed bin 钳制到 255");
    check(mon.series().back().db == -90.0f, "读取的是 bin255 的底噪");
}

static void test_monitor_stats() {
    hackrftool::dsp::ChannelMonitor mon(16);
    mon.set_mode(hackrftool::dsp::ChannelMonitor::Mode::fixed_bin);
    mon.set_fixed_bin(0);
    for (const float db : {-50.0f, -80.0f, -45.0f, -90.0f}) {
        auto f = frame_with(256, 0, db, db);   // bin0 即目标
        mon.push(f);
    }
    const auto st = mon.stats(-70.0f);
    check(st.count == 4, "统计样本数");
    check(std::abs(st.mean - (-66.25f)) < 0.01, "均值");
    check(std::abs(st.peak - (-45.0f)) < 0.01, "峰值");
    // 方差：样本方差（除以 count）
    check(std::abs(st.variance - 370.0f) < 0.5, "方差");
    check(std::abs(st.duty - 0.5f) < 0.001, "占空比 = 高于阈值比例");
}

static void test_monitor_ring_and_csv() {
    hackrftool::dsp::ChannelMonitor mon(4);
    mon.set_mode(hackrftool::dsp::ChannelMonitor::Mode::fixed_bin);
    mon.set_fixed_bin(0);
    for (int i = 0; i < 6; ++i) {
        auto f = frame_with(256, 0, float(-50 - i), float(-50 - i));
        mon.push(f);
    }
    const auto s = mon.series();
    check(s.size() == 4, "环形容量限制");
    check(std::abs(s.front().db - (-53.0f)) < 0.01, "最旧样本被滚动掉");
    const std::wstring path = L"test-monitor-export.csv";
    check(mon.export_csv(path), "CSV 导出成功");
    std::FILE* fp = _wfopen(path.c_str(), L"r");
    check(fp != nullptr, "CSV 可读回");
    if (fp != nullptr) {
        char line[128];
        unsigned lines = 0;
        bool header_ok = false, first_data_ok = false;
        while (std::fgets(line, sizeof line, fp) != nullptr) {
            if (lines == 0) header_ok = (std::strcmp(line, "elapsed_s,db\n") == 0);
            if (lines == 1) {
                double t; double db;
                first_data_ok = (std::sscanf(line, "%lf,%lf", &t, &db) == 2 && db < -52.9 && db > -53.1);
            }
            ++lines;
        }
        std::fclose(fp);
        check(header_ok, "CSV 表头");
        check(first_data_ok, "CSV 首行数据");
        check(lines == 5, "CSV 行数 = 表头 + 4 样本");
    }
    std::remove("test-monitor-export.csv");
}
```

`main()` 追加四个调用；需 `#include <cstdio>`（已有）与 `#include <cstring>`（strcmp）。

- [ ] **Step 2: 构建确认红灯**（`dsp/channel_monitor.hpp` 不存在）

- [ ] **Step 3: 实现**

`src/dsp/channel_monitor.hpp`：

```cpp
// 信道监测：对选定 bin（自动跟踪最强峰或手动锁定）记录 RSSI 时间序列，
// 提供统计与 CSV 导出。仅 UI 线程调用，无锁。
#pragma once

#include <cstddef>
#include <vector>

#include "dsp/analyzer.hpp"

namespace hackrftool::dsp {

struct MonitorSample {
    double elapsed_s = 0.0;
    float db = -130.0f;
};

class ChannelMonitor {
public:
    struct Stats {
        float mean = -130.0f;
        float peak = -130.0f;
        float variance = 0.0f;
        float duty = 0.0f;   // db >= threshold 的样本占比
        std::size_t count = 0;
    };

    enum class Mode { auto_peak, fixed_bin };

    explicit ChannelMonitor(std::size_t capacity = 3600);

    void set_mode(Mode m) noexcept { mode_ = m; }
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    void set_fixed_bin(std::size_t bin) noexcept;   // 内部 clamp
    [[nodiscard]] std::size_t tracked_bin() const noexcept { return last_bin_; }

    // 有新帧时调用（frame.db 非空才记录）
    void push(const SpectrumFrame& frame);

    [[nodiscard]] std::vector<MonitorSample> series() const;   // 旧→新
    [[nodiscard]] Stats stats(float threshold_db) const;
    [[nodiscard]] bool export_csv(const std::wstring& path) const;

private:
    const std::size_t capacity_;
    std::vector<MonitorSample> ring_;
    std::size_t head_ = 0;   // 下一写入位置
    std::size_t count_ = 0;
    Mode mode_ = Mode::auto_peak;
    std::size_t fixed_bin_ = 128;
    std::size_t last_bin_ = 128;
    unsigned long long start_ms_ = 0;
};

} // namespace hackrftool::dsp
```

`src/dsp/channel_monitor.cpp`：

```cpp
#include "dsp/channel_monitor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <windows.h>

namespace hackrftool::dsp {

ChannelMonitor::ChannelMonitor(std::size_t capacity) : capacity_(capacity) {
    ring_.reserve(capacity_);
}

void ChannelMonitor::set_fixed_bin(std::size_t bin) noexcept {
    fixed_bin_ = std::min(bin, std::size_t(255));
}

void ChannelMonitor::push(const SpectrumFrame& frame) {
    if (frame.db.empty()) return;
    const unsigned long long now_ms = GetTickCount64();
    if (start_ms_ == 0) start_ms_ = now_ms;

    if (mode_ == Mode::auto_peak) {
        std::size_t argmax = 0;
        for (std::size_t i = 1; i < frame.db.size(); ++i)
            if (frame.db[i] > frame.db[argmax]) argmax = i;
        last_bin_ = argmax;
    } else {
        last_bin_ = std::min(fixed_bin_, frame.db.size() - 1);
    }

    MonitorSample s;
    s.elapsed_s = double(now_ms - start_ms_) / 1000.0;
    s.db = frame.db[last_bin_];
    if (count_ < capacity_) {
        ring_.push_back(s);
        ++count_;
        head_ = count_ % capacity_;
    } else {
        ring_[head_] = s;
        head_ = (head_ + 1) % capacity_;
    }
}

std::vector<MonitorSample> ChannelMonitor::series() const {
    std::vector<MonitorSample> out;
    out.reserve(count_);
    const std::size_t start = (count_ == capacity_) ? head_ : 0;
    for (std::size_t i = 0; i < count_; ++i)
        out.push_back(ring_[(start + i) % capacity_]);
    return out;
}

ChannelMonitor::Stats ChannelMonitor::stats(float threshold_db) const {
    Stats st;
    if (count_ == 0) return st;
    const auto s = series();
    double sum = 0.0, above = 0.0;
    st.peak = s.front().db;
    for (const auto& sm : s) {
        sum += sm.db;
        st.peak = std::max(st.peak, sm.db);
        if (sm.db >= threshold_db) above += 1.0;
    }
    st.count = s.size();
    st.mean = float(sum / double(s.size()));
    double var = 0.0;
    for (const auto& sm : s) var += double(sm.db - st.mean) * double(sm.db - st.mean);
    st.variance = float(var / double(s.size()));
    st.duty = float(above / double(s.size()));
    return st;
}

bool ChannelMonitor::export_csv(const std::wstring& path) const {
    if (std::FILE* fp = _wfopen(path.c_str(), L"w")) {
        std::fprintf(fp, "elapsed_s,db\n");
        for (const auto& sm : series())
            std::fprintf(fp, "%.3f,%.1f\n", sm.elapsed_s, sm.db);
        std::fclose(fp);
        return true;
    }
    return false;
}

} // namespace hackrftool::dsp
```

- [ ] **Step 4: CMake 挂源、构建、ctest 全绿**

测试目标源文件追加 `src/dsp/channel_monitor.cpp`。

- [ ] **Step 5: Commit** `M2 DSP：信道监测（RSSI 序列/统计/CSV 导出）`

---

### Task 2: 监测页 UI 与接线

**Files:**
- Create: `src/ui/monitor_view.hpp`、`src/ui/monitor_view.cpp`
- Modify: `src/app/main.cpp`、`src/ui/views.cpp`（删瀑布覆盖标签）、`CMakeLists.txt`（主目标加源、链 `Comdlg32`）

**Interfaces:**
- Consumes: `ChannelMonitor`、M1 全部、`flux::ui::tabs/toggle_switch/field/slider`、`GetSaveFileNameW`
- Produces: `ElementPtr monitor_view(App 前置声明的引用…)` —— 实现为独立函数需 App 类型；简化：`monitor_page(...)` 放 main.cpp 内（与 M1 build 同文件，类型齐备）。`monitor_view.{hpp,cpp}` 只放**无状态**的绘制件：`rssi_strip(pal, const std::vector<MonitorSample>&, float threshold_db, unsigned seq)`

- [ ] **Step 1: rssi_strip 组件（monitor_view.cpp，paint 模式同频谱）**

```cpp
// RSSI 条带图：x=时间（旧→新），y=dBFS [-100,0]，红虚线=活动阈值
flux::ElementPtr rssi_strip(const flux::Palette& pal,
                            const std::vector<hackrftool::dsp::MonitorSample>& samples,
                            float threshold_db, unsigned seq);
```

绘制：20 dB 网格；阈值横线 `pal.danger`；样本折线 `draw_polyline`（`pal.accent`，2px）；空数据显示「等待数据…」；`paint_id = seq`（用 samples.size()+首尾 db 打包：`seq` 由调用方传 `unsigned(samples.size() ^ size_t(samples.back().db*10))`——直接传帧 seq 即可）。

- [ ] **Step 2: main.cpp 监测页**

App 新增：`dsp::ChannelMonitor monitor;`、`State<int>* page`、`State<bool>* auto_track`、`State<std::wstring>* mon_text`（目标 MHz 文本，默认 L"2450"）、`State<double>* threshold`（默认 -70）、`std::wstring mon_status;`（统计行文本，build 时拼）。

build() 中：
- 帧推进块同步 `app.monitor.push(f);`（`f.seq != app.frame.seq` 时）
- 统计行：`const auto st = app.monitor.stats(float(app.threshold.get()));` 拼中文标签：`均值 -62.3 dB / 峰值 -41.0 dB / 方差 88.2 / 占空比 34% / 样本 123 / 跟踪 bin 45`
- 页面结构：`flux::ui::tabs(pal, {L"频谱", L"信道监测"}, page.get(), on_change)` 顶部；page==0 渲染 M1 内容（header/controls/spectrum/waterfall），page==1 渲染监测页（column）：
  - row1：`toggle_switch 自动跟踪最强信号`、`field 目标频率 MHz text_input(mon_text)`、`button 锁定该频率`（fixed_bin = 映射公式；切 mode=fixed_bin）、`button 恢复自动`（mode=auto_peak）
  - `rssi_strip(pal, app.monitor.series(), threshold.get(), f.seq)`（flex_grow 1）
  - row2：`field 活动阈值 dB slider(-100..-40, threshold)`
  - row3：统计 caption（`mon_status`）
  - row4：`button 导出 CSV`（主按钮配色；`GetSaveFileNameW` → `monitor.export_csv` → status 反馈）
- M1 内容包进 page==0 分支；tabs 常驻顶部

**bin 映射函数**（main.cpp）：
```cpp
std::size_t mhz_to_bin(double mhz, double center_mhz) noexcept {
    const double t = (mhz - (center_mhz - 10.0)) / 20.0;   // 0..1
    return static_cast<std::size_t>(
        std::clamp(t, 0.0, 1.0) * 255.0 + 0.5);
}
```

**CSV 导出**（main.cpp，`#include <commdlg.h>`）：
```cpp
void export_csv_dialog(App& app) {
    wchar_t path[MAX_PATH] = L"hackrftool-monitor.csv";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = L"CSV 文件 (*.csv)\0*.csv\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;
    app.status.set(app.monitor.export_csv(path)
                       ? L"已导出: " + std::wstring(path)
                       : L"导出失败");
}
```

- [ ] **Step 3: M1 瑕疵清理**：`views.cpp` 删除瀑布 `draw_text(L"瀑布（最新在上）"…)` 行；`main.cpp` 删 `build_count`（声明与 `++app.build_count;`）
- [ ] **Step 4: CMake**：主目标源加 `src/dsp/channel_monitor.cpp src/ui/monitor_view.cpp`；`target_link_libraries(HackRFTool PRIVATE WinFlux::flux Comdlg32)`；测试目标加 channel_monitor.cpp（Task 1 已加）
- [ ] **Step 5: 构建零告警 + ctest 全绿 + 无硬件启动烟测**（exe 启动 2 秒进程存活、两个 tab 可切换——不截图，构建+进程存活即可）
- [ ] **Step 6: Commit** `M2 UI：信道监测页（RSSI 曲线/统计/CSV 导出/双页 tab）`

---

### Task 3: 真机验证与收尾

- [ ] **Step 1**: `hackrf_spiflash -R`；`HackRFTool.exe autom`（auto + 直接开监测页；cmd_line 解析：`auto`=收+频谱页，`autom`=收+监测页）；跑 6 秒后 PrintWindow 截图
- [ ] **Step 2**: haiku 识别一次（预算内唯一一次）：RSSI 曲线有起伏、统计行有真实数值、阈值线可见、导出按钮在
- [ ] **Step 3**: CSV 通道验证：不点 UI——加临时命令行 `export=<path>`？**不做**。CSV 落盘路径已由单测覆盖，UI 按钮与单测共用 `export_csv`，信任链成立；haiku 只确认按钮存在
- [ ] **Step 4**: README 状态更新（M2 完成）+ `docs/m2-交付记录.md`（实测统计数值、截图、已知问题：手动锁定 bin 不随中心频率重算、监测页无瀑布对照）
- [ ] **Step 5**: 全量 `ctest` + commit + push

---

## Self-Review 结论

- **Spec 覆盖**（README F2）：选定信道长期监测（auto/锁定）✓；RSSI-时间曲线（rssi_strip）✓；均值/峰值/方差/占空比 ✓；数据记录与 CSV 导出 ✓。M1 瑕疵清理 ✓。
- **占位符**：无；UI 关键代码块均给出。
- **类型一致性**：`MonitorSample/Stats/Mode`、`mhz_to_bin`、`export_csv_dialog` 命名一致；`series()` 旧→新契约与 rssi_strip、CSV 一致。
