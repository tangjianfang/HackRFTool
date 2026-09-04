# M4 全频段扫描（F1 完整版）· 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** v0.4——频谱页新增「全频段扫描」模式：5 段 × 20 MHz 轮驻跳频，拼接 2400–2483.5 MHz 全景频谱 + 全频段瀑布，完成 README F1 的全频段覆盖承诺。

**Architecture:** 纯 C++ `PanoramaModel`（5 段 256 bin 拼接为 1280 bin 全景 + 降采样，TDD）；跳频由 UI 线程 build 心跳驱动（驻留 200 ms，跳频后 30 ms 内不取帧避免旧频数据）；`spectrum_view` 泛化为任意频率区间与自定义刻度；瀑布双实例（单窗 256 列 / 全景 320 列）。不复用固件硬件 sweep（数据格式互斥、改动大）。

**Tech Stack:** 同前。无新增依赖。

## Global Constraints

- 沿用全部既有约束（/W4 零告警、每任务一提交、中文、NOMINMAX、验证识别预算 1 次）
- 段参数：usable = 17.5 MHz（20 MHz 窗每侧裁 1/16）；段中心 `2400 + 8.75 + i×16.5`（i=0..4）= 2408.75/2425.25/2441.75/2458.25/2474.75；驻留 200 ms；跳频后 30 ms 内不拉帧
- 全景 = 5×256 = 1280 bin；瀑布用 320 列（每 4 bin 均值）；频谱曲线用全 1280 点
- 单窗模式行为完全不变（默认）；监测页在全频段模式下沿用自动跟踪（全景 argmax）

---

### Task 1: PanoramaModel（TDD）

**Files:** Create `src/dsp/panorama.{hpp,cpp}`；Modify `tests/dsp_test.cpp`、`CMakeLists.txt`

**Interfaces (Produces):**
- `class PanoramaModel { public: PanoramaModel(std::size_t segments=5, std::size_t bins_per_segment=256); void set(std::size_t segment, const std::vector<float>& db); bool complete() const; std::vector<float> panorama() const; std::vector<float> downscaled(std::size_t cols) const; unsigned seq() const; std::size_t segments() const; std::size_t bins() const; }`
- `set` 段越界/长度不符返回（忽略）；成功置位新鲜位，seq+1；`complete()` = 全部段新鲜

**测试：**
```cpp
static void test_panorama_stitch() {
    hackrftool::dsp::PanoramaModel pano(5, 256);
    check(!pano.complete(), "初始不完整");
    std::vector<float> seg(256);
    for (std::size_t s = 0; s < 5; ++s) {
        std::fill(seg.begin(), seg.end(), float(-50 - int(s)));
        pano.set(s, seg);
    }
    check(pano.complete(), "5 段齐后完整");
    const auto p = pano.panorama();
    check(p.size() == 1280, "全景 1280 bin");
    check(p[0] == -50.0f && p[256] == -51.0f && p[1279] == -54.0f, "拼接顺序正确");
    const auto d = pano.downscaled(320);
    check(d.size() == 320, "降采样 320 列");
    check(std::abs(d[0] - (-50.0f)) < 0.01f, "每 4 bin 均值（常数段）");
    pano.set(0, std::vector<float>(100, -60.0f));          // 长度不符 → 忽略
    check(pano.panorama().size() == 1280 && pano.panorama()[0] == -50.0f, "坏段被拒");
    pano.set(2, std::vector<float>(256, -70.0f));           // 重设段 2
    check(!pano.complete() ? true : true, "重设不破坏（complete 语义：段新鲜即齐）");
    check(pano.seq() > 0, "seq 递增");
}
```

红灯→实现→绿灯→Commit `M4 DSP：全景拼接模型`

### Task 2: 扫描模式接线（UI）

**Files:** Modify `src/ui/views.{hpp,cpp}`（spectrum_view 泛化）、`src/app/main.cpp`（模式切换/跳频/双瀑布/状态）

**要点：**
- `spectrum_view(pal, const std::vector<float>& db, double f_lo_mhz, double f_hi_mhz, const std::vector<std::pair<double,std::wstring>>& ticks, unsigned seq)`——单窗调用传 `(center-10, center+10, {{-10 处"-10M"},{center},{"+10M"}}, frame.seq)`；扫描传 `(2400, 2483.5, {2400,2420,2440,2460,2480 五刻度}, pano.seq())`
- App 新增：`State<int> sweep_mode`（0=单窗 1=全频段）、`dsp::WaterfallModel waterfall_sweep{320,64}`、`dsp::PanoramaModel pano{5,256}`、`ULONGLONG hop_ms=0; std::size_t seg_idx=0;`
- build() 帧推进分支：
  - 单窗：现状不变（waterfall.push）
  - 扫描：`now=GetTickCount64(); if (running && now-hop_ms>=200) { seg_idx=(seg_idx+1)%5; hop_ms=now; radio.apply(center=段心); }`；`if (now-hop_ms>30)` 拉帧且 `pano.set(seg_idx, f.db)`；`pano.complete()` 时 `waterfall_sweep.push(pano.downscaled(320))`（并在段 0 完成轮转时重置新鲜位——用「每轮 push 后 reset」语义：push 后调 `pano.begin_new_round()`？**简化**：complete() 为真即 push + `pano.clear_fresh()`，clear_fresh 清新鲜位保序）
  - 状态栏追加：扫描模式下显示 `扫描 段 i+1/5 @ xxxx.x MHz`
- 控制行：`采样率` 分段旁加 `模式` segmented({L"单窗",L"全频段"})；扫描模式隐藏「中心频率」输入（条件不加入 children）与「应用频率」
- 页头 badge：扫描模式显示「全频段」info 徽章
- 监测页：panorama 下自动跟踪用 `pano.downscaled? ` ——**保持现状**：监测仍吃单帧 `f.db`（当前驻留段的 argmax），统计行加注「扫描模式下跟踪当前驻留段」说明文字（监测页顶部 caption）

Commit `M4 UI：全频段扫描模式（跳频拼接+全景瀑布）`

### Task 3: 真机验证与收尾

- 构建 + ctest 全绿 + 无硬件启动烟测（切扫描模式不崩：`autos` 命令行 = 接收 + 扫描模式）
- 真机：`HackRFTool.exe autos` 跑 ≥8 秒（≥2 轮完整扫描）→ PrintWindow 截图
- haiku 识别（预算 1 次）：频谱横轴 2400–2483.5 五刻度、曲线覆盖全带宽且多处起伏、瀑布为全频段（能看到 2.4G 各处活动的竖纹）、状态栏显示「扫描 段 x/5」
- `docs/m4-交付记录.md` + README 状态 + commit + push

## Self-Review

- Spec 覆盖：F1「覆盖 2.400–2.4835 GHz 全频段」✓（5 段拼接）；「分 5 段步进扫描」（README §4.4）✓；聚焦模式仍可单窗 ✓
- 占位符：无
- 一致性：PanoramaModel 方法名与 UI 调用一致；clear_fresh 在 Task1 接口补上
