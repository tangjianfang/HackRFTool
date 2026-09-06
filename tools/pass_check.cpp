// 过境预测快速验证（#82）：读 weather-tle.txt，对深圳预测选中卫星 24h 过境
// 用法: pass_check [卫星名前缀，默认 NOAA 15]
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include "dsp/orbit.hpp"

int main(int argc, char** argv) {
    const std::string want = argc > 1 ? argv[1] : "NOAA 15";
    std::FILE* f = std::fopen("weather-tle.txt", "rb");
    if (f == nullptr) {
        std::printf("no weather-tle.txt in CWD\n");
        return 1;
    }
    std::string txt;
    char buf[512];
    for (size_t g; (g = std::fread(buf, 1, sizeof buf, f)) > 0;) txt.append(buf, g);
    std::fclose(f);
    std::vector<std::string> ls;
    std::size_t pos = 0;
    while (pos <= txt.size()) {
        auto e = txt.find('\n', pos);
        ls.push_back(txt.substr(pos, (e == std::string::npos ? txt.size() : e) - pos));
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    int found = 0;
    for (std::size_t i = 0; i + 2 < ls.size(); ++i) {
        if (ls[i + 1].size() > 60 && ls[i + 1][0] == '1' && ls[i + 2][0] == '2') {
            auto t = hackrftool::dsp::parse_tle(ls[i], ls[i + 1], ls[i + 2]);
            if (!t.valid()) { i += 2; continue; }
            ++found;
            if (t.name.rfind(want, 0) == 0) {
                std::printf("matched: %s  incl=%.3f mm=%.5f ecc=%.6f\n",
                            t.name.c_str(), t.incl_deg, t.mean_motion, t.ecc);
                const hackrftool::dsp::GroundSite sz{22.5431, 114.0579, 0.0};
                const std::int64_t now = std::int64_t(std::time(nullptr));
                auto ps = hackrftool::dsp::predict_passes(t, sz, now, 24.0, 10.0);
                std::printf("passes in 24h: %zu\n", ps.size());
                for (const auto& e : ps) {
                    std::tm st{}, pk{}, en{};
                    const std::int64_t bj = 8 * 3600;
                    std::time_t s1 = std::time_t(e.start_unix + bj);
                    std::time_t s2 = std::time_t(e.peak_unix + bj);
                    std::time_t s3 = std::time_t(e.end_unix + bj);
                    gmtime_s(&st, &s1);
                    gmtime_s(&pk, &s2);
                    gmtime_s(&en, &s3);
                    std::printf("  %02d:%02d → %02d:%02d（峰 %02d:%02d %d°）\n",
                                st.tm_hour, st.tm_min, en.tm_hour, en.tm_min,
                                pk.tm_hour, pk.tm_min, int(e.peak_elev_deg));
                }
                return 0;
            }
            i += 2;
        }
    }
    std::printf("no match for %s (%d tles parsed)\n", want.c_str(), found);
    return 2;
}
