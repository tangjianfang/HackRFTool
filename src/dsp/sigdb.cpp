#include "dsp/sigdb.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace hackrftool::dsp {

namespace {
constexpr BandInfo kRadio{L"FM 广播", SigCat::radio, 87.5, 108.0};
constexpr BandInfo kSat{L"NOAA 卫星", SigCat::sat, 137.0, 138.0};
constexpr BandInfo kIsm{L"2.4G ISM", SigCat::ism, 2400.0, 2483.5};
constexpr BandInfo kOther{L"其他", SigCat::other, 0.0, 0.0};
} // namespace

BandInfo band_of(double mhz) noexcept {
    if (mhz >= kRadio.lo_mhz && mhz <= kRadio.hi_mhz) return kRadio;
    if (mhz >= kSat.lo_mhz && mhz <= kSat.hi_mhz) return kSat;
    if (mhz >= kIsm.lo_mhz && mhz <= kIsm.hi_mhz) return kIsm;
    return kOther;
}

double default_center(SigCat cat) noexcept {
    switch (cat) {
    case SigCat::radio: return 98.0;
    case SigCat::sat: return 137.620;
    case SigCat::ism: return 2450.0;
    default: return 2450.0;
    }
}

bool in_band(SigCat cat, double mhz) noexcept {
    const BandInfo b = band_of(mhz);
    return b.cat == cat;
}

void merge_scan(std::vector<SignalEntry>& list, double mhz, float db,
                double tol_mhz) {
    for (auto& e : list) {
        if (std::abs(e.mhz - mhz) <= tol_mhz) {
            e.mhz = mhz;
            e.db = db;
            e.online = true;
            return;
        }
    }
    list.push_back({mhz, db, true});
    std::sort(list.begin(), list.end(),
              [](const SignalEntry& a, const SignalEntry& b) { return a.mhz < b.mhz; });
}

void mark_all_offline(std::vector<SignalEntry>& list) noexcept {
    for (auto& e : list) e.online = false;
}

std::vector<std::size_t> filter_signals(const std::vector<SignalEntry>& list,
                                        SigCat cat, bool online_only,
                                        bool by_strength) noexcept {
    std::vector<std::size_t> idx;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (cat != SigCat::all && band_of(list[i].mhz).cat != cat) continue;
        if (online_only && !list[i].online) continue;
        idx.push_back(i);
    }
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
        return by_strength ? list[a].db > list[b].db : list[a].mhz < list[b].mhz;
    });
    return idx;
}

long random_pick(const std::vector<SignalEntry>& list, SigCat cat,
                 unsigned seed) noexcept {
    const auto idx = filter_signals(list, cat, true, false);
    if (idx.empty()) return -1;
    std::srand(seed);
    return long(idx[std::rand() % idx.size()]);
}

bool save_signals(const wchar_t* path,
                  const std::vector<SignalEntry>& list) noexcept {
    std::vector<SignalEntry> sorted = list;
    std::sort(sorted.begin(), sorted.end(),
              [](const SignalEntry& a, const SignalEntry& b) {
                  return a.mhz < b.mhz;
              });
    if (std::FILE* f = _wfopen(path, L"w")) {
        for (const auto& e : sorted)
            std::fprintf(f, "%.4f\t%.1f\t%d\n", e.mhz, e.db, e.online ? 1 : 0);
        std::fclose(f);
        return true;
    }
    return false;
}

std::vector<SignalEntry> load_signals(const wchar_t* path) noexcept {
    std::vector<SignalEntry> out;
    if (std::FILE* f = _wfopen(path, L"r")) {
        char line[128];
        while (std::fgets(line, sizeof line, f) != nullptr) {
            double mhz = 0;
            double db = 0;
            int on = 0;
            if (std::sscanf(line, "%lf\t%lf\t%d", &mhz, &db, &on) == 3 &&
                mhz > 0.0)
                out.push_back({mhz, float(db), on != 0});
        }
        std::fclose(f);
    }
    return out;
}

} // namespace hackrftool::dsp
