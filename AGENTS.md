# AGENTS.md — HackRFTool

基于 HackRF One 的 2.4 GHz 信号检测工具（Windows x64 桌面应用）：实时频谱+瀑布、信道监测统计、全频段扫描、实时抓包 + ESB 协议解帧。M0–M6 全部完成；任何改动的验收契约就是下面这条命令。

## 构建与验证

```sh
cmake --build --preset x64-release && ctest --preset x64-release
```

- 一键构建脚本：`build.bat`（任意 CWD 调用均可，自动定位仓库根：配置 + 构建 x64-release 全部 target；只构建不跑测试；ASCII/CRLF——cmd 按 ANSI 码页解析批处理）

- UI 行为验证走日志：exe 旁 hackrftool.jsonl（结构化遥测：UI/点击/DSP/APT/扫描/生命周期）+ python tools/log-assert.py <日志> "LIFE:app.start,RADIO:tune" 可选 --order（顺序断言）/ --tail N——截图仅核对布局（视觉识别有幻觉，lessons L14）
- 遥测分类速查：LIFE 起停 / UI cmd·state / RADIO tune·reconfig·sweep·apply.fail / AUDIO fm.on·off / DSP frame(非fm页)·fm(1Hz) / APT diag / SCAN start·done / SIGDB / SETTINGS / ESB hit
- 唯一验收命令，应 5/5 通过（217 断言单测 + 端到端合成管线 + WinFlux 测试；两个真机整机自测无设备时退出码 42 → CTest 记 SKIP）
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

## UI 架构（#52 原生 Win32 骨架）

- 骨架 = 原生主窗口（类 `HackRFToolMain`）：顶部 **ToolbarWindow32**（#88 三级架构：一级工具栏只放全局动作——启停/页签×6 单选组/全频段/录制 IQ/日志；页特有动作按钮归位各页二级行，命令 ID 不变：应用→通用行、锁定/导出 CSV→监测行、清空→抓包行、扫描信号/随机收听→收音行1、信号库→收音行2；图标为 GDI 运行时自绘 24px、洋红键转 alpha）+ 设置行原生控件（**行0=通用设置常驻、行1=页特有、收音行2=筛选/音频**——#87 根因修复：place 按行摆放，严禁页特有行与通用行同线叠放；轨道页隐藏云图专属「记录/保存 PNG」；EDIT/COMBOBOX/TRACKBAR/CHECKBOX，随页签显隐；**组合框 MoveWindow 高度必须含下拉列表**——Win32 契约，ctl_h 只给静态高度会展不开）+ 底部 **msctls_statusbar32** 六分段（文本来自 `status_parts` 纯函数）
- 中间内容区 = WinFlux Host **重父化子窗口**（`Host::create` 后清 WS_POPUP 家族、加 WS_CHILD，SetParent 进主窗）：只渲染图表（D2D/DComp 管线不变），控件值是 App 普通字段（每帧重建，无 flux::State——set-收尾铁律随之退役，见 lessons L4 注记）
- 外层 GetMessage 泵（非 `host.run()`）；WM_QUIT 来自主窗销毁或 selftest 线程；`sync_chrome` 在 build 心跳里同步工具栏态（有缓存去重）与状态栏分段
- comctl32 v6 清单内嵌于 `src/app/app.rc`（CMake 已设 `/MANIFEST:NO` 防重复）；DPI 感知靠 `flux::enable_per_monitor_dpi_v2()`（**wWinMain 里、任何窗口创建前**——重写 main.cpp 时误删过一次，整窗模糊，L7）
- **反闪烁铁律（L8/L9/L10）**：主窗口类样式禁用 CS_HREDRAW/CS_VREDRAW；**禁用 WS_EX_COMPOSITED（与 DComp 子窗冲突，工具栏会变黑）**；**工具栏禁用 TBSTYLE_FLAT**（透明样式需父窗补画背景，热跟踪重绘=黑底——L10）；WM_ERASEBKGND 返 1、WM_PAINT 只填 `ps.rcPaint`、**WM_PRINTCLIENT 填 BTNFACE**（透明子控件背景请求）；子控件与内容区 MoveWindow 一律 bRepaint=FALSE；状态栏文本缓存+4Hz 节流；内容区拖拽冻结 = `live_sizing`（ENTERSIZEMOVE 置位）期间 layout 早退——**必须配套自愈**：几何看门狗（build 心跳核对 host 实际/目标矩形，`host_target` 与 layout 共用算法，漂移一帧内校正）+ 停顿解冻（窗口尺寸停变 500ms 即解冻，`last_size_ms`）。ENTERSIZEMOVE/EXITSIZEMOVE 不保证成对、且跨进程伪造不被投递——任何依赖消息配对的机制都要有看门狗兜底
- 已知上游限制：WinFlux `Host::create` 硬编码 SW_SHOW → 启动时顶层窗闪现一帧后才重父化（wontfix-upstream，建议上游加 Config.visible）；PrintWindow 截图会漏画原生子控件 → 验证 UI 用 `tools/screenshot-fg.ps1`（前台 BitBlt）
- 工程规范：C++20 / 仅 x64（CMake 强制）/ `/W4` 零告警（`flux_apply_compiler_options` 统一施加）；WinFlux 仓库经 `add_subdirectory` 引入（`WINFLUX_ROOT` 默认 `C:/tjf/github/WinFlux`），上游组件问题本仓库不可修，只记录

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
