#include "ui/status_text.hpp"

#include <cstdio>

namespace hackrftool::ui {

std::wstring status_dynamic(const StatusInfo& s) {
    if (!s.running)
        return s.has_frame ? L" · 帧 " + std::to_wstring(s.frame_seq) : std::wstring();

    std::wstring text = L"接收中 · ";
    if (s.sweep) {
        wchar_t seg[48];
        swprintf(seg, 48, L"扫描 段 %d/5 @ %.1f MHz", s.seg_idx + 1, s.seg_center_mhz);
        text += seg;
    } else {
        wchar_t ctr[32];
        swprintf(ctr, 32, L"中心 %.1f MHz", s.center_mhz);
        text += ctr;
    }
    wchar_t rf[48];
    swprintf(rf, 48, L" · %u Msps · LNA %u/VGA %u", s.rate_msps, s.lna_db, s.vga_db);
    text += rf;
    if (s.has_frame) text += L" · 帧 " + std::to_wstring(s.frame_seq);
    if (s.recording)
        text += L" · ●录制 " + std::to_wstring(s.rec_bytes / (1024u * 1024u)) + L"MB";
    return text;
}

} // namespace hackrftool::ui
