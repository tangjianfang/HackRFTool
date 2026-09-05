# Lessons — HackRFTool

Entry template: `| id | one-sentence lesson | how to apply (an executable action, not a slogan) | source | verified |`

- Update an existing entry on the same topic instead of adding a duplicate.
- Every retrospective re-checks entries: confirmed in action → `verified +1`; proved wrong or stale → rewrite or delete on the spot.
- ≥ 3 same-theme lessons with ≥ 5 total verifications → crystallize into a named SKILL.md mechanism or a standalone skill; mark the entries `crystallized → <where>`.

## 测试与验证

| id | lesson | how to apply | source | verified |
|---|---|---|---|---|
| L1 | 构建失败后 ctest 跑的是陈旧二进制，"全绿"是假象 | 每次先确认 `cmake --build` 输出零 error（`grep -cE 'warning\|error'` 为 0）再跑 ctest；宁可多跑一次空构建；grep 到 LNK1104（exe 被占用）先 Stop-Process 清残留 GUI 实例再编 | evolve#3/#4 连续两次踩中（字段名笔误后 ctest 仍绿）；evolve#51 LNK1104 被前置检查拦住 | 3 |
| L2 | 手算期望值易错（7.5 截断写成 6）——断言值用脚本算，不要心算 | 写数值断言前用一行 python/echo 核算；测试失败先验期望值再查实现 | evolve#4 waterfall_level 中点 | 1 |
| L6 | PrintWindow(PW_RENDERFULLCONTENT) 抓 DComp 窗口会漏画普通子控件（工具栏/状态栏在、EDIT/滑条不在）——截图"控件不见了"≠UI 真不见 | 验证原生控件用前台 BitBlt（tools/screenshot-fg.ps1）或 EnumChildWindows 列 rect/vis 取证，再下结论；PrintWindow 只用于纯 D2D 内容 | evolve#52 设置行"消失"误判全程 | 1 |
| L7 | 整文件重写最容易丢的是一行式初始化调用（#52 误删 enable_per_monitor_dpi_v2 → 整窗 DWM 模糊） | 重写后 diff 逐行核对被删除的调用清单（DPI/异常过滤器/单实例守卫这类 init 类单行者），对抗检查员必审 | evolve#52 检查员实锤（exe 内 0 处 dpiAware） | 1 |
| L8 | 原生骨架拉伸闪烁多源叠加：CS_HREDRAW/CS_VREDRAW 全窗失效、每步 WM_SIZE 带重画 MoveWindow 子控件、高频同文本 SB_SETTEXT、**DComp 子窗每次 WM_SIZE 销毁重建渲染表面（拖拽=持续闪白，上游行为）**；而 WS_EX_COMPOSITED 与 DComp 子窗冲突会把工具栏搞黑（MSDN 明令禁用） | 类样式不用 H/VREDRAW；**禁用 WS_EX_COMPOSITED（含 D3D/DComp 子窗时）**；WM_ERASEBKGND 返 1 + WM_PAINT 只填 ps.rcPaint；子控件/内容区 MoveWindow 用 bRepaint=FALSE；状态栏文本缓存；**DComp 内容区用"拖拽冻结+停顿解冻+几何看门狗"**（见 L9） | evolve#52 用户实测两轮闪烁+黑工具栏三连修 | 1 |
| L9 | 配对式系统消息不可依赖：WM_ENTERSIZEMOVE/EXITSIZEMOVE 在拖拽被打断时可能不成对（EXIT 丢失→UI 永久卡死冻结态，用户实锤）；且跨进程伪造该族消息**根本不会被投递**（SendMessage/PostMessage 均静默丢弃——与 WM_QUIT 同类内部消息），消息级仿真测不到冻结路径，曾导致误判"冻结失效" | 凡依赖 ENTER/EXIT 配对的机制必须配自愈：几何看门狗（每心跳核对内容区实际/目标矩形，漂移即 layout，一帧自愈）+ 停顿解冻（窗口尺寸停变 500ms 即解冻铺满——停顿所见即所得，连续拖动仍冻结防闪）；**验证必须注入故障**（外部 SetWindowPos 把子窗改小→看门狗恢复；真实 SendInput 拖拽中途停顿→截图看铺满），只测顺利路径等于没测 | evolve#52 用户截图实锤卡死态 + 伪造消息不可投递的双重发现 | 1 |
| L10 | TBSTYLE_FLAT 等透明样式子控件不画自身背景，重绘时向父窗口请求背景（WM_PRINTCLIENT）——父窗为反闪烁不擦背景时补画缺失=**黑底**；且热跟踪重绘只在鼠标划过时发生，静止截图永远测不到（"我验证过没问题"的盲区） | 透明样式控件改自绘（工具栏去 TBSTYLE_FLAT）；父窗仍处理 WM_PRINTCLIENT 填充 BTNFACE（双保险）；验证 UI 颜色必须"鼠标划过+像素采样"（dark%/light%），静止截图不作数 | evolve#52 用户实锤"鼠标移动工具栏变黑"，像素采样 86% dark 复现→修复后 1% |
| L11 | 媒体链路无声排查：先分段装计数桩（环进/环出/块产出/设备提交/错误码/播放位置）真机跑一遍，断点自明——禁止凭直觉改代码；`waveOutGetPosition` 请求 TIME_MS 可能被驱动改写为 TIME_SAMPLES（必须检查返回的 wType 再换算），否则 pos 恒 0 的**读取假象**会误判为"设备没在播" | 音频/数据管线问题一律先插桩取数再动手；MMTIME 读位置用 TIME_SAMPLES（wave 设备必支持）并按 wType 分支换算；waveOut 输出设备可枚举+可选（WAVE_MAPPER 哑端点场景），打不开要在状态栏报错而非静默 | evolve#55b 用户实锤"收音无声"：桩定位设备在播+重写回调式 WaveOut+修 pos 假象；enum_devices 实测 Realtek 唯一 | 1 |
| L12 | **HackRF 流中 hackrf_set_sample_rate 返回成功但数据流并不切换**（HackRF Pro 实测）：解调器按新率处理旧率数据=全白噪；"收到信号但解调全是噪音"还伴随扫出一堆假台（高增益下噪声包+邻频重复） | 运行中重配一律 `reconfigure_rx`（stop_rx→apply→start_rx），禁止流中 apply 改采样率；诊断三连（tools/HackRFRxCheck）：时域饱和统计/频谱峰/离线解调"1k-8k 能量落差"（>+10dB=有效广播解调，≈0dB=白噪）——纯客观判定替代人耳；DC 假设先写红测试再改（本次 DC 红测试证伪，省掉一次无效修复） | evolve#55c 用户实锤"全是噪音"：干净路径 +16.6dB vs 流中切换 -1.4dB 铁证→修复后 +15.6dB | 1 |
| L5 | PowerShell `Start-Process -ArgumentList` 传参会吞/改坏 GUI 程序的模式参数（三次截图全同、自动模式从未生效，状态栏仍显示"未开始"） | 自动化驱动 GUI 用直接 exec（bash 后台 / CTest）而非 Start-Process 包装；截图前先核对页内状态（页签高亮/状态栏文本）与预期模式一致再采 | evolve#50 复截图三次全同 | 1 |

## 进程与协作

| id | lesson | how to apply | source | verified |
|---|---|---|---|---|
| L3 | 并行会话在同一工作树会互相踩（增量构建竞速、文件被改、Edit 锚点失配） | 开工前 ListAgents 查邻居；发现文件被外部修改立即重读再改；构建"成功"但 mtime 未变即 touch 强制重链；会话压缩/中断恢复先 `git diff` + todo 对账在制工作，勿盲目重做或回滚 | 2026-09-05 与 evolve-07 会话碰撞全程；evolve#50 压缩恢复对账 | 2 |
| L13 | 跨进程 UI 驱动验证三坑：① PS pinvoke 字符串参数（string 入/字符串出）marshalling 静默失败——SetWindowText 返 True 但没写入、WM_GETTEXT 读回陈旧值；② 应用内周期写控件的逻辑（AFC 每 tick 写回频率框）会覆盖外部写入；③ BM_CLICK 是 toggle，不查 BM_GETCHECK 直接点会反向切换 | 跨进程只信数值消息（WM_COMMAND/BM_GETCHECK）；写控件前先关掉周期写回的自动逻辑（AFC 等）再写；状态判定以截图/二进制字符串为准绳（写盘前 grep 二进制确认新代码在产物里） | 2026-09-05 人声波形真机验证排障（调谐一度“不生效”實为假读假写+陈旧 exe 叠加） | 2 |

## WinFlux UI

| id | lesson | how to apply | source | verified |
|---|---|---|---|---|
| L4 | 点击处理器内 State::set() 之后访问 app = UAF（set 同步重建树释放正在执行的 lambda） | 副作用全部前置，set 永远最后一句；多步逻辑包自由函数（App& 走参数）——详见 memory/winflux-set-terminal-rule。**注：#52 骨架重构后 main.cpp 已无 flux::State（控件值=普通字段，每帧重建），本条仅对仍在用 WinFlux State 的代码（如 WinFlux 仓库/工具页）有效** | 2026-09-05 用户崩溃根因（crash-9472/5652 双实证） | 1 |
