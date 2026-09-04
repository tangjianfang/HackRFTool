# HackRF 学习知识库 · 索引

> 整理日期：2026-09-04。原"按下载来源堆放"的结构已按用途归类为 K1–K7 七类。
> 迁移映射与说明见本文件；仓库副本：`C:\tjf\github\HackRFTool\docs\knowledge-base-INDEX.md`。
> 标注 ★ 的条目与 HackRFTool 项目（2.4G 信号检测工具）直接相关。

## 01-入门实操（K1）

| 条目 | 说明 |
|------|------|
| `第一件事-Windows接收FM/` ★ | 拿到设备的第一课：SDRSharp 听 FM 广播。含 zadig-2.8 驱动（WinUSB）、SDRSharp x86、视频与图文教程 |
| `使用注册事项.pdf` | 设备使用注意事项（原文件名"使用注册事项"） |
| `FMAM电台发射实验/` | Windows 下 FM/AM 发射实验（播放器 + 音频素材）。**发射需业余无线电资质，仅学习参考** |

## 02-软件工具（K2）

| 条目 | 说明 |
|------|------|
| `Windows收发软件/` ★ | **HackRF-tools-1.0.2-2023.01.1-x64.exe**（官方主机工具）、radioconda-2024.01.26、SDRangel-6.16.3 |
| `HackRF频谱分析仪/` ★ | hackrf_spectrum_analyze 频谱分析工具（Java，附 JDK 18 运行环境） |
| `SDR软件合集/` | SDR#、GNU Radio Windows 版、SDR++、SDRangel、SDRConsole、SDRUno、PlutoSDR、RX888、Malahit（孔雀石）、MMDVM 等及汉化包 |
| `安卓接收软件/` | RF Analyzer（两个版本压缩包，内容相近，未解压） |
| `周边工具与汉化/` | 解码工具、频谱分析插件、虚拟串口/声卡工具、卫星相关、HAM 软件汉化 |

## 03-固件（K3）

| 条目 | 说明 |
|------|------|
| `HCK1固件版本/` | release 2022.09.1 / 2023.01.1 / 2024.02.1。⚠️ **不适用于本项目设备**（HackRF Pro/Praline 平台，已运行 2026.01.3 固件） |
| `Linux更新主机软件.txt` | Linux 下更新主机软件的操作笔记 |

## 04-GNURadio实验（K4）

| 条目 | 说明 |
|------|------|
| `实验资料/` ★ | GRC 流图文件 + PDF 实验文档 |
| `GRC示例集/` ★ | 大量 .grc 示例，**含 2.4GHz 相关示例**（SSB 发射为主，另有 1.2G/10G 等）；本项目算法原型阶段直接可用 |

## 05-视频课程（K5）

| 条目 | 说明 |
|------|------|
| `Michael Ossmann SDR课程/` | HackRF 作者的 11 集 SDR 课程视频 + Lessons.txt（SDR 教学经典） |
| `Software Defined Radio with HackRF lesson.zip` | 同名课程配套课件包 |
| `U盘硬盘装Ubuntu/` | 移动硬盘安装 Ubuntu 系统教学视频（GNU Radio 环境准备） |

## 06-参考资料（K6）

| 条目 | 说明 |
|------|------|
| `HackRF One 使用手册.pdf` ★ | 官方使用手册 |
| `GNU Radio入门 V0.99.pdf` | GNU Radio 入门教程 |
| `Sensitivity of the HackRF.pdf` | HackRF 灵敏度测试报告 |
| `原理图schematic/` | HackRF One 电路原理图（PDF） |

## 07-合规法规（K7）

| 条目 | 说明 |
|------|------|
| `中华人民共和国无线电频率划分规定.pdf` ★ | 频率划分法规全文 |
| `中国业余无线电频段一览图.jpg` / `中国无线电频率划分图2018版.jpg` | 频谱划分速查图 |

## _archive-压缩包（隔离区）

与已解压目录重复的压缩包，确认无误后可整体删除：

- `01-入门实操__sdrsharp-x86.zip`（已解压在 01-入门实操/第一件事-Windows接收FM/sdrsharp-x86）
- `01-入门实操__zadig-2.8.zip`（已解压在同名目录）

## 整理规则备忘

1. 目录 `01-…` ~ `07-…` 与需求文档（README §3.2）K1–K7 一一对应
2. 命名微调：去特殊字符（如智能引号）、错别字（ubantu→Ubuntu）、无意义前缀（"01 第一件做的事"）
3. 未做深层递归整理（软件合集内部保持原样），后续按需整理
