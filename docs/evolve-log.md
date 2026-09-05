# evolve log — HackRFTool

- verify: cmake --build --preset x64-release && ctest --preset x64-release   # 5 ctests（单测/真机自测×2/WinFlux×2，无设备自动 SKIP）+ 单测 160 断言
- pointer: #54（用户功能三部曲：#53 收音机✓ → #54 NOAA APT 云图 → #55 Meteor QPSK）
- rounds done: 8（#50–#53 为用户指定轮号/主题，#5–#49 未运行）
- status: active
- metrics: findings 22 | fixes 25 | regressions 0（E4 按轮次行累加）
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
#52 | 用户指令级 epic：原生 Win32 骨架重构（#51 的 WinFlux 自绘重排被判"布局没变化"，三次下达后经计划模式确认方案） | findings(3) | actions(1) | result(green+progress, 5 ctest/145 断言) | diff(+910/-540) | 原生主窗（HackRFToolMain）+顶部工具栏（ToolbarWindow32：启停/页签×3 单选/全频段/录制/锁定/导出/清空/应用频率，GDI 自绘 24px 图标洋红键透明）+设置行原生控件（EDIT/COMBOBOX/TRACKBAR/CHECKBOX 随页显隐）+底部 msctls_statusbar32 六分段（status_parts 新纯函数，138→145 断言）；WinFlux Host 重父化为内容区子窗（保留 D2D/DComp 管线，图表不迁移——用户选定）；app.manifest（comctl32 v6）+ /MANIFEST:NO 防重复；flux::State 全退场（普通字段+每帧重建），set-收尾铁律随之退役；外层 GetMessage 泵替换 host.run()；selfclick 退役（L4 根因已修）。检查员 4/6：DPI 感知回归（enable_per_monitor_dpi_v2 误删→已修）+组合框下拉高度（MoveWindow 高度须含列表→已修 drop 标志）实锤修复；启动 WinFlux 顶层窗闪现一帧=wontfix-upstream（Host::create 硬编码 SW_SHOW，建议上游加 Config.visible）。验证：真机 ctest 5/5（builds=256/frames=1045 PASS）、三页前台截图+1500x950 拉伸截图全部正常；PrintWindow PW_RENDERFULLCONTENT 漏画普通子控件（L6，新增 screenshot-fg.ps1 BitBlt 前台抓图）。后续三个修复提交：拉伸闪烁三连修（63bb01c）、拖拽卡死看门狗+停顿解冻（c62f02a，L9 伪造消息不可投递）、鼠标划过工具栏黑底 TBSTYLE_FLAT+WM_PRINTCLIENT（6126191，L10）+selftest 被守卫拦截静默 42
#53 | 用户功能：收音机（立体声+自动扫台，三部曲 #1，计划模式批准） | findings(4) | actions(1) | result(green+progress, 5 ctest/160 断言) | diff(+649/-23) | dsp/fm 纯链路：FIR 抽取 fs→250k→正交鉴频→19k 导频 PLL（cos 型 PD 同相锁定）→38k DSBSC 立体声矩阵（sin 族同源，2θ 免 π 模糊）→50µs 去加重→250k→48k 线性重采样；audio/waveout（48k 立体声 4×100ms）；SPSC 4MB 环 + fm 专职线程（不挤采集/UI）；UI：页签+2（收音/云图，GDI 图标）、收音机页（频率大字+STEREO/MONO 徽章+音频电平表+信号 dB+FM 频谱）、扫台线程（87.5–108 步进 0.1 驻留 80ms 峰均差判台→预设下拉→调谐最强台）；kRatesMsps 加 2Msps 档（T4.5 关闭）+频谱窗宽/监测 bin 按实际采样率折算（原 20Msps 硬编码）+全频段强制 20Msps；合成测试 145→160（立体声分离 >20dB、单声道回退、抽取阻带 >20dB）。调试踩坑：重采样方向写反（×27 时间轴）、PLL PD −sinθ 锁正交点（L/R 恒互换）、合成器导频 sin/副载波 cos 违反同源惯例。真机验证：98MHz 实收广播（-32.9dB），音频听感待用户实测；云图页 #54 占位（卫星预设可调谐）

## 运行总结（#1–#4，用户指令停止）

- 结果：4/4 green+progress，0 red/0 regression；断言基线 86→119（+33）；回放审计通过（#3/#4 父提交无新测试符号）
- 修复：T1.1 死代码、T1.2 报告落 exe 旁（检查员双确认）、T1.3 监测页扫描说明、T4.1 参数 sidecar、T4.3 瀑布色阶扩窗
- 池剩余：T2.3 esb 性能基准、T4.2 --csv、T4.5 低采样率档、Tier 3 模块轮转（src/app/main.cpp 最大未轮转）
- 经验库：docs/lessons.md 立 4 条（L1 陈旧二进制假绿 / L2 期望值用脚本算 / L3 并行会话碰撞 / L4 set-收尾铁律）

