# 新功能：收音机（立体声）+ 卫星云图（NOAA APT / Meteor）——分三轮交付

## 总架构（一次搭建，三轮填充）

```
rx 线程（libusb）: rx_trampoline → analyzer + live + recorder + iq_ring（新 SPSC 字节环，~4 MB）
fm 专职线程: iq_ring 读出 → FIR 低通抽取 fs→250 kHz → FM 正交鉴频 → 分路：
  ├─ 收音机: 19 kHz 导频 PLL → 38 kHz DSBSC 解调(L−R) → 立体声矩阵 → 50µs 去加重 → 48 kHz → waveOut
  ├─ NOAA APT: 2.4 kHz 子载波同步检波 → 1040 Hz 行同步 → 2080 px 行装配 → 图像环形缓冲 → 原生 GDI 显示窗
  └─ Meteor LRPT: QPSK Costas 环 + Gardner 定时 → 符号流 → CCSDS ASM 帧同步 →（解压缩后置）
```
- 共用一条 FM 前端（FIR+鉴频），三个消费者；HackRF 单通道半双工——同一时刻只收一个频点，页面即"当前收听目标"。
- 采样率：`kRatesMsps` 增加最低档 **2 Msps**（hackrf.cpp 已确认直接透传成功；顺带关闭池中 T4.5）；进入收音/云图页自动切 2 Msps。
- **连带修正**：现有代码按 20 Msps 硬编码频谱窗宽（`spectrum_view` 的 ±10 MHz、`mhz_to_bin` 的 20.0）——按实际采样率折算，否则低速档频谱/监测频标错位。
- 音频：waveOut（winmm）48 kHz 16-bit 立体声，4×100 ms 缓冲队列；导频幅度不足自动回退单声道（页面显示立体声/单声道指示）。

## #53 收音机（立体声 + 自动扫台）

- `src/dsp/fm.{hpp,cpp}`：`FmReceiver` 纯处理类（feed(iq) → 抽取 → 鉴频 → MPX → 立体声/单声道判定 → 音频块回调）；19 kHz 二阶导频 PLL、38 kHz 相干解调、50 µs 去加重、250 k→48 k 音频重采样。
- `src/audio/waveout.{hpp,cpp}`：waveOut 队列（仅主程序 target，链 winmm）。
- UI：页签组 +2（「收音」「云图」，图标 GDI 自绘）；收音机页上下文行 = 频率 EDIT + 预设 COMBO + 音量 TRACKBAR + 静音 CHECK + 「扫描」按钮；内容区 = 解调电平/立体声指示/音频电平表头（WinFlux）。
- 自动扫台：后台线程 87.5→108 MHz 步进 100 kHz，每点驻留 80 ms 取 analyzer 均值峰，超阈值记台填入预设下拉；扫完自动跳回最强台。
- 测试（红→绿）：鉴频器相位差分还原频率；FIR 抽取阻带衰减；导频 PLL 锁定+DSBSC 往返（合成 L/R→MPX→FM→IQ→FmReceiver 输出与原信号相关系数 >0.9）；去加重时间常数。预计 +25 断言。

## #54 卫星云图（NOAA APT）

- `src/dsp/apt.{hpp,cpp}`：2400 Hz 子载波 I/Q 同步检波 → 行同步（1040 Hz 相关器）→ 行对齐 → 2080×滚动缓冲灰度行；纯函数。
- `src/ui/` APT 显示 = **原生子窗口 + GDI `StretchDIBits`**（WinFlux 渲染器无位图接口——探查确认；原生骨架正合适）：宽 2080 灰度 DIB，按内容区缩放滚动显示，layout() 接管几何（page==4 显示）。
- 上下文行：卫星预设 COMBO（NOAA 15/18/19：137.620/137.9125/137.100 MHz）+ 手动频率 + 「记录」CHECK（累计图像）+ 「保存 PNG」按钮（GDI+ 编码，链 gdiplus）。
- 测试：合成 APT 音频（同步串+灰度梯调图案）→ 解码断言行对齐偏移与像素灰度线性；端到端 FM 合成。预计 +15 断言。

## #55 卫星云图（Meteor M2 LRPT）

- `src/dsp/meteor.{hpp,cpp}`：QPSK 软解调（Costas 载波环 + Gardner 符号定时，72 ksym/s 带内 120 kHz）→ CCSDS ASM（0x1ACFFC1D）帧同步 + 丢帧统计。
- 输出 v1：解调符号/帧记录落盘（供离线解码）+ 状态栏锁定指示（EVM/帧计数）。**图像解压缩（去交错/维特比/压缩图像重组）明确后置**——这是 wxtoImg/satdump 级工程，视 #55 后余力单独成轮。
- 测试：合成 QPSK 加噪 → Costas 锁定 → 符号误码率断言；ASM 检测断言。预计 +12 断言。

## 验证与合规

- 每轮：构建零告警 → 合成信号单测全绿 → 真机 ctest 5/5 → 运行截图核验（收音机页表头/扫台列表；云图页图像窗口）。
- 全程只收不发（合规边界不变）；FM 广播与 NOAA APT 均为公开广播业务，README 增补功能说明。
- 真实卫星过境与实际电台收听效果依赖天线（ANT500 覆盖 75 MHz–1 GHz ✓），合成测试先行兜底。
- 每轮独立提交（evolve #53/#54/#55），日志与经验库同步更新。