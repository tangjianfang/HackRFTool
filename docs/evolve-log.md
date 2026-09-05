# evolve log — HackRFTool

- verify: cmake --build --preset x64-release && ctest --preset x64-release   # 5 ctests（单测/真机自测×2/WinFlux×2，无设备自动 SKIP）+ 单测 228 断言
- pointer: #63（频谱 Y 轴范围可调——用户建议 3b 余项）
- rounds done: 17（#50–#62 已完成；本 run= #59–#78 共 20 轮）
- status: active
- metrics: findings 38 | fixes 40 | regressions 0（E4 按轮次行累加）
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
#54 | 用户功能：NOAA APT 卫星云图（三部曲 #2） | findings(5) | actions(1) | result(green+progress, 5 ctest/168 断言) | diff(+~600) | dsp/apt：2.4k 子载波 I/Q 同步检波（相位无关免锁相）→包络 AGC→1040Hz 正交行同步（峰沿+周期验证+不应期+236 样本群延迟补偿+近满行补齐 flush）→2080px/0.5s 行装配→1800 行滚动缓冲（fm 线程 feed/UI 线程 snapshot 互斥）；ui/apt_view 原生 GDI 窗（StretchDIBits 8bpp 灰度 DIB，云图页覆盖内容区——WinFlux 渲染器无位图接口）+ GDI+ PNG 保存；云图页上下文行：卫星预设（NOAA 19/15/18）/记录勾选（默认开）/保存 PNG；apt_on=云图页+接收+记录。调试发现（合成信号驱动）：①延迟相关判同步方向反——图像平坦区相关积(0.25)>同步串交替区(0.09)，改 1040Hz 正交检测；②合成器像素样本 floor 导致行长 23203≠24000 行永不满；③峰重置打断近满行→补齐 flush；④测试取锁定前行验证（假失败）；⑤sed 正则引入 img.rows-1*kWidth 优先级 bug。测试 160→168：6 行合成端到端（同步锁定/暗标记精确 min@600/亮标记钟形峰显著/梯度）+零行守卫。真机验证：autowx 云图页 GDI 视窗+占位文案+预设行正常；真实卫星过境待用户实测（NOAA 15 默认 137.620）
#55 | 用户功能：选择化交互——统一信号库/多条件筛选/随机收听/场景默认频率/频谱点击调谐（用户详细需求指令，Meteor 顺延 #56） | findings(0) | actions(1) | result(green+progress, 5 ctest/187 断言) | diff(+~700) | dsp/sigdb 纯函数：SignalEntry（频率/强度/在线）+ band_of 频段判定（FM 87.5-108/NOAA 137-138/2.4G ISM）+ filter_signals（类别×在线×强度/频率排序）+ random_pick（种子可测）+ merge_scan/mark_all_offline + TSV 持久化（signals.tsv 开机加载）；统一调谐 tune_to（自动频段分流+状态栏提示）；场景默认频率 apply_page_default（进页时中心不在该页频段→默认值：收音 98/云图 137.620/工具页 2450）；扫描泛化：类别选择（FM/NOAA/ISM）→信号库累积+落盘+电台段回最强台；收音机页重设计：筛选三下拉（类别/在线/排序）+信号库可点列表（行=频率+强度条+dB+在线点+频段名，点击即听，零手动输入）+频谱；频谱点击调谐（spectrum_view 增 SpectrumGeom paint 回写+on_click，main 侧 host.last_click_pos 换算；全景点击=退出扫描单窗跳转"专门收听该段"）；工具栏「随机收听」骰子按钮。测试 168→187（band_of/筛选/随机/合并/TSV 往返）。行为验证：跨进程 WM_GETTEXT 读频率框——点频谱 25%→97.48（97±1 窗 25% 数学吻合）、78%→98.06（新窗 78% 吻合）；ctest 5/5 含真机
#56 | 用户功能：人声信号强度检测波形（用户实收 107.1 人声清晰后提出；Meteor 顺延 #57） | findings(1) | actions(1) | result(green+progress, 5 ctest/199 断言) | diff(+~150) | dsp/fm 新 VoiceLevelMeter（300-3400Hz 话音频带 RBJ 带通双二阶级联——单节 8kHz 仅 -10dB 频翹翹尾，级联后 -28.6dB；块 RMS→dBFS）；fm_audio_cb 静噪后输出 feed，每 5 块（20Hz）入史 600 点=30s；ui 新 audio_level_strip（-60..0dBFS 刻度网格+accent 折线+“人声 x dB”标注）插 radio_display 头部下方；测试 187→199（1kHz@0.5→-9dB 精确/8kHz 带外低 19.6dB/静音第二块<-100dB——首块有上块状态尾巴假失败）。排障一次性踩中 L1：接线后构建中途失败跑陈旧 exe（二进制无“人声”字符串实锤）。真机验证：跨进程驱动调谐 107.1+AFC 关后截图——“人声 -35 dB”折线语音包络状起伏+STEREO+有台（L13：PS pinvoke 字符串参数假读假写三坑：string/StringBuilder marshalling 静默失败、AFC 周期写回覆盖外部写入、BM_CLICK 反复 toggle——数值消息可靠、截图为准绳）
#57 | 用户功能：界面参数全量持久化（启动恢复+变更即缓存）+ 收音机音频 FFT 频谱（Meteor 顺延 #58） | findings(2) | actions(2) | result(green+progress, 5 ctest/217 断言) | diff(+~600) | app/settings 纯函数：18 项参数（页签/双频率/采样率/增益/功放/AFC/带宽/音量/输出设备/筛选三下拉…）TSV 序列化，容错解析（坏行跳过、越界回退默认、strtod endptr 防 abc→0 误收）；保存=build 心跳 3s 节流+序列化比对变化才写盘+WM_DESTROY 兜底；恢复=控件创建后写字段+同步控件（combo 下标越界自愈+audio_dev 设备列表变化回退默认）+自动开始接收（打开即回到上次工作状态）；selftest 模式禁写（防污染用户参数，真机双自测跑完 settings 原样保留实测）+命令行模式禁读（断言依赖出厂默认）；ensure_fm 增益默认策略加 gains_pinned 门（不再覆盖恢复值，实测 LNA24 保留）。dsp/fm 新 AudioSpectrumMeter：2048 点汉宁实 FFT（48k 下 23.4Hz 分辨率）+慢衰减峰保持（-0.3dB/帧，19k 导频窄峰不闪烁）+计数器触发（回绕点在块中不漏算）；ui 新 audio_spectrum_strip（-90..-10dBFS、0-24kHz 刻度、19k 导频参考线、峰保持淡线+实时谱主线）；fm_audio_cb 取静噪前 (L+R)/2 喂入。测试 199→217：settings 往返 18 项/容错（垃圾行+越界+nan+endptr）/空文本；频谱整 bin 峰位 42/幅值 -12.04dBFS（numpy 校准）/19k 峰位 811/静音后峰保持衰减 <1dB。真机验证：键盘模拟调谐 107.1+AFC 关+LNA24→settings.tsv 落盘→重启零操作自动接收中（107.1/STEREO/音频频谱低频能量集中/人声波形有数据/LNA24 不被广播默认覆盖）。排障：外部改文本再踩 SetWindowText 假成功——终用驱动级 keybd_event 真键盘（tools/tune-kb.ps1，Ctrl+A 和弦四步序列是关键）；构建验证 grep 英文错误行漏检中文 VS locale（错误 C）跑了陈旧 exe 一次——必须看构建 exit code
#58 | 用户指令：全功能验收（工具栏/设置逐项+组合矩阵）→ 修复 B1-B5 + X 轴缩放 | findings(6) | actions(6) | result(green+progress, 5 ctest/217 断言) | diff(+~350) | 验收产物 docs/acceptance-report.md（功能脑图/选项规范性对照表/组合矩阵/问题清单）；修复：B1 收音链切采样率丢带宽（漏传 fm_bw 静默回 120k）、B2 全频段扫描锁采样率 20M（低档 5 段拼接频标错乱）、B3 全频段×收音页（开扫描停 FM 链+进页拒开，真机实测状态栏提示）、B4 收音页头部重构（信号灯绿/红+导频/峰值数值读数替代 24 格跳动条+立体声选项热切换 force_mono；设置行拆两行——行1 频率/带宽/音量/静音，行2 筛选/输出/微调/立体声，settings_rows() 纯函数 host_target/layout 共用）、B5 频谱页模式标题（单窗显示范围/全景标注扫描中——解用户"显示全频道是什么意思"疑问：即全频段扫描模式）；新增频谱 X 轴缩放（×1/2/4/8 segmented+左右平移+范围文本+刻度 0.2~10M 六档自适应，显示窗切片实现 spectrum_view 零改动）；stereo_opt/spec_zoom_idx 入设置持久化（20 项）；WM_DPICHANGED 字体列表补齐 radio 行（顺手修）。真机复测：两行布局全控件可见、×8=5MHz 窗 1M 刻度、B3 提示实测。待办移交 #59：信号库非模态弹窗（3a）、波形 Y 轴详细设置（3b 余）、云图接收链排查（B6 需卫星过境）
#59 | 用户指令：全量日志系统（UI/点击/事件/数据详细记录，日志推算分析问题；减少视觉识别——本轮起验收走日志断言，run=#59–#78 共 20 轮） | findings(2) | actions(1) | result(green+progress, 5 ctest/228 断言) | diff(+318) | src/app/telemetry：JSONL 结构化事件+环形缓冲（tail/count_event 自测断言）+文件 sink（1MB×3 轮转逐条 fflush）；埋点 on_command（code 白名单 0/1 滤 EN 刷屏）/on_hscroll/tune_to/toggle_rx 起停含失败 err/ensure_fm/reconfigure_rx/applyfreq/app 起停；测试 217→228（转义/序列化/环形/计数）；真机日志验收新范式：驱动序列→断言 app.start→rx.start→reconfig→fm.on→tune 事件链 ALL PASS 零截图。排障：kv 键 cat 与 JSON 顶层撞名（重复键覆盖）→band。视觉识别问题本 session 已实证（LNA 40 幻觉/波形数据幻觉两次）——用户指令正确
#60 | 数据面遥测：UI 状态变更快照+DSP 1Hz+断言工具（日志替代视觉 #2） | findings(0) | actions(1) | result(green+progress, 5 ctest/228 断言) | diff(+~120) | UI state 事件=状态串 diff 变化即记（9 字段）；DSP fm 1Hz（peak/pilot/voice/静噪/avg——信号质量时间序列）；selftest 报告附遥测行（events=3 实测）；tools/log-assert.py 断言工具；真机六类事件断言 ALL PASS（tail 验证 1Hz 节流正确）
#61 | APT 链路诊断遥测+scan/record/sweep 埋点（日志替代视觉 #3；云图 B6 排查基础） | findings(1) | actions(1) | result(green+progress, 5 ctest/228 断言) | diff(+~110) | AptDecoder 导出 subcarrier_level/sync_per_sec（过境判读三要素：sub/sync/lines）；APT diag 1Hz 实测待过境 sub=0.006-0.023 底噪/lines=2 噪声行——卫星来时日志可直接判链路哪段断；SCAN start/done+record+sweep 生命周期入日志。排障：埋点插在变量声明前（C2065 连锁 13 错——python replace 锚点选声明行之前）
#62 | 信号库非模态弹窗（用户建议 3a；日志替代视觉 #4） | findings(2) | actions(1) | result(green+progress, 5 ctest/228 断言) | diff(+~160) | 工具栏「信号库」按钮→原生非模态 ListView（双击=调谐，任意页可用）；库版本戳增量刷新防闪；SIGDB open/close/pick 入日志——日志驱动验收 ALL PASS 零截图。坑：filter_signals 返回下标集；匿名 ns 内定义与前向声明=两个函数（C2668）

## 运行总结（#1–#4，用户指令停止）

- 结果：4/4 green+progress，0 red/0 regression；断言基线 86→119（+33）；回放审计通过（#3/#4 父提交无新测试符号）
- 修复：T1.1 死代码、T1.2 报告落 exe 旁（检查员双确认）、T1.3 监测页扫描说明、T4.1 参数 sidecar、T4.3 瀑布色阶扩窗
- 池剩余：T2.3 esb 性能基准、T4.2 --csv、T4.5 低采样率档、Tier 3 模块轮转（src/app/main.cpp 最大未轮转）
- 经验库：docs/lessons.md 立 4 条（L1 陈旧二进制假绿 / L2 期望值用脚本算 / L3 并行会话碰撞 / L4 set-收尾铁律）

