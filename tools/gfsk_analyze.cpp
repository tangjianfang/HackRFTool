// 离线分析控制台：HackRFAnalyze <file.cs8> [threshold_db=-40] [fs_hz=20e6] [sym_rate=1e6]
// 突发检测 + 每突发 GFSK 解调（比特数 + 前 16 字节 hex 预览）
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dsp/burst_detector.hpp"
#include "dsp/gfsk.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: HackRFAnalyze <file.cs8> [threshold_db=-40] [fs_hz=20e6] "
                    "[sym_rate=1e6]\n");
        return 1;
    }
    const float threshold = (argc > 2) ? std::strtof(argv[2], nullptr) : -40.0f;
    const double fs = (argc > 3) ? std::atof(argv[3]) : 20e6;
    const double sym_rate = (argc > 4) ? std::atof(argv[4]) : 1e6;
    constexpr double kDevHz = 160e3;

    std::FILE* fp = std::fopen(argv[1], "rb");
    if (fp == nullptr) {
        std::printf("无法打开文件: %s\n", argv[1]);
        return 1;
    }
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    std::vector<std::int8_t> iq(size);
    if (std::fread(iq.data(), 1, iq.size(), fp) != iq.size()) {
        std::printf("读取失败\n");
        std::fclose(fp);
        return 1;
    }
    std::fclose(fp);

    const std::size_t n_c = iq.size() / 2;
    std::printf("文件: %s  %ld 字节  %zu 复样本  %.3f 秒 @ %.0f Hz\n", argv[1], size, n_c,
                double(n_c) / fs, fs);

    const std::size_t window = 256;   // 12.8 µs @ 20 Msps
    const auto bursts =
        hackrftool::dsp::detect_bursts(iq.data(), iq.size(), threshold, window);
    std::printf("阈值 %.1f dB 检出 %zu 个突发:\n", threshold, bursts.size());
    std::printf("%4s %12s %10s %8s %8s %10s  %s\n", "#", "start_us", "len_us", "peak_db",
                "bits", "bit_q>0.5", "前16字节hex");

    const hackrftool::dsp::GfskDemod demod(fs, sym_rate, kDevHz);
    for (std::size_t b = 0; b < bursts.size(); ++b) {
        const auto& bu = bursts[b];
        // 切片转复浮点
        std::vector<std::complex<float>> slice(bu.length_samples);
        for (std::size_t i = 0; i < bu.length_samples; ++i) {
            slice[i] = {float(iq[(bu.start_sample + i) * 2]),
                        float(iq[(bu.start_sample + i) * 2 + 1])};
        }
        const auto r = demod.demod(slice, 0);
        std::size_t good = 0;
        for (const float q : r.quality)
            if (q > 0.5f) ++good;

        // 前 128 比特 MSB-first 打包为 16 字节
        char hex[64];
        std::size_t hx = 0;
        const std::size_t n_bits = std::min<std::size_t>(r.bits.size(), 128);
        for (std::size_t byte_i = 0; byte_i < 16; ++byte_i) {
            unsigned v = 0;
            for (std::size_t bit_i = 0; bit_i < 8; ++bit_i) {
                const std::size_t idx = byte_i * 8 + bit_i;
                v <<= 1;
                if (idx < n_bits) v |= r.bits[idx] & 1u;
            }
            hx += std::snprintf(hex + hx, sizeof hex - hx, "%02X", v);
        }

        std::printf("%4zu %12.1f %10.1f %8.1f %8zu %10zu  %s\n", b,
                    double(bu.start_sample) / fs * 1e6,
                    double(bu.length_samples) / fs * 1e6, bu.peak_db, r.bits.size(),
                    good, hex);
    }

    if (bursts.empty()) {
        std::printf("（无突发：降低阈值重试，或该频段当前无活动）\n");
        return 3;
    }
    std::printf("提示: WiFi 为 OFDM/宽带，GFSK 比特仅对蓝牙 BR/无线键鼠类突发有意义；\n"
                "抓取自家鼠标键盘：录制时持续移动鼠标，再对比静默时段的突发。\n");
    return 0;
}
