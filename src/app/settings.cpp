#include "settings.hpp"

#include <cmath>
#include <cstdlib>
#include <sstream>

namespace hackrftool::app {

namespace {

// 值域约束（与 App 字段同源）：越界值回退默认，防手改文件把 UI 搞瘫
constexpr int kPageMax = 4;
constexpr int kRateMax = 4;
constexpr int kFmBwMax = 2;
constexpr int kSigCatMax = 3;

[[nodiscard]] bool clamp_int(long long v, int lo, int hi, int& out) {
    if (v < lo || v > hi) return false;
    out = int(v);
    return true;
}

[[nodiscard]] bool finite_double(double v, double lo, double hi, double& out) {
    if (!std::isfinite(v) || v < lo || v > hi) return false;
    out = v;
    return true;
}

} // namespace

std::string serialize(const Settings& s) {
    std::ostringstream os;
    os << "# HackRFTool settings v1\n";
    os << "page\t" << s.page << "\n";
    os << "center_mhz\t" << s.center_mhz << "\n";
    os << "radio_mhz\t" << s.radio_mhz << "\n";
    os << "rate_index\t" << s.rate_index << "\n";
    os << "lna\t" << s.lna << "\n";
    os << "vga\t" << s.vga << "\n";
    os << "amp\t" << (s.amp ? 1 : 0) << "\n";
    os << "auto_track\t" << (s.auto_track ? 1 : 0) << "\n";
    os << "afc_on\t" << (s.afc_on ? 1 : 0) << "\n";
    os << "stereo_opt\t" << (s.stereo_opt ? 1 : 0) << "\n";
    os << "spec_zoom_idx\t" << s.spec_zoom_idx << "\n";
    os << "threshold\t" << s.threshold << "\n";
    os << "burst_thr\t" << s.burst_thr << "\n";
    os << "symrate_idx\t" << s.symrate_idx << "\n";
    os << "fm_bw\t" << s.fm_bw << "\n";
    os << "audio_dev\t" << s.audio_dev << "\n";
    os << "vol\t" << s.vol << "\n";
    os << "sig_cat\t" << s.sig_cat << "\n";
    os << "sig_online\t" << s.sig_online << "\n";
    os << "sig_sort\t" << s.sig_sort << "\n";
    return os.str();
}

std::optional<Settings> deserialize(std::string_view text) {
    if (text.empty()) return std::nullopt;
    Settings s;
    std::size_t pos = 0;
    bool any = false;
    while (pos <= text.size()) {
        const std::size_t eol = text.find('\n', pos);
        const std::size_t end = eol == std::string_view::npos ? text.size() : eol;
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line[0] == '#') {
            if (eol == std::string_view::npos) break;
            continue;
        }
        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, tab);
        const std::string val(line.substr(tab + 1));
        // strtod 失败返回 0 会被 int 约束误收（"abc"→vol=0）：endptr 必须
        // 走到末尾（允许尾随空白）才算数值
        char* vp = nullptr;
        const double d = std::strtod(val.c_str(), &vp);
        if (vp == val.c_str()) continue;    // 非数值 → 跳过该键
        while (*vp == ' ' || *vp == '\t') ++vp;
        if (*vp != '\0') continue;          // 数值后有杂物 → 跳过
        int i = 0;
        if (key == "page") {
            if (clamp_int(std::llround(d), 0, kPageMax, i)) { s.page = i; any = true; }
        } else if (key == "center_mhz") {
            if (finite_double(d, 24.0, 1800.0, s.center_mhz)) any = true;
        } else if (key == "radio_mhz") {
            if (finite_double(d, 24.0, 1800.0, s.radio_mhz)) any = true;
        } else if (key == "rate_index") {
            if (clamp_int(std::llround(d), 0, kRateMax, i)) { s.rate_index = i; any = true; }
        } else if (key == "lna") {
            if (clamp_int(std::llround(d), 8, 40, i)) { s.lna = unsigned(i); any = true; }
        } else if (key == "vga") {
            if (clamp_int(std::llround(d), 2, 62, i)) { s.vga = unsigned(i); any = true; }
        } else if (key == "amp") {
            if (clamp_int(std::llround(d), 0, 1, i)) { s.amp = i != 0; any = true; }
        } else if (key == "auto_track") {
            if (clamp_int(std::llround(d), 0, 1, i)) { s.auto_track = i != 0; any = true; }
        } else if (key == "afc_on") {
            if (clamp_int(std::llround(d), 0, 1, i)) { s.afc_on = i != 0; any = true; }
        } else if (key == "stereo_opt") {
            if (clamp_int(std::llround(d), 0, 1, i)) { s.stereo_opt = i != 0; any = true; }
        } else if (key == "spec_zoom_idx") {
            if (clamp_int(std::llround(d), 0, 3, i)) { s.spec_zoom_idx = i; any = true; }
        } else if (key == "threshold") {
            if (finite_double(d, -120.0, 0.0, s.threshold)) any = true;
        } else if (key == "burst_thr") {
            if (finite_double(d, -120.0, 0.0, s.burst_thr)) any = true;
        } else if (key == "symrate_idx") {
            if (clamp_int(std::llround(d), 0, 1, i)) { s.symrate_idx = i; any = true; }
        } else if (key == "fm_bw") {
            if (clamp_int(std::llround(d), 0, kFmBwMax, i)) { s.fm_bw = i; any = true; }
        } else if (key == "audio_dev") {
            if (clamp_int(std::llround(d), -1, 32, i)) { s.audio_dev = i; any = true; }
        } else if (key == "vol") {
            if (clamp_int(std::llround(d), 0, 100, i)) { s.vol = i; any = true; }
        } else if (key == "sig_cat") {
            if (clamp_int(std::llround(d), 0, kSigCatMax, i)) { s.sig_cat = i; any = true; }
        } else if (key == "sig_online") {
            if (clamp_int(std::llround(d), 0, 1, i)) { s.sig_online = i; any = true; }
        } else if (key == "sig_sort") {
            if (clamp_int(std::llround(d), 0, 1, i)) { s.sig_sort = i; any = true; }
        }
        if (eol == std::string_view::npos) break;
    }
    if (!any) return std::nullopt;
    return s;
}

} // namespace hackrftool::app
