#include "ui/apt_view.hpp"

#include <algorithm>
#include <cstring>

#include <objidl.h>   // IStream 等（gdiplus.h 的前置要求）
#include <gdiplus.h>

namespace hackrftool::ui {

namespace {
constexpr wchar_t kCls[] = L"HackRFAptView";
AptView* g_view = nullptr;   // 单实例（主窗口只建一个云图视图）
} // namespace

void AptView::register_class(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &AptView::proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kCls;
    RegisterClassExW(&wc);
}

bool AptView::create(HWND parent, HINSTANCE inst) {
    wnd_ = CreateWindowExW(0, kCls, nullptr, WS_CHILD, 0, 0, 10, 10, parent,
                           nullptr, inst, nullptr);
    g_view = this;
    return wnd_ != nullptr;
}

void AptView::rebuild_dib(const hackrftool::dsp::AptImage& img) {
    const std::size_t rows = std::max<std::size_t>(img.rows, 1);
    if (rows_ == rows && !dib_.empty()) {   // 行数未变只拷像素
        std::memcpy(bits_, img.px.data(), img.px.size());
        return;
    }
    const std::size_t head = sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD);
    dib_.assign(head + rows * hackrftool::dsp::AptImage::kWidth, 0);
    bmi_ = reinterpret_cast<BITMAPINFO*>(dib_.data());
    bmi_->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi_->bmiHeader.biWidth = LONG(hackrftool::dsp::AptImage::kWidth);
    bmi_->bmiHeader.biHeight = -LONG(rows);   // 顶向下
    bmi_->bmiHeader.biPlanes = 1;
    bmi_->bmiHeader.biBitCount = 8;
    bmi_->bmiHeader.biCompression = BI_RGB;
    bmi_->bmiHeader.biSizeImage = DWORD(rows * hackrftool::dsp::AptImage::kWidth);
    for (int i = 0; i < 256; ++i) {
        bmi_->bmiColors[i].rgbRed = BYTE(i);
        bmi_->bmiColors[i].rgbGreen = BYTE(i);
        bmi_->bmiColors[i].rgbBlue = BYTE(i);
        bmi_->bmiColors[i].rgbReserved = 0;
    }
    bits_ = dib_.data() + head;
    std::memcpy(bits_, img.px.data(), img.px.size());
    rows_ = rows;
}

void AptView::refresh(hackrftool::dsp::AptDecoder& dec) {
    if (wnd_ == nullptr) return;
    static hackrftool::dsp::AptImage snap;
    dec.snapshot(snap);
    if (snap.rows == 0) return;
    rebuild_dib(snap);
    InvalidateRect(wnd_, nullptr, FALSE);
}

LRESULT CALLBACK AptView::proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        RECT rc;
        GetClientRect(wnd, &rc);
        if (g_view == nullptr || g_view->bits_ == nullptr || g_view->rows_ == 0) {
            FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, L"等待卫星信号…\n勾选「记录」后，卫星过境时云图在此逐行累积",
                      -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        } else {
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0,
                          hackrftool::dsp::AptImage::kWidth, int(g_view->rows_), g_view->bits_,
                          g_view->bmi_, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(wnd, &ps);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

bool save_apt_png(const wchar_t* path, const hackrftool::dsp::AptImage& img) {
    if (img.rows == 0) return false;
    Gdiplus::GdiplusStartupInput si;
    ULONG_PTR tok = 0;
    if (Gdiplus::GdiplusStartup(&tok, &si, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    // 灰度像素 → 24bpp BGR 位图（GDI+ 8bpp PNG 调色板路径繁琐，直接展开）
    std::vector<std::uint8_t> bgr(img.px.size() * 3);
    for (std::size_t i = 0; i < img.px.size(); ++i) {
        bgr[i * 3] = img.px[i];
        bgr[i * 3 + 1] = img.px[i];
        bgr[i * 3 + 2] = img.px[i];
    }
    Gdiplus::Bitmap bmp(int(hackrftool::dsp::AptImage::kWidth), int(img.rows),
                        int(hackrftool::dsp::AptImage::kWidth * 3), PixelFormat24bppRGB, bgr.data());
    CLSID png{};
    // PNG encoder CLSID（图像解码器枚举省略——固定值即可靠）
    const BYTE png_clsid[16] = {0x55, 0x7C, 0xFC, 0x27, 0x1A, 0x04, 0x11, 0xD3,
                                0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E};
    std::memcpy(&png, png_clsid, sizeof png);
    ok = bmp.Save(path, &png) == Gdiplus::Ok;
    Gdiplus::GdiplusShutdown(tok);
    return ok;
}

} // namespace hackrftool::ui
