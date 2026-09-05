// 状态栏动态段纯函数：供 build() 拼接底部状态栏，本头文件保持纯 std
//（无 flux/windows 依赖），让 dsp_test 可以直接单测文本契约。
#pragma once

#include <string>

namespace hackrftool::ui {

// 状态栏输入快照：UI 线程每 build 从 State/原子量填一份
struct StatusInfo {
    bool running = false;
    bool sweep = false;               // 全频段扫描模式
    bool recording = false;
    bool has_frame = false;           // analyzer 是否已产出至少一帧
    int seg_idx = 0;                  // 0..4（sweep 时显示 seg_idx+1）
    double seg_center_mhz = 0.0;      // 当前驻留段中心
    double center_mhz = 0.0;          // 单窗中心
    unsigned rate_msps = 20;
    unsigned lna_db = 16;
    unsigned vga_db = 16;
    unsigned long long frame_seq = 0;
    unsigned long long rec_bytes = 0;
};

// 动态段（不含 app.status 基础文本）：
//   接收中 → "接收中 · 中心 2450.0 MHz · 20 Msps · LNA 16/VGA 16[ · 帧 N][ · ●录制 NMB]"
//   接收中+sweep → "接收中 · 扫描 段 2/5 @ 2417.5 MHz · …"
//   已停止且有帧 → " · 帧 N"；已停止且无帧 → ""
[[nodiscard]] std::wstring status_dynamic(const StatusInfo& s);

} // namespace hackrftool::ui
