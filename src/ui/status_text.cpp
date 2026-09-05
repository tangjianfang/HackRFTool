#include "ui/status_text.hpp"

#include <cstdio>

namespace hackrftool::ui {

void status_parts(const StatusInfo& s, std::wstring out[5]) {
    if (s.sweep) {
        wchar_t seg[48];
        swprintf(seg, 48, L"扫描 段 %d/5 @ %.1f MHz", s.seg_idx + 1, s.seg_center_mhz);
        out[0] = seg;
    } else {
        wchar_t ctr[32];
        swprintf(ctr, 32, L"中心 %.1f MHz", s.center_mhz);
        out[0] = ctr;
    }
    wchar_t rg[48];
    swprintf(rg, 48, L"%u Msps · LNA %u/VGA %u", s.rate_msps, s.lna_db, s.vga_db);
    out[1] = rg;
    out[2] = s.has_frame ? L"帧 " + std::to_wstring(s.frame_seq) : std::wstring();
    out[3] = s.recording
                 ? L"●录制 " + std::to_wstring(s.rec_bytes / (1024u * 1024u)) + L"MB"
                 : std::wstring();
    out[4] = s.esb_hits > 0 ? L"ESB " + std::to_wstring(s.esb_hits) : std::wstring();
}

std::wstring status_dynamic(const StatusInfo& s) {
    if (!s.running)
        return s.has_frame ? L" · 帧 " + std::to_wstring(s.frame_seq) : std::wstring();

    std::wstring text = L"接收中";
    std::wstring parts[5];
    status_parts(s, parts);
    for (const auto& p : parts)
        if (!p.empty()) text += L" · " + p;
    return text;
}

} // namespace hackrftool::ui
