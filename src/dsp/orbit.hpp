// 卫星过境预测（#82）：TLE 解析 + J2 平均根数传播 + 观测点过境几何。
// 精度定位：LEO 短期（TLE 龄期数天内）过境时刻 ±1 分钟级——对收图
// 调度足够；非精密星历。纯 C++ 无网络/UI 依赖（TLE 文本外部供给）。
#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hackrftool::dsp {

// 观测点（深圳：22.5431N 114.0579E 由调用方传）
struct GroundSite {
    double lat_deg = 0.0;
    double lon_deg = 0.0;
    double alt_km = 0.0;
};

// TLE 两行根数（解析后）
struct TleElements {
    std::string name;
    double epoch_jd = 0.0;      // 儒略日（UTC）
    double incl_deg = 0.0;      // 轨道倾角
    double raan_deg = 0.0;      // 升交点赤经
    double ecc = 0.0;           // 偏心率
    double argp_deg = 0.0;      // 近地点幅角
    double mean_anom_deg = 0.0; // 平近点角（历元）
    double mean_motion = 0.0;   // 圈/天
    double bstar = 0.0;         // 阻力项（简化模型忽略，龄期补偿靠更新）

    [[nodiscard]] bool valid() const noexcept {
        return mean_motion > 0.5 && mean_motion < 20.0 && ecc >= 0.0 &&
               ecc < 0.1 && incl_deg > 0.0 && incl_deg < 180.0;
    }
};

// 一条 TLE（三行：名称+L1+L2）解析；失败返回 valid()==false
[[nodiscard]] TleElements parse_tle(const std::string& name,
                                    const std::string& line1,
                                    const std::string& line2);

// 传播：TLE 历元 + dt 天 → ECI 位置（km）/速度不必需
[[nodiscard]] std::array<double, 3> propagate_eci(const TleElements& tle,
                                                  double dt_days);

// 儒略日 ↔ Unix 秒（UTC）
[[nodiscard]] double unix_to_jd(std::int64_t unix_sec) noexcept;
[[nodiscard]] std::int64_t jd_to_unix(double jd) noexcept;
[[nodiscard]] double now_jd() noexcept;   // 系统时钟

// 过境事件：仰角越过门限的窗口
struct PassEvent {
    std::int64_t start_unix = 0;   // 仰角升过门限
    std::int64_t peak_unix = 0;    // 最大仰角时刻
    std::int64_t end_unix = 0;     // 降过门限
    double peak_elev_deg = 0.0;
};

// 未来 window_hours 内（从 t0_unix 起）仰角 ≥ min_elev 的过境列表，
// 按时间升序。步进 30s 搜索 + 抛物线插值细化峰。
[[nodiscard]] std::vector<PassEvent> predict_passes(const TleElements& tle,
                                                    const GroundSite& site,
                                                    std::int64_t t0_unix,
                                                    double window_hours,
                                                    double min_elev_deg);

} // namespace hackrftool::dsp
