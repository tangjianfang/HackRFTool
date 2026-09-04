// 真机冒烟：打开设备 → 配置 2450 MHz/20 Msps → 收 0.5 秒 → 报告字节数
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "radio/hackrf.hpp"

namespace {
std::atomic<std::size_t> g_bytes{0};
std::atomic<std::size_t> g_blocks{0};

void rx_cb(const std::int8_t* /*iq*/, std::size_t bytes, void* /*ctx*/) {
    g_bytes += bytes;
    g_blocks += 1;
}
} // namespace

int main() {
    using namespace hackrftool::radio;
    HackRadio radio;
    std::string err;
    if (!radio.open(&err)) {
        std::printf("打开失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("libhackrf %s\n", radio.library_version().c_str());

    RadioConfig cfg;   // 默认 2450 MHz / 20 Msps / LNA32 / VGA30
    if (!radio.apply(cfg, &err)) {
        std::printf("配置失败: %s\n", err.c_str());
        return 1;
    }
    if (!radio.start_rx(&rx_cb, nullptr, &err)) {
        std::printf("启动接收失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("接收中 0.5 秒...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    radio.stop_rx();

    const std::size_t bytes = g_bytes.load();
    std::printf("收到 %zu 字节 / %zu 块（满速率约 40e6 字节/秒）\n", bytes,
                g_blocks.load());
    const bool ok = bytes > 5'000'000;   // >5MB 即认为流水线健康
    std::printf(ok ? "冒烟通过\n" : "冒烟失败：数据量不足\n");
    return ok ? 0 : 1;
}
