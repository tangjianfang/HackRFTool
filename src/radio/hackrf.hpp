// libhackrf 动态加载封装：MSVC 不链接 MSYS2 的 MinGW 导入库，
// 全部函数经 LoadLibrary/GetProcAddress 取得；DLL 由构建后步骤复制到 exe 旁。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <windows.h>

#include <libhackrf/hackrf.h>

namespace hackrftool::radio {

struct RadioConfig {
    double center_hz = 2.45e9;
    double sample_rate_hz = 20e6;
    unsigned lna_gain_db = 32;   // 8..40，步进 8
    unsigned vga_gain_db = 30;   // 2..62，步进 2
    bool amp = false;
};

class HackRadio {
public:
    // 回调在 libusb 线程执行：不得调用本类方法，只应拷贝/处理数据
    using Callback = void (*)(const std::int8_t* iq, std::size_t bytes, void* ctx);

    // start_rx 内部经此中转把 libhackrf 回调翻译成 Callback
    struct RxTrampoline {
        Callback cb;
        void* ctx;
    };

    HackRadio() = default;
    ~HackRadio();
    HackRadio(const HackRadio&) = delete;
    HackRadio& operator=(const HackRadio&) = delete;

    [[nodiscard]] bool open(std::string* error = nullptr);
    void close();
    [[nodiscard]] bool apply(const RadioConfig& cfg, std::string* error = nullptr);
    [[nodiscard]] bool start_rx(Callback cb, void* ctx, std::string* error = nullptr);
    void stop_rx();

    [[nodiscard]] bool is_open() const noexcept { return dev_ != nullptr; }
    [[nodiscard]] bool is_running() const noexcept { return running_; }
    [[nodiscard]] std::string library_version() const;

private:
    [[nodiscard]] bool load_api(std::string* error);
    [[nodiscard]] std::string hackrf_error_text(int code) const;

    HMODULE module_ = nullptr;
    hackrf_device* dev_ = nullptr;
    bool inited_ = false;
    bool running_ = false;
    RxTrampoline trampoline_{};

    // 动态加载的 API（open 成功后有效）
    int (*fn_init)() = nullptr;
    int (*fn_exit)() = nullptr;
    int (*fn_open)(hackrf_device**) = nullptr;
    int (*fn_close)(hackrf_device*) = nullptr;
    int (*fn_start_rx)(hackrf_device*, hackrf_sample_block_cb_fn, void*) = nullptr;
    int (*fn_stop_rx)(hackrf_device*) = nullptr;
    int (*fn_set_freq)(hackrf_device*, std::uint64_t) = nullptr;
    int (*fn_set_sample_rate)(hackrf_device*, double) = nullptr;
    int (*fn_set_lna_gain)(hackrf_device*, std::uint32_t) = nullptr;
    int (*fn_set_vga_gain)(hackrf_device*, std::uint32_t) = nullptr;
    int (*fn_set_amp_enable)(hackrf_device*, std::uint8_t) = nullptr;
    const char* (*fn_library_version)() = nullptr;
    const char* (*fn_error_name)(hackrf_error) = nullptr;
};

} // namespace hackrftool::radio
