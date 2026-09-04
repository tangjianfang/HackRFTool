# M1 频谱检测工具 · 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付可运行的 v0.1 —— WinFlux 主窗口实时显示 HackRF Pro 单窗 20 MHz 频谱图 + 瀑布图，带启停/频率/增益/采样率控制与状态栏。

**Architecture:** 三层：`dsp/`（纯 C++：FFT → 谱平均 → 瀑布历史，全部可单测）；`radio/`（libhackrf 动态加载封装，USB 回调线程直接喂 SpectrumAnalyzer）；`app/` + `ui/`（WinFlux 声明式 UI，10 fps 定时器拉快照渲染，UI 线程与采集线程经互斥锁隔离）。

**Tech Stack:** C++20 / Win32 / WinFlux（`WinFlux::flux` 静态库，add_subdirectory）/ libhackrf（LoadLibrary 动态加载，不链接导入库）/ CMake + VS2022 x64 / 测试沿用 WinFlux 的裸 `main()` + `check()` 模式。

## Global Constraints

- C++20、x64、`/W4` 零告警（复用 WinFlux 的 `WinFluxCompilerOptions.cmake`，源码含 `/utf-8`，中文字面量安全）
- 无新增第三方依赖；WinFlux 路径来自缓存变量 `WINFLUX_ROOT`（默认 `C:/tjf/github/WinFlux`），以 `EXCLUDE_FROM_ALL` 子目录引入
- libhackrf 仅 `LoadLibraryW(L"libhackrf.dll")` 动态加载；构建后从 `MSYS2_UCRT64_BIN`（默认 `C:/msys64/ucrt64/bin`）复制 `libhackrf.dll`、`libusb-1.0.dll`、`libwinpthread-1.dll`（已用 ldd 验证的完整依赖闭包）
- 头文件 `<libhackrf/hackrf.h>`，include 路径 `MSYS2_UCRT64_INCLUDE`（默认 `C:/msys64/ucrt64/include`）
- 真机测试**前**必须先运行 `hackrf_spiflash -R`（HackRF Pro 已知固件 bug，见 docs/m0-环境验证记录.md）
- 频率钳制 2400–2483.5 MHz；仅接收；UI 文案与注释用中文，标识符用英文
- 常量：频谱 dB 映射 `[-100, 0]` dBFS；瀑布空底 `-130`；FFT 512 点，边缘裁剪 1/16，输出 256 bin；帧平均 64 块；刷新 100 ms
- 每个任务完成即 `git commit`（消息中文，附 Co-Authored-By 尾注）
- 构建命令：`cmake --preset x64-debug`、`cmake --build --preset x64-debug`、`ctest --preset x64-debug`；产物在 `out/build/x64-debug/Debug/`

---

### Task 1: 工程骨架

**Files:**
- Create: `CMakeLists.txt`
- Create: `CMakePresets.json`
- Create: `src/app/main.cpp`（stub 窗口）
- Create: `tests/dsp_test.cpp`（测试骨架）
- Create: `tools/screenshot.ps1`（窗口截图脚本，Task 7/8 验证用）

**Interfaces:**
- Consumes: WinFlux `flux::Host`（`make_state`/`set_root_builder`/`Config{title,width,height}`/`create`/`run`）、`flux::enable_per_monitor_dpi_v2()`、`flux::label()`
- Produces: 目标 `HackRFTool`（WIN32 GUI）、`HackRFToolTest`（控制台测试）、构建后 DLL 复制规则、截图命令 `powershell -NoProfile -File tools/screenshot.ps1`

- [ ] **Step 1: 写 CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.25)
project(HackRFTool VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "HackRFTool targets x64 only.")
endif()

set(WINFLUX_ROOT "C:/tjf/github/WinFlux" CACHE PATH "WinFlux 仓库路径")
set(MSYS2_UCRT64_BIN "C:/msys64/ucrt64/bin" CACHE PATH "libhackrf 运行时 DLL 目录")
set(MSYS2_UCRT64_INCLUDE "C:/msys64/ucrt64/include" CACHE PATH "libhackrf 头文件目录")

add_subdirectory(${WINFLUX_ROOT} winflux EXCLUDE_FROM_ALL)
# add_subdirectory 之后 WinFlux 的 cmake/ 已挂入 CMAKE_MODULE_PATH
include(WinFluxCompilerOptions)

add_executable(HackRFTool WIN32 src/app/main.cpp)
target_include_directories(HackRFTool PRIVATE src ${MSYS2_UCRT64_INCLUDE})
target_link_libraries(HackRFTool PRIVATE WinFlux::flux)
flux_apply_compiler_options(HackRFTool)

foreach(dll libhackrf.dll libusb-1.0.dll libwinpthread-1.dll)
    add_custom_command(TARGET HackRFTool POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MSYS2_UCRT64_BIN}/${dll}" $<TARGET_FILE_DIR:HackRFTool>
        COMMENT "复制 ${dll}")
endforeach()

enable_testing()
add_executable(HackRFToolTest tests/dsp_test.cpp)
target_include_directories(HackRFToolTest PRIVATE src)
flux_apply_compiler_options(HackRFToolTest)
add_test(NAME HackRFToolTest COMMAND HackRFToolTest)
```

- [ ] **Step 2: 写 CMakePresets.json**

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "x64-debug", "generator": "Visual Studio 17 2022", "architecture": "x64", "binaryDir": "out/build/x64-debug" },
    { "name": "x64-release", "generator": "Visual Studio 17 2022", "architecture": "x64", "binaryDir": "out/build/x64-release" }
  ],
  "buildPresets": [
    { "name": "x64-debug", "configurePreset": "x64-debug", "configuration": "Debug" },
    { "name": "x64-release", "configurePreset": "x64-release", "configuration": "Release" }
  ],
  "testPresets": [
    { "name": "x64-debug", "configurePreset": "x64-debug", "configuration": "Debug" }
  ]
}
```

- [ ] **Step 3: 写 stub 窗口 `src/app/main.cpp`**

```cpp
// HackRFTool M1 —— 频谱检测工具（骨架）
#include <windows.h>

#include <flux/flux.hpp>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    flux::enable_per_monitor_dpi_v2();
    flux::Host host;
    host.set_root_builder([] { return flux::label(L"HackRFTool M1"); });
    flux::Host::Config cfg;
    cfg.title = L"HackRFTool";
    cfg.width = 1200;
    cfg.height = 800;
    if (!host.create(cfg, instance)) return 1;
    return host.run();
}
```

- [ ] **Step 4: 写测试骨架 `tests/dsp_test.cpp`**

```cpp
// 纯 C++ DSP 单元测试 —— 沿用 WinFlux 的裸 main + check 模式，无测试框架
#include <cstdio>

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

int main() {
    if (failures == 0) std::printf("HackRFToolTest: 全部通过\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 5: 写截图脚本 `tools/screenshot.ps1`**

```powershell
param([string]$Title = "HackRFTool", [string]$Out = "shot.png")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Shot {
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$h = [Win32Shot]::FindWindow($null, $Title)
if ($h -eq [IntPtr]::Zero) { Write-Error "未找到窗口: $Title"; exit 1 }
[Win32Shot]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500
$r = New-Object Win32Shot+RECT
[Win32Shot]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "已保存 $Out (${w}x${ht})"
```

- [ ] **Step 6: 构建验证**

Run: `cd C:/tjf/github/HackRFTool && cmake --preset x64-debug && cmake --build --preset x64-debug && ctest --preset x64-debug`
Expected: 构建零告警零错误；ctest `HackRFToolTest` PASS，打印 `全部通过`；`out/build/x64-debug/Debug/` 下出现 `HackRFTool.exe` 与三个 DLL。

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt CMakePresets.json src/app/main.cpp tests/dsp_test.cpp tools/screenshot.ps1
git commit -m "M1 骨架：CMake 工程 + WinFlux 窗口 stub + 测试脚手架

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 2: FFT 模块（TDD）

**Files:**
- Create: `src/dsp/fft.hpp`
- Create: `src/dsp/fft.cpp`
- Modify: `tests/dsp_test.cpp`（追加用例）
- Modify: `CMakeLists.txt`（测试目标加源文件）

**Interfaces:**
- Produces: `namespace hackrftool::dsp`；`constexpr double kPi`；`bool is_power_of_two(std::size_t n)`；`void fft(std::vector<std::complex<double>>& a)`（就地、n 为 2 的幂、前向变换，未做归一化——幅度约定：bin k 单频幅度 A ⇒ |X[k]| = A·N）

- [ ] **Step 1: 在 tests/dsp_test.cpp 追加失败测试（含头文件引用）**

文件顶部追加：

```cpp
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "dsp/fft.hpp"
```

`main()` 前追加：

```cpp
static void test_fft_dc() {
    std::vector<std::complex<double>> x(4, {1.0, 0.0});
    hackrftool::dsp::fft(x);
    check(std::abs(x[0] - std::complex<double>(4.0, 0.0)) < 1e-9, "fft 直流分量 X[0]=4");
    for (std::size_t k = 1; k < 4; ++k)
        check(std::abs(x[k]) < 1e-9, "fft 直流输入其余频点为 0");
}

static void test_fft_tone_bin1() {
    const std::size_t N = 8;
    std::vector<std::complex<double>> x(N);
    for (std::size_t n = 0; n < N; ++n) {
        const double ph = 2.0 * hackrftool::dsp::kPi * double(n) / double(N);
        x[n] = {std::cos(ph), std::sin(ph)};   // bin 1 复单频，幅度 1
    }
    hackrftool::dsp::fft(x);
    check(std::abs(std::abs(x[1]) - 8.0) < 1e-9, "fft bin1 幅度 = N");
    for (std::size_t k = 0; k < N; ++k)
        if (k != 1) check(std::abs(x[k]) < 1e-9, "fft 单频无泄漏");
}

static void test_fft_parseval() {
    const std::size_t N = 64;
    std::vector<std::complex<double>> x(N);
    unsigned lcg = 12345u;   // 确定性伪随机
    double time_energy = 0.0, freq_energy = 0.0;
    for (std::size_t n = 0; n < N; ++n) {
        lcg = lcg * 1664525u + 1013904223u;
        const double re = double(int(lcg >> 24) - 128) / 128.0;
        lcg = lcg * 1664525u + 1013904223u;
        const double im = double(int(lcg >> 24) - 128) / 128.0;
        x[n] = {re, im};
        time_energy += re * re + im * im;
    }
    hackrftool::dsp::fft(x);
    for (std::size_t k = 0; k < N; ++k) freq_energy += std::norm(x[k]);
    check(std::abs(freq_energy - double(N) * time_energy) < 1e-6 * freq_energy,
          "fft Parseval 定理");
}
```

`main()` 内追加调用：

```cpp
    test_fft_dc();
    test_fft_tone_bin1();
    test_fft_parseval();
```

- [ ] **Step 2: 运行确认编译失败**

Run: `cmake --build --preset x64-debug 2>&1 | head -5`
Expected: 编译错误——`dsp/fft.hpp: No such file or directory`（TDD 红灯）。

- [ ] **Step 3: 写 `src/dsp/fft.hpp`**

```cpp
// 迭代式 radix-2 复数 FFT（无第三方依赖，M1 处理量 64×512 点/帧，性能足够）
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace hackrftool::dsp {

inline constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] inline bool is_power_of_two(std::size_t n) noexcept {
    return n != 0 && (n & (n - 1)) == 0;
}

// 就地前向 FFT。前置条件：a.size() 为 2 的幂。
// 幅度约定：bin k 单频（复幅度 A）⇒ |X[k]| = A·N，不做 1/N 归一化。
void fft(std::vector<std::complex<double>>& a);

} // namespace hackrftool::dsp
```

- [ ] **Step 4: 写 `src/dsp/fft.cpp`**

```cpp
#include "dsp/fft.hpp"

#include <utility>

namespace hackrftool::dsp {

void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    if (n < 2 || !is_power_of_two(n)) return;

    // 位反转置换
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }

    // 蝶形：len 2, 4, ..., n
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / double(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

} // namespace hackrftool::dsp
```

- [ ] **Step 5: CMake 挂源文件并跑测试**

`CMakeLists.txt` 测试目标改为：

```cmake
add_executable(HackRFToolTest tests/dsp_test.cpp src/dsp/fft.cpp)
```

Run: `cmake --build --preset x64-debug && ctest --preset x64-debug`
Expected: PASS，`全部通过`。

- [ ] **Step 6: Commit**

```bash
git add src/dsp/fft.hpp src/dsp/fft.cpp tests/dsp_test.cpp CMakeLists.txt
git commit -m "M1 DSP：radix-2 FFT（直流/单频/Parseval 单测）

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 3: SpectrumAnalyzer 频谱分析器（TDD）

**Files:**
- Create: `src/dsp/analyzer.hpp`
- Create: `src/dsp/analyzer.cpp`
- Modify: `tests/dsp_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `hackrftool::dsp::fft`
- Produces:
  - `struct SpectrumFrame { std::vector<float> db; std::vector<float> peak; unsigned seq = 0; }`——`db/peak` 长度 `bins()`，dBFS，钳制 `[-100, 0]`；`seq` 每完成一帧 +1（UI 用作 paint_id）
  - `class SpectrumAnalyzer`：构造 `(fft_size=512, average_blocks=64, bins_out=256)`；`void feed(const std::int8_t* iq, std::size_t byte_count)`（线程安全，USB 回调线程调用，interleaved int8 I/Q）；`SpectrumFrame snapshot() const`；`void reset_peaks()`；`std::size_t bins() const`

- [ ] **Step 1: 追加失败测试**

`tests/dsp_test.cpp` 头部追加 `#include "dsp/analyzer.hpp"`，用例：

```cpp
static void test_analyzer_tone_location_and_level() {
    hackrftool::dsp::SpectrumAnalyzer an(512, 4, 256);
    check(an.bins() == 256, "分析器输出 256 bin");
    check(an.snapshot().db.empty(), "无数据时快照为空");

    // 输入 bin 64 复单频，幅度 100（满幅 127）
    const std::size_t N = 512, k = 64;
    std::vector<std::int8_t> iq(N * 2);
    for (std::size_t n = 0; n < N; ++n) {
        const double ph = 2.0 * hackrftool::dsp::kPi * double(k) * double(n) / double(N);
        iq[n * 2]     = static_cast<std::int8_t>(100 * std::cos(ph));
        iq[n * 2 + 1] = static_cast<std::int8_t>(100 * std::sin(ph));
    }
    for (int i = 0; i < 4; ++i) an.feed(iq.data(), iq.size());
    const auto f = an.snapshot();
    check(!f.db.empty(), "4 块后出帧");
    std::size_t argmax = 0;
    for (std::size_t i = 1; i < f.db.size(); ++i)
        if (f.db[i] > f.db[argmax]) argmax = i;
    // 期望位置：skip=32，usable=448 → out ≈ (64-32)*256/448 ≈ 18
    check(argmax > 15 && argmax < 22, "分析器峰位 ≈ 18");
    // 期望电平：20log10(100/127) ≈ -2.1 dBFS（int8 量化略降）
    check(f.db[argmax] > -4.0 && f.db[argmax] < -1.0, "分析器峰电平 ≈ -2 dBFS");
}

static void test_analyzer_peak_hold_and_reset() {
    hackrftool::dsp::SpectrumAnalyzer an(512, 2, 256);
    std::vector<std::int8_t> quiet(512 * 2, 0);   // 直流 0：全频段底噪
    an.feed(quiet.data(), quiet.size());
    an.feed(quiet.data(), quiet.size());
    const auto f1 = an.snapshot();
    an.reset_peaks();
    // 再喂一次同样数据，峰值应不低于当前值
    an.feed(quiet.data(), quiet.size());
    an.feed(quiet.data(), quiet.size());
    const auto f2 = an.snapshot();
    for (std::size_t i = 0; i < f2.db.size(); ++i)
        check(f2.peak[i] >= f2.db[i] - 0.001, "峰值保持 ≥ 当前值");
}
```

`main()` 追加两个调用。

- [ ] **Step 2: 构建确认失败（红灯）**

Run: `cmake --build --preset x64-debug 2>&1 | head -3`
Expected: `dsp/analyzer.hpp: No such file or directory`。

- [ ] **Step 3: 写 `src/dsp/analyzer.hpp`**

```cpp
// 频谱分析器：int8 IQ → 块平均功率谱（dBFS）→ 峰值保持。线程安全。
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace hackrftool::dsp {

struct SpectrumFrame {
    std::vector<float> db;     // 当前帧 dBFS，[-100, 0]
    std::vector<float> peak;   // 峰值保持（每帧衰减 0.2 dB）
    unsigned seq = 0;          // 帧序号，完成一帧 +1
};

class SpectrumAnalyzer {
public:
    SpectrumAnalyzer(std::size_t fft_size = 512, std::size_t average_blocks = 64,
                     std::size_t bins_out = 256);

    // USB 回调线程调用；byte_count 为字节数（I/Q 交错，每样本 2 字节）。
    // 不足一个 FFT 块的尾巴丢弃（功率平均对块边界不敏感）。
    void feed(const std::int8_t* iq, std::size_t byte_count);

    [[nodiscard]] SpectrumFrame snapshot() const;
    void reset_peaks();
    [[nodiscard]] std::size_t bins() const noexcept { return bins_out_; }

private:
    void finish_frame_locked();

    const std::size_t fft_size_;
    const std::size_t average_blocks_;
    const std::size_t bins_out_;
    const std::size_t skip_;   // 边缘裁剪（基带滤波器过渡带），每侧 fft_size_/16

    mutable std::mutex mutex_;
    std::vector<double> accum_;   // 每 bin 功率累计
    std::size_t accum_count_ = 0;
    SpectrumFrame frame_;         // 最近完成的帧
};

} // namespace hackrftool::dsp
```

- [ ] **Step 4: 写 `src/dsp/analyzer.cpp`**

```cpp
#include "dsp/analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

#include "dsp/fft.hpp"

namespace hackrftool::dsp {

namespace {
// 满幅参考：int8 单频幅度 127 定义为 0 dBFS
constexpr double kFullScaleAmp = 127.0;
constexpr double kDbFloor = -100.0;
constexpr double kPeakDecayDb = 0.2;   // 每帧峰值衰减
} // namespace

SpectrumAnalyzer::SpectrumAnalyzer(std::size_t fft_size, std::size_t average_blocks,
                                   std::size_t bins_out)
    : fft_size_(fft_size),
      average_blocks_(average_blocks),
      bins_out_(bins_out),
      skip_(fft_size / 16),
      accum_(fft_size, 0.0) {}

void SpectrumAnalyzer::feed(const std::int8_t* iq, std::size_t byte_count) {
    const std::size_t complex_samples = byte_count / 2;
    std::vector<std::complex<double>> buf(fft_size_);

    std::size_t used = 0;
    while (used + fft_size_ <= complex_samples) {
        for (std::size_t i = 0; i < fft_size_; ++i)
            buf[i] = {double(iq[(used + i) * 2]), double(iq[(used + i) * 2 + 1])};
        fft(buf);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (std::size_t k = 0; k < fft_size_; ++k) accum_[k] += std::norm(buf[k]);
            if (++accum_count_ >= average_blocks_) finish_frame_locked();
        }
        used += fft_size_;
    }
}

void SpectrumAnalyzer::finish_frame_locked() {
    const std::size_t usable = fft_size_ - 2 * skip_;
    const double ref = kFullScaleAmp * kFullScaleAmp *
                       double(fft_size_) * double(fft_size_) *
                       double(accum_count_);   // 与累计口径一致的满幅功率

    SpectrumFrame next;
    next.db.resize(bins_out_);
    next.peak.resize(bins_out_);
    next.seq = frame_.seq + 1;
    const double floor_db = kDbFloor - 30.0;   // 底以下钳到 -130，瀑布空底一致

    for (std::size_t i = 0; i < bins_out_; ++i) {
        const std::size_t in = skip_ + (i * usable + bins_out_ / 2) / bins_out_;
        const double db = std::max(floor_db,
                                   10.0 * std::log10(accum_[in] / ref + 1e-30));
        next.db[i] = float(db);
        const float prev_peak =
            (frame_.peak.size() == bins_out_) ? frame_.peak[i] : float(floor_db);
        next.peak[i] = std::max(prev_peak - float(kPeakDecayDb), float(db));
    }

    frame_ = std::move(next);
    std::fill(accum_.begin(), accum_.end(), 0.0);
    accum_count_ = 0;
}

SpectrumFrame SpectrumAnalyzer::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frame_;
}

void SpectrumAnalyzer::reset_peaks() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fill(frame_.peak.begin(), frame_.peak.end(),
              float(kDbFloor - 30.0));
}

} // namespace hackrftool::dsp
```

- [ ] **Step 5: CMake 挂源文件、构建、测试（绿灯）**

`CMakeLists.txt` 测试目标源文件追加 `src/dsp/analyzer.cpp`。

Run: `cmake --build --preset x64-debug && ctest --preset x64-debug`
Expected: PASS（含 Task 2 用例）。

- [ ] **Step 6: Commit**

```bash
git add src/dsp/analyzer.hpp src/dsp/analyzer.cpp tests/dsp_test.cpp CMakeLists.txt
git commit -m "M1 DSP：频谱分析器（块平均 dBFS + 峰值保持，线程安全）

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 4: WaterfallModel 瀑布历史（TDD）

**Files:**
- Create: `src/dsp/waterfall.hpp`
- Create: `src/dsp/waterfall.cpp`
- Modify: `tests/dsp_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SpectrumFrame.db`（长度 256）
- Produces: `class WaterfallModel`——构造 `(cols=256, rows=64)`；`bool push(const std::vector<float>& db)`（长度不符返回 false；仅 UI 线程调用，无锁）；`std::vector<float> snapshot() const`（返回 rows×cols，行 0 = 最新，未填满的行 = -130）；`unsigned seq() const`（push 成功 +1，paint_id 用）；`std::size_t rows()/cols()`

- [ ] **Step 1: 追加失败测试**

`#include "dsp/waterfall.hpp"`，用例：

```cpp
static void test_waterfall_ring() {
    hackrftool::dsp::WaterfallModel wf(4, 2);
    const std::vector<float> a = {-10, -20, -30, -40};
    const std::vector<float> b = {-50, -60, -70, -80};
    const std::vector<float> c = {-90, -100, -110, -120};
    wf.push(a);
    wf.push(b);
    wf.push(c);   // 环回，a 被挤出
    const auto snap = wf.snapshot();
    check(snap.size() == 8, "瀑布快照 rows*cols");
    check(snap[0] == -90 && snap[1] == -100, "瀑布行 0 = 最新帧");
    check(snap[4] == -50 && snap[5] == -60, "瀑布行 1 = 上一帧");
    check(!wf.push({-1.0f, -2.0f}), "长度不符 push 返回 false");
}

static void test_waterfall_prefill() {
    hackrftool::dsp::WaterfallModel wf(2, 3);
    wf.push({-5, -6});
    const auto snap = wf.snapshot();
    check(snap[0] == -5 && snap[1] == -6, "瀑布首帧在行 0");
    check(snap[2] == -130 && snap[4] == -130, "未填充行 = -130");
}
```

`main()` 追加调用。

- [ ] **Step 2: 构建确认红灯**

Run: `cmake --build --preset x64-debug 2>&1 | head -3`
Expected: `dsp/waterfall.hpp: No such file or directory`。

- [ ] **Step 3: 写 `src/dsp/waterfall.hpp`**

```cpp
// 瀑布历史：定长环形行缓冲。仅 UI 线程调用，无锁。
#pragma once

#include <cstddef>
#include <vector>

namespace hackrftool::dsp {

class WaterfallModel {
public:
    WaterfallModel(std::size_t cols = 256, std::size_t rows = 64);

    // db 长度必须 == cols；成功返回 true 并使 seq +1
    bool push(const std::vector<float>& db);

    // rows*cols，行 0 最新；未填满的行 = -130
    [[nodiscard]] std::vector<float> snapshot() const noexcept;

    [[nodiscard]] unsigned seq() const noexcept { return seq_; }
    [[nodiscard]] std::size_t rows() const noexcept { return rows_; }
    [[nodiscard]] std::size_t cols() const noexcept { return cols_; }

private:
    const std::size_t cols_;
    const std::size_t rows_;
    std::vector<float> buf_;   // rows*cols 环形
    std::size_t next_ = 0;     // 下一写入行
    std::size_t filled_ = 0;
    unsigned seq_ = 0;
};

} // namespace hackrftool::dsp
```

- [ ] **Step 4: 写 `src/dsp/waterfall.cpp`**

```cpp
#include "dsp/waterfall.hpp"

#include <algorithm>

namespace hackrftool::dsp {

namespace {
constexpr float kEmptyDb = -130.0f;
}

WaterfallModel::WaterfallModel(std::size_t cols, std::size_t rows)
    : cols_(cols), rows_(rows), buf_(rows * cols, kEmptyDb) {}

bool WaterfallModel::push(const std::vector<float>& db) {
    if (db.size() != cols_) return false;
    std::copy(db.begin(), db.end(), buf_.begin() + static_cast<std::ptrdiff_t>(next_ * cols_));
    next_ = (next_ + 1) % rows_;
    filled_ = std::min(filled_ + 1, rows_);
    ++seq_;
    return true;
}

std::vector<float> WaterfallModel::snapshot() const noexcept {
    std::vector<float> out;
    out.reserve(rows_ * cols_);
    for (std::size_t r = 0; r < rows_; ++r) {
        const std::size_t src = (next_ + rows_ - 1 - r) % rows_;
        out.insert(out.end(), buf_.begin() + static_cast<std::ptrdiff_t>(src * cols_),
                   buf_.begin() + static_cast<std::ptrdiff_t>((src + 1) * cols_));
    }
    return out;
}

} // namespace hackrftool::dsp
```

- [ ] **Step 5: CMake 挂源、构建、测试（绿灯）**

测试目标源文件追加 `src/dsp/waterfall.cpp`。

Run: `cmake --build --preset x64-debug && ctest --preset x64-debug`
Expected: PASS。

- [ ] **Step 6: Commit**

```bash
git add src/dsp/waterfall.hpp src/dsp/waterfall.cpp tests/dsp_test.cpp CMakeLists.txt
git commit -m "M1 DSP：瀑布历史环形缓冲

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 5: HackRadio 动态加载封装 + 真机冒烟

**Files:**
- Create: `src/radio/hackrf.hpp`
- Create: `src/radio/hackrf.cpp`
- Create: `tools/hackrf_smoke.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `<libhackrf/hackrf.h>`（函数类型 `decltype(&hackrf_init)` 等；`hackrf_transfer{buffer, valid_length, rx_ctx}`）
- Produces:
  - `struct RadioConfig { double center_hz = 2.45e9; double sample_rate_hz = 20e6; unsigned lna_gain_db = 32; unsigned vga_gain_db = 30; bool amp = false; }`
  - `class HackRadio`：`using Callback = void(*)(const std::int8_t* iq, std::size_t bytes, void* ctx);`
    `bool open(std::string* error)`（LoadLibrary + hackrf_init + hackrf_open）；`void close()`；`bool apply(const RadioConfig&, std::string*)`（freq/rate/gains/amp）；`bool start_rx(Callback, void* ctx, std::string*)`；`void stop_rx()`；`bool is_open() const; bool is_running() const; std::string library_version() const;`（析构自动 stop_rx + close）
  - 控制台目标 `HackRFSmoke`

- [ ] **Step 1: 写 `src/radio/hackrf.hpp`**

```cpp
// libhackrf 动态加载封装：MSVC 不链接 MSYS2 的 MinGW 导入库，
// 全部函数经 LoadLibrary/GetProcAddress 取得；DLL 由构建后步骤复制到 exe 旁。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <hackrf/hackrf.h>

namespace hackrftool::radio {

struct RadioConfig {
    double center_hz = 2.45e9;
    double sample_rate_hz = 20e6;
    unsigned lna_gain_db = 32;   // 8..40，步进 8
    unsigned vga_gain_db = 30;   // 2..62，步进 2
    bool amp = false;
};

class HackRadio {
public:
    // 回调在 libusb 线程执行：不得调用本类方法，只应拷贝/处理数据
    using Callback = void (*)(const std::int8_t* iq, std::size_t bytes, void* ctx);

    HackRadio() = default;
    ~HackRadio();
    HackRadio(const HackRadio&) = delete;
    HackRadio& operator=(const HackRadio&) = delete;

    [[nodiscard]] bool open(std::string* error = nullptr);
    void close();
    [[nodiscard]] bool apply(const RadioConfig& cfg, std::string* error = nullptr);
    [[nodiscard]] bool start_rx(Callback cb, void* ctx, std::string* error = nullptr);
    void stop_rx();

    [[nodiscard]] bool is_open() const noexcept { return dev_ != nullptr; }
    [[nodiscard]] bool is_running() const noexcept { return running_; }
    [[nodiscard]] std::string library_version() const;

private:
    struct RxTrampoline {
        Callback cb;
        void* ctx;
    };

    [[nodiscard]] bool load_api(std::string* error);
    [[nodiscard]] static std::string hackrf_error_text(int code);

    HMODULE module_ = nullptr;
    hackrf_device* dev_ = nullptr;
    bool inited_ = false;
    bool running_ = false;
    RxTrampoline trampoline_{};

    // 动态加载的 API（open 成功后有效）
    int (*fn_init)() = nullptr;
    int (*fn_exit)() = nullptr;
    int (*fn_open)(hackrf_device**) = nullptr;
    int (*fn_close)(hackrf_device*) = nullptr;
    int (*fn_start_rx)(hackrf_device*, hackrf_sample_block_cb_fn, void*) = nullptr;
    int (*fn_stop_rx)(hackrf_device*) = nullptr;
    int (*fn_set_freq)(hackrf_device*, std::uint64_t) = nullptr;
    int (*fn_set_sample_rate)(hackrf_device*, double) = nullptr;
    int (*fn_set_lna_gain)(hackrf_device*, std::uint32_t) = nullptr;
    int (*fn_set_vga_gain)(hackrf_device*, std::uint32_t) = nullptr;
    int (*fn_set_amp_enable)(hackrf_device*, std::uint8_t) = nullptr;
    const char* (*fn_library_version)() = nullptr;
};

} // namespace hackrftool::radio
```

- [ ] **Step 2: 写 `src/radio/hackrf.cpp`**

```cpp
#include "radio/hackrf.hpp"

#include <windows.h>

namespace hackrftool::radio {

namespace {
int rx_trampoline(hackrf_transfer* t) {
    // 回调契约：只读 transfer，不调 libhackrf
    const auto* tramp = static_cast<HackRadio::RxTrampoline*>(t->rx_ctx);
    if (tramp != nullptr && tramp->cb != nullptr)
        tramp->cb(reinterpret_cast<const std::int8_t*>(t->buffer),
                  static_cast<std::size_t>(t->valid_length), tramp->ctx);
    return 0;   // 0 = 继续流
}
} // namespace

HackRadio::~HackRadio() {
    stop_rx();
    close();
}

bool HackRadio::load_api(std::string* error) {
    const auto fail = [this, error](const char* what) {
        if (error != nullptr) *error = std::string("libhackrf 加载失败：") + what;
        close();
        return false;
    };
    module_ = LoadLibraryW(L"libhackrf.dll");
    if (module_ == nullptr) return fail("LoadLibraryW(libhackrf.dll)");

#define LOAD_FN(field, name)                                                   \
    field = reinterpret_cast<decltype(field)>(                                 \
        reinterpret_cast<void*>(GetProcAddress(module_, name)));               \
    if (field == nullptr) return fail("缺函数 " name);
    LOAD_FN(fn_init, "hackrf_init")
    LOAD_FN(fn_exit, "hackrf_exit")
    LOAD_FN(fn_open, "hackrf_open")
    LOAD_FN(fn_close, "hackrf_close")
    LOAD_FN(fn_start_rx, "hackrf_start_rx")
    LOAD_FN(fn_stop_rx, "hackrf_stop_rx")
    LOAD_FN(fn_set_freq, "hackrf_set_freq")
    LOAD_FN(fn_set_sample_rate, "hackrf_set_sample_rate")
    LOAD_FN(fn_set_lna_gain, "hackrf_set_lna_gain")
    LOAD_FN(fn_set_vga_gain, "hackrf_set_vga_gain")
    LOAD_FN(fn_set_amp_enable, "hackrf_set_amp_enable")
    LOAD_FN(fn_library_version, "hackrf_library_version")
#undef LOAD_FN
    return true;
}

bool HackRadio::open(std::string* error) {
    if (is_open()) return true;
    if (!load_api(error)) return false;
    int rc = fn_init();
    if (rc != HACKRF_SUCCESS) return (error != nullptr)
        ? (*error = "hackrf_init: " + hackrf_error_text(rc), false) : false;
    inited_ = true;
    rc = fn_open(&dev_);
    if (rc != HACKRF_SUCCESS) {
        if (error != nullptr) *error = "hackrf_open: " + hackrf_error_text(rc);
        close();
        return false;
    }
    return true;
}

void HackRadio::close() {
    if (dev_ != nullptr) {
        fn_close(dev_);
        dev_ = nullptr;
    }
    if (inited_) {
        fn_exit();
        inited_ = false;
    }
    if (module_ != nullptr) {
        FreeLibrary(module_);
        module_ = nullptr;
    }
}

bool HackRadio::apply(const RadioConfig& cfg, std::string* error) {
    if (!is_open()) {
        if (error != nullptr) *error = "设备未打开";
        return false;
    }
    const struct Step {
        const char* name;
        int rc;
    } steps[] = {
        {"set_freq", fn_set_freq(dev_, static_cast<std::uint64_t>(cfg.center_hz))},
        {"set_sample_rate", fn_set_sample_rate(dev_, cfg.sample_rate_hz)},
        {"set_lna_gain", fn_set_lna_gain(dev_, cfg.lna_gain_db)},
        {"set_vga_gain", fn_set_vga_gain(dev_, cfg.vga_gain_db)},
        {"set_amp_enable", fn_set_amp_enable(dev_, cfg.amp ? 1 : 0)},
    };
    for (const auto& s : steps) {
        if (s.rc != HACKRF_SUCCESS) {
            if (error != nullptr)
                *error = std::string(s.name) + ": " + hackrf_error_text(s.rc);
            return false;
        }
    }
    return true;
}

bool HackRadio::start_rx(Callback cb, void* ctx, std::string* error) {
    if (!is_open()) {
        if (error != nullptr) *error = "设备未打开";
        return false;
    }
    trampoline_ = {cb, ctx};
    const int rc = fn_start_rx(dev_, &rx_trampoline, &trampoline_);
    if (rc != HACKRF_SUCCESS) {
        if (error != nullptr) *error = "hackrf_start_rx: " + hackrf_error_text(rc);
        return false;
    }
    running_ = true;
    return true;
}

void HackRadio::stop_rx() {
    if (running_ && dev_ != nullptr) fn_stop_rx(dev_);
    running_ = false;
}

std::string HackRadio::library_version() const {
    return (fn_library_version != nullptr) ? fn_library_version() : "(未加载)";
}

std::string HackRadio::hackrf_error_text(int code) {
    const char* s = hackrf_error_name(static_cast<hackrf_error>(code));
    return (s != nullptr) ? s : ("未知错误 " + std::to_string(code));
}

} // namespace hackrftool::radio
```

- [ ] **Step 3: 写 `tools/hackrf_smoke.cpp`（真机冒烟）**

```cpp
// 真机冒烟：打开设备 → 配置 2450 MHz/20 Msps → 收 0.5 秒 → 报告字节数
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "radio/hackrf.hpp"

namespace {
std::atomic<std::size_t> g_bytes{0};
std::atomic<std::size_t> g_blocks{0};

void rx_cb(const std::int8_t* /*iq*/, std::size_t bytes, void* /*ctx*/) {
    g_bytes += bytes;
    g_blocks += 1;
}
} // namespace

int main() {
    using namespace hackrftool::radio;
    HackRadio radio;
    std::string err;
    if (!radio.open(&err)) {
        std::printf("打开失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("libhackrf %s\n", radio.library_version().c_str());

    RadioConfig cfg;   // 默认 2450 MHz / 20 Msps / LNA32 / VGA30
    if (!radio.apply(cfg, &err)) {
        std::printf("配置失败: %s\n", err.c_str());
        return 1;
    }
    if (!radio.start_rx(&rx_cb, nullptr, &err)) {
        std::printf("启动接收失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("接收中 0.5 秒...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    radio.stop_rx();

    const std::size_t bytes = g_bytes.load();
    std::printf("收到 %zu 字节 / %zu 块（期望约 20e6 字节/秒）\n",
                bytes, g_blocks.load());
    const bool ok = bytes > 5'000'000;   // >5MB 即认为流水线健康
    std::printf(ok ? "冒烟通过\n" : "冒烟失败：数据量不足\n");
    return ok ? 0 : 1;
}
```

- [ ] **Step 4: CMake 加冒烟目标**

`CMakeLists.txt` 追加（含 DLL 复制，与主程序同一目录）：

```cmake
add_executable(HackRFSmoke tools/hackrf_smoke.cpp src/radio/hackrf.cpp)
target_include_directories(HackRFSmoke PRIVATE src ${MSYS2_UCRT64_INCLUDE})
flux_apply_compiler_options(HackRFSmoke)
foreach(dll libhackrf.dll libusb-1.0.dll libwinpthread-1.dll)
    add_custom_command(TARGET HackRFSmoke POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MSYS2_UCRT64_BIN}/${dll}" $<TARGET_FILE_DIR:HackRFSmoke>)
endforeach()
```

- [ ] **Step 5: 构建并在真机上运行冒烟**

Run: `cmake --build --preset x64-debug && hackrf_spiflash -R && ./out/build/x64-debug/Debug/HackRFSmoke.exe`
Expected: 打印 libhackrf 版本、`收到 ~10000000 字节`、`冒烟通过`，退出码 0。
（设备未插时打印 `打开失败: hackrf_open: ...` 属预期行为，非本步骤失败。）

- [ ] **Step 6: Commit**

```bash
git add src/radio/hackrf.hpp src/radio/hackrf.cpp tools/hackrf_smoke.cpp CMakeLists.txt
git commit -m "M1 radio：libhackrf 动态加载封装 + 真机冒烟

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 6: WinFlux 主窗口 + 控制面板（不接硬件）

**Files:**
- Create: `src/ui/views.hpp`（视图构建函数声明 + 共享配色）
- Create: `src/ui/views.cpp`（频谱视图、瀑布视图：view + Props::paint）
- Modify: `src/app/main.cpp`（App 组装 + 控制面板）
- Modify: `CMakeLists.txt`（主目标加源文件）

**Interfaces:**
- Consumes: WinFlux 全套（`view/label/button/text_input/slider/field/badge/segmented`、`Props::paint/paint_id`、`D2DRenderer::fill_rect/draw_line/draw_text/draw_polyline`、`Palette{surface,text_secondary,accent,divider,...}`、`Color{r,g,b,a}`）；`SpectrumFrame`、`WaterfallModel`
- Produces:
  - `ElementPtr spectrum_view(const Palette&, const dsp::SpectrumFrame& frame, double center_mhz)`
  - `ElementPtr waterfall_view(const Palette&, const dsp::WaterfallModel& wf, unsigned seq)`
  - `struct UiState`（main.cpp 内：running/status/freq_text/lna/vga/rate 状态指针集中管理）

- [ ] **Step 1: 写 `src/ui/views.hpp`**

```cpp
// 频谱与瀑布视图：WinFlux 元素 + Props::paint 自定义绘制
#pragma once

#include <flux/flux.hpp>

#include "dsp/analyzer.hpp"
#include "dsp/waterfall.hpp"

namespace hackrftool::ui {

// 频谱图：当前谱（accent 实线）+ 峰值保持（次级色细线）+ 网格 + 频率轴
[[nodiscard]] flux::ElementPtr spectrum_view(const flux::Palette& pal,
                                             const hackrftool::dsp::SpectrumFrame& frame,
                                             double center_mhz);

// 瀑布图：历史 64 行 × 256 列，16 级量化配色（深蓝→红）
[[nodiscard]] flux::ElementPtr waterfall_view(const flux::Palette& pal,
                                              const hackrftool::dsp::WaterfallModel& wf,
                                              unsigned seq);

} // namespace hackrftool::ui
```

- [ ] **Step 2: 写 `src/ui/views.cpp`**

```cpp
#include "ui/views.hpp"

#include <algorithm>
#include <string>

namespace hackrftool::ui {

namespace {
constexpr float kDbTop = 0.0f;
constexpr float kDbFloor = -100.0f;

float db_to_y(float db, float y, float h) noexcept {
    const float t = (kDbTop - db) / (kDbTop - kDbFloor);   // 0..1
    return y + 4.0f + t * (h - 20.0f);   // 上下留白给轴标签
}

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
        unsigned char(float(a.r) + f * float(int(b.r) - int(a.r))),
        unsigned char(float(a.g) + f * float(int(b.g) - int(a.g))),
        unsigned char(float(a.b) + f * float(int(b.b) - int(a.b))), 255};
}
} // namespace

flux::ElementPtr spectrum_view(const flux::Palette& pal,
                               const hackrftool::dsp::SpectrumFrame& frame,
                               double center_mhz) {
    flux::Props p;
    p.flex_grow = 1.0f;
    p.background = pal.surface;
    p.radius = 10.0f;
    p.paint_id = frame.seq;   // 帧不变则保持局部重绘
    p.paint = [pal, &frame, center_mhz](flux::D2DRenderer& r, float x, float y,
                                        float w, float h, bool, float, float) {
        // 网格：每 20 dB 一条
        for (float db = -20.0f; db > kDbFloor; db -= 20.0f) {
            const float gy = db_to_y(db, y, h);
            r.draw_line(x + 8.0f, gy, x + w - 8.0f, gy, pal.divider, 1.0f, 0.5f);
        }
        // 频率轴：中心 ±10 MHz
        const auto axis = [&](double mhz, const wchar_t* text) {
            const float fx = float(x + 8.0f + (float(mhz - (center_mhz - 10.0)) / 20.0f) *
                                              (w - 16.0f));
            r.draw_line(fx, y + 4.0f, fx, y + h - 20.0f, pal.divider, 1.0f, 0.4f);
            r.draw_text(flux::Rect{fx - 28.0f, y + h - 18.0f, 56.0f, 14.0f}, text,
                        10.0f, pal.text_secondary, false, flux::Align::center);
        };
        axis(center_mhz - 10.0, L"2440");
        axis(center_mhz, L"2450");
        axis(center_mhz + 10.0, L"2460");

        if (frame.db.empty()) {
            r.draw_text(flux::Rect{x, y, w, h}, L"等待数据…", 14.0f, pal.text_secondary,
                        false, flux::Align::center);
            return;
        }
        const std::size_t n = frame.db.size();
        std::vector<std::pair<float, float>> cur(n), peak(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float fx = x + 8.0f + float(i) / float(n - 1) * (w - 16.0f);
            cur[i] = {fx, db_to_y(frame.db[i], y, h)};
            peak[i] = {fx, db_to_y(frame.peak[i], y, h)};
        }
        r.draw_polyline(peak, pal.text_secondary, 1.0f, 0.55f);
        r.draw_polyline(cur, pal.accent, 2.0f, 1.0f);
    };
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
    p.paint = [pal, &wf](flux::D2DRenderer& r, float x, float y, float w, float h) {
        const auto snap = wf.snapshot();
        const std::size_t cols = wf.cols(), rows = wf.rows();
        const float cw = w / float(cols), rh = (h - 4.0f) / float(rows);
        for (std::size_t row = 0; row < rows; ++row) {
            for (std::size_t col = 0; col < cols; ++col) {
                const float db = snap[row * cols + col];
                // dB → 16 级：[-100,-40] 映射 [0,15]，<-100 视为底
                const int level = (db <= kDbFloor) ? 0
                    : std::clamp(int((db - kDbFloor) / 60.0f * 15.0f), 0, 15);
                r.fill_rect(flux::Rect{x + float(col) * cw, y + 2.0f + float(row) * rh,
                                       cw + 1.0f, rh + 1.0f},
                            wf_color(level));
            }
        }
        r.draw_text(flux::Rect{x + 8.0f, y + 4.0f, 160.0f, 16.0f}, L"瀑布（最新在上）",
                    10.0f, pal.text_secondary, false, flux::Align::start, 0.8f);
    };
    return flux::view(std::move(p));
}

} // namespace hackrftool::ui
```

**注意**：`paint` 回调签名须与 `Props::paint` 完全一致——`(D2DRenderer&, float x, float y, float w, float h, bool hovered, float mx, float my)`。瀑布 lambda 少写了三个尾参，实现时补齐为 `[..., ](..., bool, float, float)`。

- [ ] **Step 3: 重写 `src/app/main.cpp`（完整 UI，硬件留空）**

```cpp
// HackRFTool M1 —— 2.4G 频谱检测工具
#include <windows.h>

#include <algorithm>
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

    flux::State<bool>* running = nullptr;
    flux::State<std::wstring>* status = nullptr;
    flux::State<std::wstring>* freq_text = nullptr;   // MHz 文本
    flux::State<double>* center_mhz = nullptr;
    flux::State<double>* lna = nullptr;               // 8..40
    flux::State<double>* vga = nullptr;               // 2..62
    flux::State<int>* rate_index = nullptr;           // 0..3
    hackrftool::dsp::SpectrumFrame frame;             // 本帧快照（build 前拉取）
};

constexpr double kRatesMsps[4] = {8.0, 10.0, 16.0, 20.0};

double clamp_center(double mhz) noexcept {
    return std::clamp(mhz, 2400.0, 2483.5);
}

void apply_radio(App& app) {
    hackrftool::radio::RadioConfig cfg;
    cfg.center_hz = app.center_mhz->get() * 1e6;
    cfg.sample_rate_hz = kRatesMsps[size_t(app.rate_index->get())] * 1e6;
    cfg.lna_gain_db = unsigned(app.lna->get());
    cfg.vga_gain_db = unsigned(app.vga->get());
    std::string err;
    if (!app.radio.apply(cfg, &err)) {
        app.status->set(widen(err));
        return;
    }
    app.status->set(L"接收中 " + std::to_wstring(cfg.center_hz / 1e6) + L" MHz / " +
                    std::to_wstring(unsigned(cfg.sample_rate_hz / 1e6)) + L" Msps");
}

flux::ElementPtr build(App& app) {
    app.host.animate_for(100);   // 10 fps 心跳：驱动持续重绘

    // 帧拉取：新帧 → 瀑布推进
    const auto f = app.analyzer.snapshot();
    if (!f.db.empty()) {
        if (f.seq != app.frame.seq) app.waterfall.push(f.db);
        app.frame = f;
    }

    const flux::Palette& pal = app.host.palette();
    const bool running = app.running->get();

    // 顶栏
    auto header = flux::view(flux::Props{});
    header->children.push_back(flux::label(L"HackRFTool · 2.4G 频谱检测",
                                           flux::Props{.text_align = flux::Align::start,
                                                       .bold = true}));
    header->children.push_back(flux::ui::badge(
        pal, running ? flux::ui::BadgeKind::success : flux::ui::BadgeKind::neutral,
        running ? L"接收中" : L"已停止"));
    header->children.push_back(flux::ui::badge(pal, flux::ui::BadgeKind::info,
                                               L"仅接收"));

    // 控制行
    auto controls = flux::view(flux::Props{.direction = flux::Direction::row,
                                           .gap = 12.0f, .align = flux::Align::center});
    controls->children.push_back(flux::button(
        running ? L"停止" : L"开始",
        [&app] {
            if (app.running->get()) {
                app.radio.stop_rx();
                app.running->set(false);
                app.status->set(L"已停止");
            } else {
                std::string err;
                if (!app.radio.open(&err) ||
                    (apply_radio(app), false) ||
                    !app.radio.start_rx(
                        [](const std::int8_t* iq, std::size_t n, void* ctx) {
                            static_cast<hackrftool::dsp::SpectrumAnalyzer*>(ctx)->feed(iq, n);
                        },
                        &app.analyzer, &err)) {
                    app.status->set(L"启动失败: " + widen(err));
                    return;
                }
                app.running->set(true);
            }
        }));
    controls->children.push_back(flux::ui::field(
        pal, L"中心频率 MHz",
        flux::text_input(app.freq_text->get(),
                         [&app](std::wstring v) { app.freq_text->set(v); })));
    controls->children.push_back(flux::button(L"应用频率", [&app] {
        app.center_mhz->set(clamp_center(std::wcstod(app.freq_text->get().c_str(), nullptr)));
        if (app.running->get()) apply_radio(app);
    }));
    controls->children.push_back(flux::ui::field(
        pal, L"LNA " + std::to_wstring(unsigned(app.lna->get())) + L" dB",
        flux::slider(float(app.lna->get()), 8.0f, 40.0f, [&app](float v) {
            app.lna->set(double(unsigned(v / 8.0f + 0.5f) * 8));
            if (app.running->get()) apply_radio(app);
        })));
    controls->children.push_back(flux::ui::field(
        pal, L"VGA " + std::to_wstring(unsigned(app.vga->get())) + L" dB",
        flux::slider(float(app.vga->get()), 2.0f, 62.0f, [&app](float v) {
            app.vga->set(double(unsigned(v / 2.0f + 0.5f) * 2));
            if (app.running->get()) apply_radio(app);
        })));
    controls->children.push_back(flux::ui::field(
        pal, L"采样率 Msps",
        flux::ui::segmented(pal, {L"8", L"10", L"16", L"20"},
                            app.rate_index->get(), [&app](int i) {
                                app.rate_index->set(i);
                                if (app.running->get()) apply_radio(app);
                            })));

    // 状态栏
    auto status = flux::ui::caption(pal, app.status->get());
    status->props.text_align = flux::Align::start;

    auto root = flux::view(flux::Props{.direction = flux::Direction::column,
                                       .gap = 8.0f, .flex_grow = 1.0f});
    root->children.push_back(std::move(header));
    root->children.push_back(std::move(controls));
    root->children.push_back(hackrftool::ui::spectrum_view(
        pal, app.frame, app.center_mhz->get()));
    root->children.push_back(
        hackrftool::ui::waterfall_view(pal, app.waterfall, app.waterfall.seq()));
    root->children.push_back(std::move(status));
    return root;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    flux::enable_per_monitor_dpi_v2();

    App app;
    app.running    = app.host.make_state<bool>(false);
    app.status     = app.host.make_state<std::wstring>(L"未开始（点击「开始」）");
    app.freq_text  = app.host.make_state<std::wstring>(L"2450");
    app.center_mhz = app.host.make_state<double>(2450.0);
    app.lna        = app.host.make_state<double>(32.0);
    app.vga        = app.host.make_state<double>(30.0);
    app.rate_index = app.host.make_state<int>(3);
    app.host.set_root_builder([&app] { return build(app); });

    flux::Host::Config cfg;
    cfg.title = L"HackRFTool";
    cfg.width = 1200;
    cfg.height = 860;
    if (!app.host.create(cfg, instance)) return 1;
    return app.host.run();
}
```

**注意**：
- `flux::Props{...}` 指定初始化——若 `Props` 聚合初始化与成员默认值冲突（MSVC 对含默认成员初始化的聚合在 C++20 下允许），编译报错时改为逐行赋值风格（同 Dashboard 示例）。
- 上述「开始」分支里 `(apply_radio(app), false)` 的写法是为了先应用配置再启动接收；实现时可展开为清晰的三步 if，便于读。

- [ ] **Step 4: CMake 与构建**

`CMakeLists.txt` 主目标源文件改为：

```cmake
add_executable(HackRFTool WIN32
    src/app/main.cpp
    src/dsp/analyzer.cpp
    src/dsp/fft.cpp
    src/dsp/waterfall.cpp
    src/radio/hackrf.cpp
    src/ui/views.cpp)
```

Run: `cmake --build --preset x64-debug`
Expected: 零告警。若 `Props` 聚合初始化/lambda 签名报错，按源码内注释调整。

- [ ] **Step 5: 运行截图验证（无硬件）**

Run:
```bash
./out/build/x64-debug/Debug/HackRFTool.exe & sleep 2 && \
powershell -NoProfile -File tools/screenshot.ps1 -Out shot-m1-ui.png; \
taskkill //IM HackRFTool.exe //F
```
Expected: `shot-m1-ui.png` 生成；窗口含标题、控制行（开始按钮、频率输入、滑杆、分段选择）、空频谱区「等待数据…」、瀑布区、状态栏。
检查方式：读取 PNG（Read 工具可直接看图）。

- [ ] **Step 6: Commit**

```bash
git add src/app/main.cpp src/ui/views.hpp src/ui/views.cpp CMakeLists.txt shot-m1-ui.png
git commit -m "M1 UI：主窗口 + 控制面板 + 频谱/瀑布视图

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 7: 真机端到端联调

**Files:**
- Modify: 视需要微调 `src/app/main.cpp`、`src/ui/views.cpp`（以实测为准：配色、dB 范围、瀑布行数）

**Interfaces:**
- Consumes: Task 5 `HackRadio`、Task 6 完整 UI
- Produces: 可用的 v0.1

- [ ] **Step 1: 复位设备并启动**

Run: `hackrf_spiflash -R && ./out/build/x64-debug/Debug/HackRFTool.exe & sleep 3 && powershell -NoProfile -File tools/screenshot.ps1 -Out shot-m1-live.png`
（保持进程运行中截图，然后 taskkill）

- [ ] **Step 2: 自动点「开始」验证数据流**

窗口程序无法脚本点击——改用带命令行参数：`main.cpp` 解析 `wWinMain` 第 3 参，若为 `auto` 则启动 500 ms 后自动触发开始逻辑（复用「开始」按钮 handler 提出的函数）。
实现：把「开始/停止」逻辑提为 `void toggle_rx(App&)`，`wWinMain` 里 `SetTimer` 或 `host.set_key_hook` 不可用时用线程：启动时若参数为 `auto`，起 `std::thread` 睡 500 ms 后 `toggle_rx`（需 `host.request_render` 线程安全性——保守做法：参数 auto 时直接在 `create` 前打开设备并 start_rx，再进入消息循环）。
简化决定：**接受 `auto` 参数 = 启动即开接收**（在 `host.create` 之前完成 open/apply/start_rx，失败则弹状态栏文字）。

Run: `hackrf_spiflash -R && ./out/build/x64-debug/Debug/HackRFTool.exe auto & sleep 4 && powershell -NoProfile -File tools/screenshot.ps1 -Out shot-m1-auto.png; taskkill //IM HackRFTool.exe //F`
Expected: 频谱线出现起伏（2.4G 频段环境信号：WiFi/蓝牙活跃时 2450±10 MHz 内可见多个凸起），瀑布出现彩色横纹。
检查方式：Read `shot-m1-auto.png` 人工确认频谱非平线、瀑布有色彩梯度。

- [ ] **Step 3: 性能粗检**

观察截图与运行 1 分钟无崩溃、无告警输出；任务管理器记录 CPU（记入 M1 交付记录，不设硬门槛）。

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "M1 联调：真机 2.4G 频谱 + 瀑布实时显示

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 8: 收尾——文档与推送

**Files:**
- Create: `docs/m1-交付记录.md`
- Modify: `README.md`（状态表 + §4.5 M1 交付物）
- .gitignore 追加 `shot-*.png` 之外的临时截图（保留两张关键截图入库）

- [ ] **Step 1: 写交付记录 `docs/m1-交付记录.md`**

内容要点（以实测为准填写）：构建产物路径、测试结果（ctest 项数）、冒烟字节率、真机截图引用、已知限制（20 Msps 时 USB 总线拥挤提示、瀑布 16k fill_rect/帧的渲染余量、HackRF Pro 每次 `hackrf_spiflash -R`）、下一步 M2 计划。

- [ ] **Step 2: 更新 README**

§1 状态行改为：`M1 完成（2026-09-04）：频谱+瀑布 v0.1，见 docs/m1-交付记录.md`；§4.5 M1 行交付物改为 `工具 v0.1 ✅`。

- [ ] **Step 3: 全量验证 + 提交推送**

Run:
```bash
cmake --build --preset x64-debug && ctest --preset x64-debug && \
git add -A && git commit -m "M1 完成：文档与交付记录

Co-Authored-By: Claude Code <noreply@anthropic.com>" && git push
```

---

## Self-Review 结论

- **Spec 覆盖**：F1（全频段 2400–2483.5 单窗 20 MHz + 中心频率/带宽(采样率)/增益可调 + 峰值保持 + 频谱/瀑布）→ Task 3/6/7；F4（控制面板/状态栏/开始停止）→ Task 6；M0 前置（驱动/工具/软复位）→ Task 5 Step 5 与 Global Constraints。全频段步进扫描（5 段）不在 M1（README M1 定义为单窗），M2 处理。
- **占位符扫描**：无 TBD/TODO；UI lambda 签名风险点已在正文显式标注修正方式。
- **类型一致性**：`SpectrumFrame{db,peak,seq}`、`WaterfallModel::seq()`、`HackRadio::Callback`、`RadioConfig` 各任务引用一致；`paint_id` 分别来自 `frame.seq` / `wf.seq()`。
