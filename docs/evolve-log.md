# evolve log — HackRFTool

- verify: cmake --build --preset x64-release && ctest --preset x64-release   # 5 ctests（单测/真机自测×2/WinFlux×2，无设备自动 SKIP）+ 单测 138 断言
- pointer: #52（N=50 运行进行中；上下文吃紧即在此checkpoint，新会话从 #52 续作）
- rounds done: 6（#50–#51 为用户指定轮号/主题，#5–#49 未运行）
- status: active（用户 N=50 主题运行：UI 性能与数据显示；主题耗尽或池清空按收敛规则提前终止）
- metrics: findings 16 | fixes 19 | regressions 0（E4 按轮次行累加）
- epics pending: none

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

#1 | T2.1/T2.2/T2.4 测试覆盖缺口 | findings(1) | actions(2) | result(green+progress, 5 ctest / 105 断言) | diff(+96) | 基线 86→105 断言：IqRecorder 契约边界/burst 极端输入/LiveBursts 环回绕；顺修 T1.1 死代码
#2 | T1.2+T1.3 | findings(2) | actions(2) | result(green+progress, 5 ctest) | diff(+28) | 报告落 exe 旁（exe_dir_path）；监测页扫描说明；独立检查员双确认（含真机 sweep 自测 PASS）
#3 | T4.1 sidecar | findings(0) | actions(1) | result(green+progress, 5 ctest / 112 断言) | diff(+78) | IQ 参数侧车 red→green；接线 iq_capture+UI 录制；105→112 断言
#4 | T4.3 色阶映射 | findings(0) | actions(1) | result(green+progress, 5 ctest / 119 断言) | diff(+52) | waterfall_level 纯函数+扩窗 [-110,-30]，深蓝档恢复可见；112→119 断言
#50 | 用户指定：UI 性能与数据显示（顶部工具栏/底部状态栏布局、丝滑拉伸、关键信号标识、解析数据实时显示；皮肤库引入被否——红线禁新三方依赖，纯 Win32 重写属 epic 级且无性能收益） | findings(6) | actions(6) | result(green+progress, 5 ctest/132 断言) | diff(+172/-15) | 瀑布 fill_rect 16384→503（3.1%，waterfall_runs 纯函数+覆盖等价断言）；频谱补 0/-50 dB 刻度；频谱页控件拆两行（窄窗不裁切）；ESB 关键信号横幅+行绿粗+清空全复位；解调预算 2/build（风暴不掉帧，预算行不写缓存后续补全）；hex_dump 纯函数。检查员 6/6 成立；重放审计 #1/#50 通过；真机自测单窗 builds=259 frames=1059 / 全频段 builds=331 frames=1373 均 PASS；会话压缩中断后以 git diff+todo 对账续作（L3 verified+1，新立 L5）
#51 | 用户主题（N=50 首轮）：统一顶栏/中间纯内容区/底部状态栏 | findings(7) | actions(7) | result(green+progress, 5 ctest/138 断言) | diff(+388/-262) | 布局契约落地：toolbar=页签+全局徽章（含 ESB N 帧任何页可见、●录制 NMB）+全局设备两行+页上下文行，三页改纯显示，状态栏独立成条；status_dynamic 抽纯函数 red→green（132→138 断言）；概览拆加粗计数+次级提示；删冗余「恢复自动」；瀑布空态占位；刻度底衬（检查员驳回频谱页绘制顺序：底衬画在波形前无效→已修：ylab 移波形后）。wontfix 记录：-100 刻度避让（#50 有据）、tabs/segmented 汉字间距与滑块手柄尺寸（上游组件）；LNK1104 残留进程锁 exe 被"构建零告警"前置检查拦住（L1 verified+1）

## 运行总结（#1–#4，用户指令停止）

- 结果：4/4 green+progress，0 red/0 regression；断言基线 86→119（+33）；回放审计通过（#3/#4 父提交无新测试符号）
- 修复：T1.1 死代码、T1.2 报告落 exe 旁（检查员双确认）、T1.3 监测页扫描说明、T4.1 参数 sidecar、T4.3 瀑布色阶扩窗
- 池剩余：T2.3 esb 性能基准、T4.2 --csv、T4.5 低采样率档、Tier 3 模块轮转（src/app/main.cpp 最大未轮转）
- 经验库：docs/lessons.md 立 4 条（L1 陈旧二进制假绿 / L2 期望值用脚本算 / L3 并行会话碰撞 / L4 set-收尾铁律）

