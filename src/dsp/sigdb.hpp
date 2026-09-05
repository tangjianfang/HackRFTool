// 统一信号库（#55）：扫描结果条目 / 频段判定 / 多条件筛选 / 随机挑选 /
// 持久化。纯 C++（文件 IO 用宽字符路径），dsp_test 直测。
#pragma once

#include <cstddef>
#include <vector>

namespace hackrftool::dsp {

// 一条信号记录：频率 + 最近扫描强度 + 在线（最近一次扫描命中）
struct SignalEntry {
    double mhz = 0.0;
    float db = -120.0f;
    bool online = false;
};

enum class SigCat { all, radio, sat, ism, other };

struct BandInfo {
    const wchar_t* name;
    SigCat cat;
    double lo_mhz;
    double hi_mhz;
};

// 频段判定：FM 广播 87.5–108 / NOAA 气象卫星 137–138 / 2.4G ISM
// 2400–2483.5 / 其余归 other
[[nodiscard]] BandInfo band_of(double mhz) noexcept;

// 场景默认中心频率：进入页面时若当前中心不在该页频段内则落到默认值
[[nodiscard]] double default_center(SigCat cat) noexcept;   // radio=98, sat=137.62, ism=2450
[[nodiscard]] bool in_band(SigCat cat, double mhz) noexcept;

// 扫描合并：同频（±容差 Hz 换算 MHz）更新强度并置在线，否则新增
void merge_scan(std::vector<SignalEntry>& list, double mhz, float db,
                double tol_mhz = 0.05);
// 全部离线（新扫描开始前调用，保留条目但标记待验证）
void mark_all_offline(std::vector<SignalEntry>& list) noexcept;

// 多条件筛选：类别 + 在线 + 排序（强度降序或频率升序）→ 原表下标
[[nodiscard]] std::vector<std::size_t> filter_signals(
    const std::vector<SignalEntry>& list, SigCat cat, bool online_only,
    bool by_strength) noexcept;

// 随机挑选（在线优先；seed 注入可测试）：无合适条目返回 -1
[[nodiscard]] long random_pick(const std::vector<SignalEntry>& list, SigCat cat,
                               unsigned seed) noexcept;

// 持久化：TSV "mhz\tdb\tonline"，按频率升序
[[nodiscard]] bool save_signals(const wchar_t* path,
                                const std::vector<SignalEntry>& list) noexcept;
[[nodiscard]] std::vector<SignalEntry> load_signals(const wchar_t* path) noexcept;

} // namespace hackrftool::dsp
