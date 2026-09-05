// 48 kHz 立体声 waveOut 输出（#53 收音机）：4×100ms 缓冲队列，欠载自动静音。
// 仅主程序使用（依赖 winmm），不进测试 target。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <windows.h>
#include <mmsystem.h>

namespace hackrftool::audio {

class WaveOut {
public:
    WaveOut() = default;
    ~WaveOut();
    WaveOut(const WaveOut&) = delete;
    WaveOut& operator=(const WaveOut&) = delete;

    bool start();          // 打开 48k/16bit/立体声
    void stop();
    [[nodiscard]] bool running() const noexcept { return wave_ != nullptr; }

    // 写入音频（阻塞凑整块；fm 线程调用）
    void write(const float* l, const float* r, std::size_t n);

    void set_volume(float v) noexcept { volume_ = v; }   // 0..1
    void set_mute(bool m) noexcept { muted_ = m; }

private:
    void pump();
    HWAVEOUT wave_ = nullptr;
    static constexpr std::size_t kBlocks = 4;
    static constexpr std::size_t kSamples = 4800;   // 100 ms
    std::vector<std::int16_t> pcm_[kBlocks];
    WAVEHDR hdr_[kBlocks]{};
    std::size_t next_ = 0;      // 待填充块
    std::size_t filled_ = 0;    // 已填样本数
    float volume_ = 0.8f;
    bool muted_ = false;
};

} // namespace hackrftool::audio
