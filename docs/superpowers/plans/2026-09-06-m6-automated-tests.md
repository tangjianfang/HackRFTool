# M6 自动化测试体系 · 实现计划

> **For agentic workers:** superpowers:executing-plans（本会话内联执行）。Steps use checkbox (`- [ ]`) syntax.

**Goal:** v0.6——验证方式自动化：①端到端合成管线测试（IQ→频谱→突发→GFSK→ESB 全链路断言，无硬件无图像）；②`selftest` 自测模式（指标+断言+报告+退出码，CTest 注册、无设备 SKIP）；③2 Mbps 解调开关；④锁定 bin 随中心频率重算。

## Global Constraints

- 沿用全部既有约束；**本里程碑识别预算 0 次**（自动化即目的）
- selftest 断言分级：硬（builds≥10 / frames≥50 / 报告落盘）失败退出码 1；软（bursts、wf 轮次、ESB 命中）仅记录；无设备 → 报告写 SKIP、退出码 42（CTest `SKIP_RETURN_CODE 42`）
- 合成端到端用例复用测试内既有的 `gfsk_modulate` 与 `esb_pack` 助手

---

### Task 1: 端到端合成管线测试（TDD）
`tests/dsp_test.cpp` 新增 `test_end_to_end_pipeline()`：
1. 构造 1 个 ESB 包（addr 5B + payload 6B）→ `esb_pack` → 比特 → `gfsk_modulate(bits, 20, 160e3, 20e6)` → 复浮点波
2. 波形前后垫静默、转 int8 交错：
   - `SpectrumAnalyzer(512,64,256).feed` → 峰位/峰电平在期望 bin±2
   - `LiveBursts(50000).write` ×2 → `refresh(-30,200)` 恰好 1 突发；`read_slice` 取回
   - 切片转复浮点 → `GfskDemod(20e6,1e6,160e3).demod` → `esb_scan` → 还原 addr/payload（跳过首尾哑符号影响：包外已垫静默，ESB 扫描自带容错）
3. 全绿后 Commit `M6 测试：端到端合成管线（IQ→ESB 还原）断言`

### Task 2: GfskDemod 2 Mbps + UI 开关
- 测试：`test_gfsk_roundtrip_2m`（sps=10 往返 BER=0，跳过首尾 2 符号）
- UI：抓包页加 `符号率` segmented {L"1M", L"2M"} → `State<int> symrate_idx`；`burst_row_text` 用 `symrate_idx ? 2e6 : 1e6`
- Commit

### Task 3: 锁定 bin 随中心频率重算（M2 修复）
- App 增 `double mon_lock_mhz = 0;`；「锁定该频率」记录之；「应用频率」后若 fixed_bin 且 mon_lock_mhz>0 → `set_fixed_bin(mhz_to_bin(mon_lock_mhz, 新中心))`
- Commit

### Task 4: selftest 自测模式
`src/app/main.cpp`：
- App 增 `std::atomic<unsigned> build_count{0}; std::atomic<unsigned> esb_hits{0};`（burst_row_text 命中 ESB 时 +1；build() 里 ++build_count——常驻非临时）
- `wWinMain` 解析 `selftest`（单窗 6s）/ `selftestsweep`（全频段 8s）：auto_start 接收后记录 `ui_thread_id = GetCurrentThreadId()`；起看门狗线程 sleep N 秒 → 汇总指标写 `selftest-report.txt`（硬/软断言分行）→ `PostThreadMessage(ui_tid, WM_QUIT, 0, 0)`
- `host.run()` 返回后：硬断言全过 → 返回 0；设备 open 失败 → 报告 SKIP → 返回 42；否则 1
- CMake：`add_test(NAME HackRFSelfTestSingle COMMAND HackRFTool selftest)` + `PROPERTIES SKIP_RETURN_CODE 42`；`HackRFSelfTestSweep` 同理
- Commit

### Task 5: 真机运行自动化验证 + 交付
- `ctest -R HackRFToolTest`（合成）→ 真机 `./HackRFTool.exe selftest; echo $?` 与 `selftestsweep` → `selftest-report.txt` 内容入交付记录
- `docs/m6-交付记录.md` + README + push
