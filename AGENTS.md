# AGENTS.md — HackRFTool

基于 HackRF One 的 2.4 GHz 信号检测工具（Windows x64 桌面应用）：实时频谱+瀑布、信道监测统计、全频段扫描、实时抓包 + ESB 协议解帧。M0–M6 全部完成；任何改动的验收契约就是下面这条命令。

## 构建与验证

```sh
cmake --build --preset x64-release && ctest --preset x64-release
```

- 唯一验收命令，应 5/5 通过（119 断言单测 + 端到端合成管线 + WinFlux 测试；两个真机整机自测无设备时退出码 42 → CTest 记 SKIP）
- **跑 ctest 前先确认构建零 error**：构建失败后 ctest 跑的是陈旧二进制，"全绿"是假象（docs/lessons.md L1，已两次踩中）
- 整机自测必须 Release：Debug 下全量 FFT 会 CPU 饥饿
- Preset：`x64-debug` / `x64-release`（VS2022 生成器），产物在 `out/build/<preset>/`；libhackrf 等三个 DLL 由构建后步骤从 `C:/msys64/ucrt64/bin` 复制到 exe 旁
- 真机自测入口：`HackRFTool.exe selftest`（单窗 6s）/ `selftestsweep`（全频段 8s），报告落 exe 旁

## 结构与分层

- `src/dsp/` — 纯 DSP（频谱 analyzer、waterfall、channel_monitor、panorama、burst_detector、gfsk、esb 解帧、live_bursts 环形缓冲、level_map 色阶）。**不得依赖 UI 或硬件**；新算法优先写成纯函数，让 `tests/dsp_test.cpp` 能直接单测
- `src/radio/` — 硬件层：`hackrf.hpp` 用 LoadLibrary/GetProcAddress 动态加载 libhackrf（MSVC 链不了 MSYS2 的 MinGW 导入库，**不要改成链接 .dll.a**）；接收回调在 libusb 线程执行，回调内禁止调用本类方法，只许拷贝数据
- `src/ui/` — WinFlux 声明式视图；数据侧只把"频谱帧/统计快照"推入 UI 状态
- `src/app/main.cpp` — UI 组装、线程与状态（仓库最大文件 1200+ 行）
- `tools/` — 控制台工具（hackrf_smoke / iq_capture / gfsk_analyze）与排障脚本；每个 CMake target 只编译 CMakeLists 里列出的 src 子集，**新增文件须同步加进相关 target**

## WinFlux UI 铁律

- **`State::set()` 必须永远是处理器最后一句**：set() 会同步重建元素树、释放正在执行的 lambda，set 之后访问 app 即 UAF（真实崩溃根因，lessons.md L4）。副作用全部前置；多步逻辑包成自由函数、`App&` 走参数
- 工程规范：C++20 / 仅 x64（CMake 强制）/ `/W4` 零告警（`flux_apply_compiler_options` 统一施加）
- WinFlux 仓库经 `add_subdirectory` 引入（`WINFLUX_ROOT` 默认 `C:/tjf/github/WinFlux`），上游组件问题本仓库不可修，只记录

## 已知坑

- 合成测试信号必须把载波搬进带内（如 +2.5 MHz）：analyzer 为硬件基带跌落裁掉数组两端（DC 与负频边缘 bin）
- 真机每次插 USB 后先 `hackrf_spiflash -R` 软复位一次（固件已知 bug）
- 数值断言的期望值用脚本算，不要心算（L2）
- 同一工作树开多个并行会话会互相踩（增量构建竞速、Edit 锚点失配）：开工前确认无邻居会话，文件被外部修改立即重读再改（L3）
- 合规边界：**只收不发**；抓包对象限自有设备

## 文档

- `docs/lessons.md` — 经验库：踩坑/复盘先更新这里，同主题改条目而非加重复
- `docs/m0…m6-*.md` — 里程碑交付记录（含排障细节与根因分析）
- `docs/evolve-log.md` — 演进迭代状态（verify 命令、目标池、轮次记录）
- `readme.txt` 是原始需求草稿，保留不动；`README.md` 是整理后的需求文档

## 约定

- 注释、提交信息、文档全用中文；提交风格如 `M6 完成：…` / `evolve #N: …` / `修复…（根因）`
