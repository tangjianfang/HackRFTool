# HackRFTool

基于 HackRF One 的 2.4 GHz 信号检测工具开发项目，同时整理 HackRF 学习资料形成知识库。

> 本文档由原始需求草稿（`readme.txt`）整理而来，是项目的需求文档；原始草稿保留不动。

## 1. 项目简介

| 事项 | 内容 |
|------|------|
| 目标一 | 整理 `C:\baidunetdiskdownload\HackRF` 的 RF 开发/学习资料，形成结构化知识库（见第 3 章） |
| 目标二 | 开发 2.4 GHz 信号检测工具：检测信号强度与稳定性，并尝试抓取 2.4 GHz 空口数据包（见第 4 章） |
| 扩展功能 | **收音机**（FM 立体声 87.5–108、AFC 自动微调、静音带宽三档、人声强度波形、音频 FFT 频谱 0–24kHz 含 19k 导频线）· **卫星云图**（NOAA APT 三星预设 137.100/137.620/137.9125、链路状态卡、PNG 保存；Meteor M2 LRPT QPSK 解调 137.900、眼图/ASM 帧同步诊断）· **信号库**（扫描累积、多条件筛选、随机收听、非模态弹窗双击调谐）· **设置持久化**（21 项参数 settings.tsv，重启即用含自动接收）· **频谱 X/Y 轴缩放**（×1–8 + 100/60/40dB 档）· **遥测日志**（UI/点击/事件/数据全量 JSONL + 应用内日志查看器 + log-assert 断言工具——UI 验收走日志不走截图） |
| 当前状态 | **M0–M6 全部完成（2026-09-06）**：v0.1 频谱+瀑布（[M1](docs/m1-交付记录.md)）· v0.2 信道监测/统计/CSV（[M2](docs/m2-交付记录.md)）· v0.3 抓包离线链路（[M3](docs/m3-交付记录.md)）· v0.4 全频段扫描（[M4](docs/m4-交付记录.md)）· v0.5 实时抓包 UI + ESB 协议解帧（[M5](docs/m5-交付记录.md)）· v0.6 自动化测试体系（[M6](docs/m6-交付记录.md)，`ctest --preset x64-release` 一键 5/5）；演进至 #97：UI 三级架构（一级全局工具栏/二级通用+页特有设置行/内容区纯显示+图内轴刻度规范，[#87-#97](docs/evolve-log.md)）· 收音机/云图/轨道页/信号库/遥测/卫星过境预测 |

## 2. 背景与资源

### 2.1 硬件

- **HackRF One × 1（已有）**——实测为 **HackRF Pro**（Board ID 5，平台代号 Praline，2026 年新品）
- 已知问题：每次插入 USB 后需执行 `hackrf_spiflash -R` 软复位一次（固件已知 bug，详见 [M0 验证记录](docs/m0-环境验证记录.md)）
- 关键规格：频率范围 1 MHz – 6 GHz；最大采样率 20 Msps（8-bit I/Q）；模拟带宽 20 MHz；半双工；USB 2.0 接口
- 固件：资料库 `HCK1固件版本/` 内含 2022.09.1 / 2023.01.1 / 2024.02.1 三个版本
- Windows 驱动：Zadig（WinUSB），资料库 `zadig-2.8/` 已备

### 2.2 资料库

`C:\baidunetdiskdownload\HackRF` —— RF 开发和学习的基础资料与工具集，分类方案见第 3 章。

### 2.3 界面库（WinFlux）

- 路径：`C:\tjf\github\WinFlux`（开发中，使用其生成的 SDK）
- 技术：C++20 / Win32 / Direct2D 声明式 UI（Element / State\<T\> / Flex 布局 / 主题令牌），单 HWND + DirectComposition 呈现
- 构建：CMake + VS2022，x64，`/W4` 零告警

### 2.4 开发环境

- Windows + Visual Studio 2022（本机已装齐所需组件）
- 语言/框架：C++、Win32 API；界面使用 WinFlux SDK
- 产物形态：Windows x64 桌面应用

### 2.5 构建与运行（统一 Release 路径，T4.4）

```sh
cmake --build --preset x64-release      # 或仓库根 build.bat（只构建）
ctest --preset x64-release              # 一键验收：5/5（无真机时自测记 SKIP）
```

- 主程序：`out/build/x64-release/Release/HackRFTool.exe`（libhackrf 等三个 DLL 由构建后步骤自动复制到 exe 旁）
- 真机自测：`HackRFTool.exe selftest`（单窗 6s）/ `selftestsweep`（全频段 8s），报告落 exe 旁；命令行模式另有 `auto`/`autom`/`autos`/`autocap`/`autoradio`/`autowx`/`autosc`
- 数据文件（exe 旁）：`settings.tsv`（21 项参数持久化）、`signals.tsv`（信号库）、`hackrftool.jsonl`（结构化遥测，`python tools/log-assert.py <日志> "LIFE:app.start,RADIO:tune"` 断言）
- 真机注意：每次插 USB 先 `hackrf_spiflash -R` 软复位（固件已知 bug）；合规边界只收不发

## 3. 知识库整理需求

### 3.1 目标

把资料库从"按下载来源堆放"整理为按用途分类、命名规范、有索引、无重复的知识库，支撑第 4 章的工具开发。

> **状态：已完成（2026-09-04）**。资料库已重组为 `01-入门实操` … `07-合规法规` 七类，重复压缩包隔离至 `_archive-压缩包`，索引见 [docs/knowledge-base-INDEX.md](docs/knowledge-base-INDEX.md)（知识库根目录亦有同名 INDEX.md）。

### 3.2 分类方案

| # | 类别 | 现有内容（原始目录） |
|---|------|----------------------|
| K1 | 入门实操 | `01 第一件做的事 Windows 接收FM/`（SDRSharp + Zadig + 图文/视频教程）、`01 使用注册事项.pdf` |
| K2 | 软件工具 | `SDR软件/`（SDR#、GNU Radio Windows 版、SDR++、SDRangel、SDRConsole、SDRUno、PlutoSDR、RX888、Malahit、MMDVM 等及汉化包）、`windows 接收发送软件/`（HackRF-tools 1.0.2 x64、radioconda 2024.01.26、SDRangel 6.16.3）、`HackRF频谱分析仪/`（hackrf_spectrum_analyze + JDK 18 运行环境）、`安卓接收软件/`（RF Analyzer）、`教程软件插件学习资料/`（解码、频谱分析、虚拟串口/声卡等周边工具） |
| K3 | 固件 | `HCK1固件版本/`（release 2022.09.1 / 2023.01.1 / 2024.02.1） |
| K4 | GNU Radio 实验 | `HackRF_GNURadio实验资料/`（GRC 文件 + PDF 实验文档）、`hackrf-gnuradio-grc-examples/`（大量 .grc 示例，含 2.4 GHz 相关示例，发射为主） |
| K5 | 视频课程 | `Michael Ossmann's SDR class/`（11 课视频 + Lessons.txt）、`其他/Software Defined Radio with HackRF lesson.zip`、`U盘硬盘装ubantu/`、`windows FMAM电台发射/` |
| K6 | 参考资料 | `schematic/`（HackRF One 原理图）、`GNU Radio入门 V0.99.pdf`、`HackRF One 使用手册.pdf`、`Sensitivity of the HackRF.pdf` |
| K7 | 合规法规 | `中华人民共和国无线电频率划分规定.pdf`、`中国业余无线电频段一览图.jpg`、`中国无线电频率划分图2018版.jpg` |

### 3.3 整理动作

1. 按上表把文件归类到统一目录结构（建议 `01-入门实操/` … `07-合规法规/`，与 K1–K7 一一对应）
2. 命名规范化：去空格与特殊字符，版本号写进文件名（如 `hackrf-tools-1.0.2-2023.01.1-x64.exe`）
3. 去重：压缩包与已解压目录二选一，只保留一份
4. 在根目录建立 `INDEX.md`：每个类别一节，逐条列出文件名 + 一句话说明
5. 在索引中标注与本项目直接相关的资料（★）：2.4 GHz GRC 示例、频谱分析工具、HackRF 主机工具链与固件、无线电频率划分规定

### 3.4 验收标准

- 每个文件恰好归入一个类别，根目录无孤儿文件
- 无重复副本（压缩包/解压目录不并存）
- `INDEX.md` 覆盖全部条目，能按图索骥找到任何资料
- 与工具开发直接相关的资料在索引中有 ★ 标注

## 4. 2.4 GHz 信号检测工具需求

### 4.1 背景与目标

- 2.400–2.4835 GHz ISM 频段是无线鼠标/键盘的主要通信频段。私有 2.4G 协议多采用 GFSK 调制（1–2 Mbps，信道间隔 1 MHz，与 nRF24L01 系列兼容的信道划分常见）；蓝牙设备为跳频体制，不在首期目标内
- 工具目标：以 HackRF One 为接收机，检测该频段空口信号的**强度**与**稳定性**，并在可行时**捕获数据包**
- 检测对象：无线鼠标/键盘的空口通信（over-the-air）

### 4.2 功能需求

| ID | 功能 | 优先级 | 说明 |
|----|------|--------|------|
| F1 | 频谱检测 | P0 | 覆盖 2.400–2.4835 GHz 全频段；频谱图 + 瀑布图实时显示；中心频率/带宽/LNA/VGA 增益可调；峰值保持 |
| F2 | 稳定性分析 | P0 | 选定信道长期监测；RSSI-时间曲线；统计指标（均值/峰值/方差、活动占空比）；数据记录与 CSV 导出 |
| F3 | 信号抓包 | P1 | 固定信道 IQ 捕获 → GFSK 解调 → 包边界识别；原始包数据落盘保存；先离线处理，后逐步实时化 |
| F4 | 界面交互 | P0 | WinFlux 主窗口：参数控制面板、实时频谱/瀑布视图、开始/停止、状态栏（设备状态、采样率、丢帧计数） |

优先级含义：P0 = 首个可用版本必须具备；P1 = 增强目标（原始需求中抓包为"最好能"，故先离线验证可行性，再决定实时化投入）。

### 4.3 非功能需求

- 实时性：频谱/瀑布图刷新 ≥ 10 fps
- 流式处理：频谱/监测路径下 20 Msps I/Q 实时处理、不落盘（环形缓冲），内存占用有界（F3 抓包的 IQ 录制不受此限）
- 稳定性：连续运行 ≥ 1 小时不崩溃、无内存泄漏（稳定性分析本身需要长时间运行）
- 线程模型：采集/处理线程与 UI 线程分离，数据经线程安全队列传递，界面不卡顿
- 工程规范：与 WinFlux 一致 —— C++20、x64、`/W4` 零告警

### 4.4 技术方案要点

- **主机侧驱动链**：Zadig/WinUSB → libhackrf（`hackrf_transfer` 流式回调，8-bit I/Q）；官方工具链（HackRF-tools 1.0.2、固件 2024.02.1）已备于资料库，用于 M0 验证
- **扫描策略**：ISM 频段宽 83.5 MHz > 单窗 20 MHz，需分 5 段步进扫描；聚焦模式可对准常用信道（1 MHz 间隔）驻留
- **解调**：PC 端软件 GFSK 解调（20 Msps 下：1 Mbps → 20 样本/符号，2 Mbps → 10 样本/符号）
- **算法验证**：先用资料库中的 GNU Radio / GRC 示例做算法原型，验证通过后再以 C++ 落地到工具中
- **界面**：WinFlux 声明式 UI，数据侧仅把"频谱帧/统计快照"推入 UI 状态

### 4.5 阶段规划

| 阶段 | 内容 | 交付物 |
|------|------|--------|
| M0 | 环境就绪：装驱动、更新固件、跑通 `hackrf_info` / `hackrf_transfer`，确认硬件与主机库工作正常 | 环境验证记录 |
| M1 | 频谱检测（F1 + F4）：单窗口实时频谱 + 瀑布图的最小可用工具 | 可运行工具 v0.1 |
| M2 | 稳定性分析（F2）：信道驻留监测、统计与导出 | 工具 v0.2 |
| M3 | 抓包试验（F3）：IQ 录制 → 离线 GFSK 解调 → 评估实时化 | 工具 v0.3（增强） |

### 4.6 合规边界

- 本工具**仅接收不发射**；如后续需发射实验，须持有相应业余无线电操作资质并遵守国家法规
- 监测与抓包对象限于**自有设备**，用途限于协议分析与安全研究/学习
- 无线电活动遵守《中华人民共和国无线电频率划分规定》（资料库 K7 类已含该文件）
