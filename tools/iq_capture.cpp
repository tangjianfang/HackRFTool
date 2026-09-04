// IQ 采集控制台：HackRFCapture <out.cs8> <seconds> [center_mhz=2450] [lna=32] [vga=30]
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "radio/hackrf.hpp"
#include "radio/iq_recorder.hpp"

namespace {
hackrftool::radio::IqRecorder g_rec;
std::atomic<std::size_t> g_blocks{0};

void rx_cb(const std::int8_t* iq, std::size_t bytes, void* /*ctx*/) {
    g_rec.write(iq, bytes);
    g_blocks += 1;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("用法: HackRFCapture <out.cs8> <seconds> [center_mhz] [lna] [vga]\n");
        return 1;
    }
    const std::wstring path(argv[1], argv[1] + std::strlen(argv[1]));
    const double seconds = std::atof(argv[2]);
    if (seconds <= 0.0 || seconds > 600.0) {
        std::printf("seconds 取值 0-600\n");
        return 1;
    }
    hackrftool::radio::RadioConfig cfg;
    if (argc > 3) cfg.center_hz = std::atof(argv[3]) * 1e6;
    if (argc > 4) cfg.lna_gain_db = unsigned(std::atoi(argv[4]));
    if (argc > 5) cfg.vga_gain_db = unsigned(std::atoi(argv[5]));

    using namespace hackrftool::radio;
    HackRadio radio;
    std::string err;
    if (!radio.open(&err)) {
        std::printf("打开失败: %s\n", err.c_str());
        return 1;
    }
    if (!radio.apply(cfg, &err)) {
        std::printf("配置失败: %s\n", err.c_str());
        return 1;
    }
    if (!g_rec.start(path)) {
        std::printf("无法创建文件: %ls\n", path.c_str());
        return 1;
    }
    if (!radio.start_rx(&rx_cb, nullptr, &err)) {
        std::printf("启动接收失败: %s\n", err.c_str());
        g_rec.stop();
        return 1;
    }
    std::printf("采集 %.1f 秒 @ %.0f MHz / %.0f Msps ...\n", seconds, cfg.center_hz / 1e6,
                cfg.sample_rate_hz / 1e6);
    std::this_thread::sleep_for(std::chrono::milliseconds(int(seconds * 1000)));
    radio.stop_rx();
    g_rec.stop();

    const std::uint64_t bytes = g_rec.bytes_written();
    std::printf("完成: %llu 字节 / %llu 块（期望 %llu 字节，丢弃 %llu 块）\n",
                static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(g_blocks.load()),
                static_cast<unsigned long long>(
                    double(bytes ? bytes : 1) * 0.0 +
                    seconds * cfg.sample_rate_hz * 2.0),
                static_cast<unsigned long long>(g_rec.dropped_blocks()));
    return (bytes > 0 && g_rec.dropped_blocks() == 0) ? 0 : 2;
}
