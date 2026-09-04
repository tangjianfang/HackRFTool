#include "radio/hackrf.hpp"

namespace hackrftool::radio {

namespace {
int rx_trampoline(hackrf_transfer* t) {
    // 回调契约：只读 transfer，不调 libhackrf
    const auto* tramp = static_cast<HackRadio::RxTrampoline*>(t->rx_ctx);
    if (tramp != nullptr && tramp->cb != nullptr)
        tramp->cb(reinterpret_cast<const std::int8_t*>(t->buffer),
                  static_cast<std::size_t>(t->valid_length), tramp->ctx);
    return 0;   // 0 = 继续流
}
} // namespace

HackRadio::~HackRadio() {
    stop_rx();
    close();
}

bool HackRadio::load_api(std::string* error) {
    const auto fail = [this, error](const char* what) {
        if (error != nullptr) *error = std::string("libhackrf 加载失败：") + what;
        close();
        return false;
    };
    module_ = LoadLibraryW(L"libhackrf.dll");
    if (module_ == nullptr) return fail("LoadLibraryW(libhackrf.dll)");

#define LOAD_FN(field, name)                                                   \
    field = reinterpret_cast<decltype(field)>(                                 \
        reinterpret_cast<void*>(GetProcAddress(module_, name)));               \
    if (field == nullptr) return fail("缺函数 " name);
    LOAD_FN(fn_init, "hackrf_init")
    LOAD_FN(fn_exit, "hackrf_exit")
    LOAD_FN(fn_open, "hackrf_open")
    LOAD_FN(fn_close, "hackrf_close")
    LOAD_FN(fn_start_rx, "hackrf_start_rx")
    LOAD_FN(fn_stop_rx, "hackrf_stop_rx")
    LOAD_FN(fn_set_freq, "hackrf_set_freq")
    LOAD_FN(fn_set_sample_rate, "hackrf_set_sample_rate")
    LOAD_FN(fn_set_lna_gain, "hackrf_set_lna_gain")
    LOAD_FN(fn_set_vga_gain, "hackrf_set_vga_gain")
    LOAD_FN(fn_set_amp_enable, "hackrf_set_amp_enable")
    LOAD_FN(fn_library_version, "hackrf_library_version")
    LOAD_FN(fn_error_name, "hackrf_error_name")
#undef LOAD_FN
    return true;
}

bool HackRadio::open(std::string* error) {
    if (is_open()) return true;
    if (!load_api(error)) return false;
    int rc = fn_init();
    if (rc != HACKRF_SUCCESS) {
        if (error != nullptr) *error = "hackrf_init: " + hackrf_error_text(rc);
        return false;
    }
    inited_ = true;
    rc = fn_open(&dev_);
    if (rc != HACKRF_SUCCESS) {
        if (error != nullptr) *error = "hackrf_open: " + hackrf_error_text(rc);
        close();
        return false;
    }
    return true;
}

void HackRadio::close() {
    if (dev_ != nullptr) {
        fn_close(dev_);
        dev_ = nullptr;
    }
    if (inited_) {
        fn_exit();
        inited_ = false;
    }
    if (module_ != nullptr) {
        FreeLibrary(module_);
        module_ = nullptr;
    }
}

bool HackRadio::apply(const RadioConfig& cfg, std::string* error) {
    if (!is_open()) {
        if (error != nullptr) *error = "设备未打开";
        return false;
    }
    const struct Step {
        const char* name;
        int rc;
    } steps[] = {
        {"set_freq", fn_set_freq(dev_, static_cast<std::uint64_t>(cfg.center_hz))},
        {"set_sample_rate", fn_set_sample_rate(dev_, cfg.sample_rate_hz)},
        {"set_lna_gain", fn_set_lna_gain(dev_, cfg.lna_gain_db)},
        {"set_vga_gain", fn_set_vga_gain(dev_, cfg.vga_gain_db)},
        {"set_amp_enable", fn_set_amp_enable(dev_, cfg.amp ? 1 : 0)},
    };
    for (const auto& s : steps) {
        if (s.rc != HACKRF_SUCCESS) {
            if (error != nullptr)
                *error = std::string(s.name) + ": " + hackrf_error_text(s.rc);
            return false;
        }
    }
    return true;
}

bool HackRadio::set_center_hz(double hz) noexcept {
    if (!is_open()) return false;
    return fn_set_freq(dev_, static_cast<std::uint64_t>(hz)) == HACKRF_SUCCESS;
}

bool HackRadio::start_rx(Callback cb, void* ctx, std::string* error) {
    if (!is_open()) {
        if (error != nullptr) *error = "设备未打开";
        return false;
    }
    trampoline_ = {cb, ctx};
    const int rc = fn_start_rx(dev_, &rx_trampoline, &trampoline_);
    if (rc != HACKRF_SUCCESS) {
        if (error != nullptr) *error = "hackrf_start_rx: " + hackrf_error_text(rc);
        return false;
    }
    running_ = true;
    return true;
}

void HackRadio::stop_rx() {
    if (running_ && dev_ != nullptr) fn_stop_rx(dev_);
    running_ = false;
}

std::string HackRadio::library_version() const {
    return (fn_library_version != nullptr) ? fn_library_version() : "(未加载)";
}

std::string HackRadio::hackrf_error_text(int code) const {
    const char* s = (fn_error_name != nullptr)
                        ? fn_error_name(static_cast<hackrf_error>(code))
                        : nullptr;
    return (s != nullptr) ? s : ("未知错误 " + std::to_string(code));
}

} // namespace hackrftool::radio
