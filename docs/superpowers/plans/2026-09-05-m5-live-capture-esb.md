# M5 实时抓包 UI + ESB 解帧 + 状态栏清理 · 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans（本会话内联执行）。Steps use checkbox (`- [ ]`) syntax.

**Goal:** v0.5——①「实时抓包」页：UI 内实时突发列表 + GFSK 比特预览 + 一键 IQ 录制；② 协议级解帧：nRF24 ESB（Enhanced ShockBurst）帧搜索器（合成样本 TDD），集成进 `gfsk_analyze --esb`；③ 状态栏文案随模式动态生成。

**Architecture:** `LiveBursts`（线程安全近期 IQ 环形 + UI 侧去重突发列表，复用 `detect_bursts`）；`esb_scan`（前导码 0xAA/0x55 × 地址长 3/4/5 × CRC 初值 0xFFFF/0x3D18 组合搜索，纯函数 TDD）；UI 第三页用 `scroll_view` + 文本行；`IqRecorder` 接 UI 按钮；状态文本在 build() 按模式拼接。

## Global Constraints

- 沿用全部既有约束；真机验证用 **Release**；识别预算 **1 次**（实时抓包页截图）
- LiveBursts 环 1M 复样本（~50ms @20Msps）；列表上限 500，显示最新 60；仅对长度 ≥128 样本的突发做 GFSK 预览（≤16 字节 hex，按 start_sample 缓存）
- ESB 约定：空口比特 LSB-first；前导码 1 字节 ∈ {0xAA,0x55}；地址 3-5 字节；PCF 9 位（低 6 位=载荷长）；载荷 0-32 字节；CRC16-CCITT poly 0x1021，初值尝试 {0xFFFF, 0x3D18}，比特 LSB-first 进位；仅输出 CRC 通过的帧
- 状态栏：`apply_radio` 只设「接收中」；build() 按模式拼「中心 X MHz」或「扫描 段 x/5 @ X MHz」+「帧 N」+「●录制中」

---

### Task 1: LiveBursts（TDD）
Create `src/dsp/live_bursts.{hpp,cpp}`；接口：
- `struct LiveBurst { unsigned long long start_sample, samples; float peak_db; };`
- `class LiveBursts { LiveBursts(size_t ring_complex=1'000'000); void write(const int8_t*, size_t bytes); size_t refresh(float threshold_db, size_t window=256); const std::vector<LiveBurst>& bursts() const; void clear(); unsigned long long total_samples() const; }`
- write：USB 线程，锁内拷贝环形；refresh：UI 线程，锁内整环快照 → detect_bursts → 只追加 `start_sample > 既有最大` 的突发（去重），`detected_upto_ = written_ - window`
- 测试：write 三突发 + 静默 → refresh 得 3；再写静默+1 突发 → refresh 只新增 1；clear 复位

### Task 2: ESB 帧搜索器（TDD）
Create `src/dsp/esb.{hpp,cpp}`；接口：
- `struct EsbFrame { std::vector<uint8_t> address; std::vector<uint8_t> payload; std::size_t bit_offset; };`
- `std::vector<EsbFrame> esb_scan(const std::vector<uint8_t>& bits)`
- 测试文件内合成器 `esb_pack(addr, payload) -> bits`（前导 0xAA + addr + PCF(9bit, len) + payload + CRC(init 0xFFFF)），断言 scan 还原 address/payload；翻转 1 个载荷比特后 scan 不产出该帧
- Commit

### Task 3: 实时抓包页 + IQ 录制按钮 + 状态栏清理（UI）
Modify `src/app/main.cpp`、`CMakeLists.txt`：
- App：`dsp::LiveBursts live;`、`radio::IqRecorder recorder;`、`State<int> page` 第三页、`State<double> burst_thr(-40)`、`std::map<unsigned long long, std::wstring> hex_cache;`
- `rx_trampoline_ui`：追加 `live.write(iq, bytes)`
- build()：running 时 `live.refresh(burst_thr)`；rows = 最新 60 条倒序，`scroll_view` 文本行（`#N t=123.456s len=3.31ms -32.4dB | A1B2…`）
- 录制按钮：GetSaveFileNameW → `recorder.start/stop`，回调里 `recorder.write`；状态/徽章「●录制中」
- 状态栏：apply_radio 改设 L"接收中"；build 拼 `接收中 · 中心 X / 扫描 段 x/5 @ X · 帧 N · ●录制中`
- Commit

### Task 4: gfsk_analyze 集成 ESB
Modify `tools/gfsk_analyze.cpp`：加 `--esb` 第 5 参或参数扫描；对每突发 demod 后 `esb_scan`，打印帧（addr/len/payload hex）；无参数说明更新
- Commit

### Task 5: 真机验证 + 交付
- Release 构建 + ctest；`HackRFTool.exe autocap`（新参数：接收+开实时抓包页）跑 8s → 激活截图 → haiku×1（列表有真实突发行、阈值滑杆、录制按钮、状态栏新文案）
- `docs/m5-交付记录.md` + README 状态 + push
