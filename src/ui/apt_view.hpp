// APT 云图原生显示窗（#54）：GDI StretchDIBits 灰度 DIB（WinFlux 渲染器
// 无位图接口，原生骨架正好）。云图页时覆盖内容区显示；PNG 保存走 GDI+。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <windows.h>

#include "dsp/apt.hpp"

namespace hackrftool::ui {

class AptView {
public:
    static void register_class(HINSTANCE inst);
    bool create(HWND parent, HINSTANCE inst);
    [[nodiscard]] HWND hwnd() const noexcept { return wnd_; }

    // 拉取解码器快照 → 更新 DIB → 重绘（UI 线程调用，行数变化时）
    void refresh(hackrftool::dsp::AptDecoder& dec);

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    void rebuild_dib(const hackrftool::dsp::AptImage& img);
    HWND wnd_ = nullptr;
    std::vector<std::uint8_t> dib_;   // BITMAPINFO + 8bpp 顶向下灰度像素
    BITMAPINFO* bmi_ = nullptr;
    std::uint8_t* bits_ = nullptr;
    std::size_t rows_ = 0;
};

// 灰度图 → PNG（GDI+）。失败返回 false。
[[nodiscard]] bool save_apt_png(const wchar_t* path,
                                const hackrftool::dsp::AptImage& img);

} // namespace hackrftool::ui
