#include "dsp/orbit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace hackrftool::dsp {

namespace {
constexpr double kReKm = 6378.137;        // WGS84 长半轴
constexpr double kMu = 398600.4418;       // km³/s²
constexpr double kJ2 = 1.08262668e-3;
constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] inline double rad(double d) { return d * kPi / 180.0; }
[[nodiscard]] inline double deg(double r) { return r * 180.0 / kPi; }

// 平近点角 → 偏近点角（牛顿迭代）；偏心率小时 2-3 步收敛
[[nodiscard]] double mean_to_eccentric(double m_rad, double e) {
    double e0 = m_rad;
    for (int i = 0; i < 8; ++i) {
        const double f = e0 - e * std::sin(e0) - m_rad;
        const double fp = 1.0 - e * std::cos(e0);
        const double de = f / fp;
        e0 -= de;
        if (std::abs(de) < 1e-12) break;
    }
    return e0;
}
} // namespace

double unix_to_jd(std::int64_t unix_sec) noexcept {
    return 2440587.5 + double(unix_sec) / 86400.0;
}

std::int64_t jd_to_unix(double jd) noexcept {
    return std::int64_t((jd - 2440587.5) * 86400.0);
}

double now_jd() noexcept {
    return unix_to_jd(std::int64_t(std::time(nullptr)));
}

TleElements parse_tle(const std::string& name, const std::string& l1,
                      const std::string& l2) {
    TleElements t;
    t.name = name;
    if (l1.size() < 63 || l2.size() < 63) return t;
    const auto fld = [](const std::string& s, std::size_t from,
                        std::size_t n) -> double {
        return std::atof(s.substr(from, n).c_str());
    };
    // 行1：历元 YYDDD.DDDDDDDD（列 19-32）
    const double ep = fld(l1, 18, 14);
    const int yy = int(ep / 1000.0);
    const double doy = ep - double(yy) * 1000.0;
    const int year = yy < 57 ? 2000 + yy : 1900 + yy;
    // 年首儒略日（简化格里历）
    const int y1 = year - 1;
    const double jd0 = 1721424.5 + 365.0 * y1 + std::floor(y1 / 4.0) -
                       std::floor(y1 / 100.0) + std::floor(y1 / 400.0);
    t.epoch_jd = jd0 + doy;
    // 阻力（指数格式列 54-61，形如 " 12345-6"）
    {
        const std::string bs = l1.substr(53, 8);
        if (bs.find('-') != std::string::npos && bs.size() >= 8) {
            const double mant = std::atof(("0." + bs.substr(0, 6)).c_str());
            const double expo = std::atof(bs.substr(7, 1).c_str());
            t.bstar = mant * std::pow(10.0, expo);
        }
    }
    t.incl_deg = fld(l2, 8, 8);
    t.raan_deg = fld(l2, 17, 8);
    t.ecc = std::atof(("0." + l2.substr(26, 7)).c_str());
    t.argp_deg = fld(l2, 34, 8);
    t.mean_anom_deg = fld(l2, 43, 8);
    t.mean_motion = fld(l2, 52, 11);
    return t;
}

std::array<double, 3> propagate_eci(const TleElements& tle, double dt_days) {
    const double n0 = tle.mean_motion * 2.0 * kPi / 86400.0;   // rad/s
    // J2 长期项（平均根数）：RAAN 与 argp 漂移 + 平近点角进动
    const double p = std::pow(kMu / (n0 * n0), 1.0 / 3.0) *
                     (1.0 - tle.ecc * tle.ecc);   // 半通径 km
    const double f = 1.5 * kJ2 * (kReKm / p) * (kReKm / p) * n0;
    const double ci = std::cos(rad(tle.incl_deg));
    const double raan = rad(tle.raan_deg) - f * ci * dt_days * 86400.0;
    const double argp =
        rad(tle.argp_deg) + 0.5 * f * (5.0 * ci * ci - 1.0) * dt_days * 86400.0;
    const double m = rad(tle.mean_anom_deg) +
                     n0 * dt_days * 86400.0 *
                         (1.0 + 3.0 * kJ2 * (kReKm / p) * (kReKm / p) *
                                    std::sqrt(1.0 - tle.ecc * tle.ecc) *
                                    (1.0 - 1.5 * std::sin(rad(tle.incl_deg)) *
                                               std::sin(rad(tle.incl_deg))));
    const double e = tle.ecc;
    const double ea = mean_to_eccentric(std::fmod(m + 10 * kPi, 2 * kPi) - kPi, e);
    const double a = std::pow(kMu / (n0 * n0), 1.0 / 3.0);
    // 轨道平面坐标（近地点系）
    const double ca = std::cos(ea), sa = std::sin(ea);
    const double xo = a * (ca - e);
    const double yo = a * std::sqrt(1.0 - e * e) * sa;
    // 升交点系（argp）
    const double cw = std::cos(argp), sw = std::sin(argp);
    const double x1 = xo * cw - yo * sw;
    const double y1 = xo * sw + yo * cw;
    // ECI = Rz(raan) · Rx(incl) · 轨道面（argp 已并入 x1/y1）
    const double cr = std::cos(raan), sr = std::sin(raan);
    const double cinc = std::cos(rad(tle.incl_deg));
    const double sinc = std::sin(rad(tle.incl_deg));
    return {x1 * cr - y1 * sr * cinc,
            x1 * sr + y1 * cr * cinc,
            y1 * sinc};
}

SubPoint sat_subpoint(const TleElements& tle, std::int64_t unix_sec) noexcept {
    SubPoint sp;
    if (!tle.valid()) return sp;
    const double jd = unix_to_jd(unix_sec);
    const auto r = propagate_eci(tle, jd - tle.epoch_jd);
    const double d = jd - 2451545.0;
    const double gmst = rad(280.46061837 + 360.98564736629 * d);
    const double lon_orb = std::atan2(r[1], r[0]) - gmst;
    double lon = deg(lon_orb);
    lon = std::fmod(lon + 540.0, 360.0) - 180.0;   // 归一 ±180
    const double rxy = std::sqrt(r[0] * r[0] + r[1] * r[1]);
    const double gclat = std::atan2(r[2], rxy);    // 地心纬度
    const double rmag = std::sqrt(rxy * rxy + r[2] * r[2]);
    sp.lat_deg = deg(gclat);   // 地心≈大地（LEO 扁率差 <0.2°，地图级足够）
    sp.lon_deg = lon;
    sp.alt_km = rmag - kReKm;
    return sp;
}

std::vector<PassEvent> predict_passes(const TleElements& tle,
                                      const GroundSite& site,
                                      std::int64_t t0_unix,
                                      double window_hours, double min_elev_deg) {
    std::vector<PassEvent> out;
    if (!tle.valid()) return out;
    const double lat = rad(site.lat_deg), lon = rad(site.lon_deg);
    const double ct = window_hours * 3600.0;
    const double step = 30.0;
    // GMST（度，简化）+ 站点 ECEF
    auto elev_at = [&](double t_sec) -> double {
        const std::array<double, 3> r =
            propagate_eci(tle, double(t0_unix + std::int64_t(t_sec)) / 86400.0 +
                                   (unix_to_jd(t0_unix) - tle.epoch_jd));
        // GMST（ Greenwich 平恒星时，度）
        const double d = unix_to_jd(t0_unix + std::int64_t(t_sec)) - 2451545.0;
        const double gmst = 280.46061837 + 360.98564736629 * d;
        const double g = rad(gmst);
        const double xe = r[0] * std::cos(g) + r[1] * std::sin(g);
        const double ye = -r[0] * std::sin(g) + r[1] * std::cos(g);
        const double ze = r[2];
        // 站点向量
        const double rs = kReKm + site.alt_km;
        const double sx = rs * std::cos(lat) * std::cos(lon);
        const double sy = rs * std::cos(lat) * std::sin(lon);
        const double sz = rs * std::sin(lat);
        const double dx = xe - sx, dy = ye - sy, dz = ze - sz;
        const double rng = std::sqrt(dx * dx + dy * dy + dz * dz);
        // 仰角 = 视线·天顶方向
        const double zdot = (dx * sx + dy * sy + dz * sz) / (rng * rs);
        return deg(std::asin(std::clamp(zdot, -1.0, 1.0)));
    };
    bool in_pass = false;
    PassEvent cur{};
    double prev = elev_at(0.0);
    for (double t = step; t <= ct; t += step) {
        const double e = elev_at(t);
        const bool up = e >= min_elev_deg;
        if (up && !in_pass) {
            in_pass = true;
            cur = PassEvent{};
            cur.start_unix = t0_unix + std::int64_t(t);
            cur.peak_elev_deg = e;
            cur.peak_unix = cur.start_unix;
        } else if (up && in_pass) {
            if (e > cur.peak_elev_deg) {
                cur.peak_elev_deg = e;
                cur.peak_unix = t0_unix + std::int64_t(t);
            }
        } else if (!up && in_pass) {
            in_pass = false;
            cur.end_unix = t0_unix + std::int64_t(t);
            if (cur.peak_elev_deg >= min_elev_deg + 0.5) out.push_back(cur);
        }
        prev = e;
    }
    (void)prev;
    return out;
}

} // namespace hackrftool::dsp
