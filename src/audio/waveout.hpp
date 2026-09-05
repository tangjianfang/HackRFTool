// 48 kHz 立体声 waveOut 输出（#53/#55b 重写）：回调驱动（WOM_DONE 释放
// 缓冲块），8×50ms 块。首版轮转块的提交/复用逻辑有缺陷（设备开着但
// 无声，真机诊断实锤：音频块持续产出而喇叭无声），改教科书事件模型。
// 仅主程序使用（依赖 winmm），不进测试 target。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <mmsystem.h>

namespace hackrftool::audio {

class WaveOut {
public:
    WaveOut() { InitializeCriticalSection(&cs_); }
    ~WaveOut();
    WaveOut(const WaveOut&) = delete;
    WaveOut& operator=(const WaveOut&) = delete;

    bool start(int device_id = -1);   // -1 = WAVE_MAPPER（系统默认）
    void stop();
    [[nodiscard]] bool running() const noexcept { return wave_ != nullptr; }

    // 写入音频（fm 线程调用；空闲块不足时短暂等待，仍不足则丢弃保实时）
    void write(const float* l, const float* r, std::size_t n);

    void set_volume(float v) noexcept { volume_ = v; }
    void set_mute(bool m) noexcept { muted_ = m; }

    // 设备播放位置（ms；诊断/同步用，未运行返回 0）
    [[nodiscard]] unsigned long position_ms() const noexcept {
        if (wave_ == nullptr) return 0;
        MMTIME mm{};
        mm.wType = TIME_SAMPLES;   // wave 设备必支持；TIME_MS 可能被驱动改写
        if (waveOutGetPosition(wave_, &mm, sizeof mm) != MMSYSERR_NOERROR)
            return 0;
        const unsigned long long s =
            mm.wType == TIME_SAMPLES ? mm.u.sample
            : mm.wType == TIME_MS    ? mm.u.ms * 48ull
                                     : mm.u.cb / 4ull;   // cb=字节计数/4=样本
        return unsigned(s / 48ull);
    }

    // 枚举波形输出设备（设备 id → 名称），供输出设备选择
    [[nodiscard]] static std::vector<std::wstring> enum_devices();

    // ——诊断（#55b 临时）——
    [[nodiscard]] long dbg_submitted() const noexcept { return dbg_submitted_; }
    [[nodiscard]] long dbg_werr() const noexcept { return dbg_werr_; }
    [[nodiscard]] long dbg_free() const noexcept { return free_blocks_; }

private:
    static void CALLBACK proc(HWAVEOUT w, UINT msg, DWORD_PTR inst,
                              DWORD_PTR p1, DWORD_PTR p2, DWORD_PTR p3) noexcept;
    bool submit_block();   // staging 满块 → 空闲块提交

    HWAVEOUT wave_ = nullptr;
    static constexpr std::size_t kBlocks = 8;
    static constexpr std::size_t kBlockSamples = 2400;   // 50 ms @48k
    std::vector<std::int16_t> pcm_[kBlocks];
    WAVEHDR hdr_[kBlocks]{};
    LONG free_blocks_ = 0;          // WOM_DONE ++ / submit --
    CRITICAL_SECTION cs_;
    std::vector<std::int16_t> staging_;   // 攒块暂存（kBlockSamples*2）
    float volume_ = 0.8f;
    bool muted_ = false;
    // ——诊断（#55b 临时）——
    volatile LONG dbg_submitted_ = 0;
    volatile LONG dbg_werr_ = 0;
};

} // namespace hackrftool::audio
