# evolve log — HackRFTool

- verify: cmake --build --preset x64-release && ctest --preset x64-release   # 5 ctests（单测/真机自测×2/WinFlux×2，无设备自动 SKIP）+ 单测 86 断言
- pointer: #1 (next round)
- rounds done: 0
- status: initialized
- metrics: findings 0 | fixes 0 | regressions 0

## Target pool

- Tier 1 (known defects, grep 已核):
  - T1.1 tools/iq_capture.cpp "期望字节数" 表达式残留死代码 `double(bytes?bytes:1)*0.0 + ...`（M3 遗留，能跑但丑）
  - T1.2 selftest 报告写进程 CWD（CTest 下落 build 目录）——应落 exe 旁（M6 已知）
  - T1.3 monitor_page 在扫描模式下"跟踪当前驻留段"的说明文字缺失（M2 遗留）
  - T1.4 WinFlux tabs 三枚汉字间距挤——上游组件问题，仓库内不可修（blocked-upstream，仅记录）
- Tier 2 (coverage gaps):
  - T2.1 IqRecorder 丢块路径（队列满丢最旧+计数）无单测
  - T2.2 detect_bursts 全饱和/全静默边界无单测
  - T2.3 esb_scan 大输入性能无基准（2M 比特 O(n·30)）
  - T2.4 LiveBursts 环回绕（written ≥ ring）后 refresh/read_slice 无测试（e2e 只测了未回绕）
- Tier 3 (module rotation): src/app/main.cpp（UI 组装，1200+ 行最大）→ src/radio/{hackrf,iq_recorder} → src/dsp/{esb,gfsk,burst_detector,live_bursts,analyzer,channel_monitor,panorama,waterfall,fft} → src/ui/{views,monitor_view} → tools/{gfsk_analyze,iq_capture,hackrf_smoke}
- Tier 4 (backlog, 单轮可完成):
  - T4.1 IQ 录制写参数 sidecar（.cs8 同名 .txt：频率/采样率/增益/时间）
  - T4.2 gfsk_analyze 支持 --csv 输出突发表
  - T4.3 瀑布对比度拉伸（M1 已知：[-100,-40] 映射下蓝档永不触发）
  - T4.4 README 运行指引统一为 Release 路径
  - T4.5 采样率低档 2/4 Msps（窄带驻留更久）

## Rounds

