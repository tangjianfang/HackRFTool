# M3 抓包试验（F3）· 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** v0.3（增强）——IQ 录制到 .cs8 → 突发（burst）检测 → 离线 GFSK 解调出比特流，全部控制台工具化；真机实测环境 2.4G 突发。按 README F3「先离线处理」原则，UI 集成留待实时化阶段。

**Architecture:** 三个纯 DSP/IO 模块（GFSK 解调器、突发检测器、IQ 录制器，全部 TDD）+ 两个控制台工具（`iq_capture` 采集、`gfsk_analyze` 分析）。解调正确性用**合成 GFSK 往返（BER=0）**证明；真机验证只要求管线在真实射频数据上端到端出结果。

**Tech Stack:** 同 M1/M2。无新增依赖。

## Global Constraints

- 沿用 M1/M2 全部约束（/W4 零告警、每任务一提交、中文、NOMINMAX 自卫）
- 参数约定：默认 fs=20 Msps、symrate=1 Mbps（sps=20）、GFSK 频偏 160 kHz、BT=0.5；sps 必须为整数（20/10 都满足，1 Mbps/2 Mbps 均可）
- dBFS 口径与 analyzer 一致（幅度 127 = 0 dB）
- 验证识别次数预算：**0**（控制台输出即证据）

---

### Task 1: GFSK 解调器（TDD）

**Files:** Create `src/dsp/gfsk.{hpp,cpp}`；Modify `tests/dsp_test.cpp`、`CMakeLists.txt`

**Interfaces (Produces):**
- `struct GfskResult { std::vector<std::uint8_t> bits; std::vector<float> quality; };`
- `class GfskDemod { public: GfskDemod(double sample_rate, double symbol_rate, double deviation_hz); GfskResult demod(const std::vector<std::complex<float>>& iq, std::size_t start_offset) const; };`
- 解调法：瞬时频率 `arg(z[k]·conj(z[k-1]))` → 按符号窗口（sps 整数）求均值 → 符号 = 频率符号；quality = |f_hz|/deviation

**测试（测试文件内自带合成调制器）：**

```cpp
// 高斯成形 GFSK 调制（测试助手）：NRZ → BT=0.5 高斯 FIR → 相位积分
static std::vector<std::complex<float>> gfsk_modulate(const std::vector<uint8_t>& bits,
                                                      double sps, double dev_hz,
                                                      double fs) {
    const double bt = 0.5, t_sym = 1.0;
    // 高斯 FIR：span ±2 符号，归一化面积 1
    std::vector<double> h;
    const double alpha = std::numbers::sqrt(std::numbers::ln2) / (2.0 * std::numbers::pi) / bt;
    double hsum = 0.0;
    for (int k = -8; k <= 8; ++k) {   // 0.4 符号步进的连续脉冲，按符号窗积分更简：
        ...
    }
    // 简化实现：频率脉冲 = 每符号一拍的狄拉克经高斯核平滑；实现时用 5 抽头/符号核
    ...
}
```

（实现提示：脉冲成形用「每符号 5 抽头高斯核」对 NRZ 上采样序列卷积；测试只要求无噪往返 BER=0（跳过首尾各 2 符号）。）

**核心测试用例：**
```cpp
static void test_gfsk_roundtrip() {
    // 200 随机比特，首尾各加 4 个哑符号
    const std::size_t payload = 200;
    std::vector<uint8_t> bits(4, 0);
    unsigned lcg = 7u;
    for (std::size_t i = 0; i < payload; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        bits.push_back((lcg >> 31) & 1u);
    }
    bits.insert(bits.end(), 4, 0);
    const auto wave = gfsk_modulate(bits, 20.0, 160e3, 20e6);
    hackrftool::dsp::GfskDemod demod(20e6, 1e6, 160e3);
    const auto r = demod.demod(wave, 0);
    std::size_t errors = 0;
    for (std::size_t i = 4; i < 4 + payload; ++i)
        errors += (r.bits[i] != bits[i]);
    check(errors == 0, "GFSK 无噪往返 BER=0");
    check(r.bits.size() == bits.size(), "符号数守恒");
}
```

红灯→实现→绿灯→Commit `M3 DSP：GFSK 解调器（合成往返 BER=0）`

### Task 2: 突发检测器（TDD）

**Files:** Create `src/dsp/burst_detector.{hpp,cpp}`；Modify tests/CMake

**Interfaces:** `struct RfBurst { std::size_t start_sample; std::size_t length_samples; float peak_db; };`
`std::vector<RfBurst> detect_bursts(const std::int8_t* iq, std::size_t byte_count, double sample_rate, float threshold_db, std::size_t window_samples);`
（滑窗能量 dBFS → 过阈值连通段 → 间隙 < 2×window 合并；start 修正当窗口半宽）

**测试：** 静噪（LCG 幅度 3 ≈ -70dB）中放 3 段复单音突发（幅度 100，长度 2000 样本，起点 5000/30000/60000），window=200，threshold=-40 → 恰好 3 个 burst，start±300、length±600 容差，peak > -5 dB。

Commit `M3 DSP：滑窗突发检测器`

### Task 3: IQ 录制器 + iq_capture 控制台（TDD 文件层）

**Files:** Create `src/radio/iq_recorder.{hpp,cpp}`、`tools/iq_capture.cpp`；Modify CMake

**Interfaces:**
- `class IqRecorder { ~IqRecorder(); bool start(const std::wstring& path); void write(const std::int8_t*, std::size_t); bool stop(); bool recording() const; uint64_t bytes_written() const; uint64_t dropped_blocks() const; }`
  （USB 回调线程只入队（有界 64 块，满则丢最旧并计数），独立写线程 fwrite；stop 冲刷合并）
- 控制台 `HackRFCapture <out.cs8> <seconds>`：HackRadio(2450M/20Msps/LNA32/VGA30) → 回调 `recorder.write` → 到时 stop → 打印字节/块/丢弃
- **测试：** start → write 三段 0xAA/0xBB/0xCC 各 1000 字节 → stop → 文件 3000 字节且顺序正确

Commit `M3 工具：IQ 录制器与 iq_capture 控制台采集`

### Task 4: gfsk_analyze 控制台

**Files:** Create `tools/gfsk_analyze.cpp`；Modify CMake

**用法：** `HackRFAnalyze <file.cs8> [threshold_db=-40] [fs_hz=20e6] [sym_rate=1e6]`
**输出：** 文件样本数/时长；burst 表（#、start_us、len_us、peak_db）；每 burst GFSK 解调比特数与前 16 字节 hex（MSB first 打包）；汇总。

Commit `M3 工具：gfsk_analyze 离线分析`

### Task 5: 真机实验与收尾

- `hackrf_spiflash -R` → `HackRFCapture cap-2450.cs8 5` → `HackRFAnalyze cap-2450.cs8 -40 20e6 1e6`
- 验收：捕获 ≥100 MB；burst 表非空（环境 WiFi/BT 活动）；无崩溃。把控制台输出贴入交付记录
- 交付记录 `docs/m3-交付记录.md`（含「如何抓自家鼠标键盘」操作指引：录制时动鼠标 → 分析对应突发）
- README 状态更新 + 全量 ctest + commit + push

## Self-Review

- Spec 覆盖（README F3）：固定信道 IQ 捕获 ✓（iq_capture）；GFSK 解调 ✓（TDD BER=0）；包边界识别 ✓（burst 检测，"包"以能量突发近似，协议级解 framing 明示不在 M3）；原始数据落盘 ✓（.cs8 + bits/hex 报告）；先离线后实时 ✓（控制台即离线工作流）
- 占位符：调制器实现提示给了结构，具体抽头在实现时定（测试断言 BER=0 是硬验收）
- 类型一致性：RfBurst/GfskResult/IqRecorder 命名前后一致
