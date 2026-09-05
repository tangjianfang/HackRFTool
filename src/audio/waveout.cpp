#include "audio/waveout.hpp"

#include <algorithm>
#include <cstring>

namespace hackrftool::audio {

WaveOut::~WaveOut() { stop(); }

bool WaveOut::start() {
    if (wave_ != nullptr) return true;
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 48000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = 48000 * 4;
    if (waveOutOpen(&wave_, WAVE_MAPPER, &fmt,
                    reinterpret_cast<DWORD_PTR>(nullptr),
                    reinterpret_cast<DWORD_PTR>(nullptr),
                    CALLBACK_NULL) != MMSYSERR_NOERROR) {
        wave_ = nullptr;
        return false;
    }
    for (std::size_t i = 0; i < kBlocks; ++i) {
        pcm_[i].assign(kSamples * 2, 0);
        hdr_[i].lpData = reinterpret_cast<LPSTR>(pcm_[i].data());
        hdr_[i].dwBufferLength = DWORD(kSamples * 2 * sizeof(std::int16_t));
    }
    // 预投全部空块（欠载静音）
    for (std::size_t i = 0; i < kBlocks; ++i) {
        waveOutPrepareHeader(wave_, &hdr_[i], sizeof(WAVEHDR));
        waveOutWrite(wave_, &hdr_[i], sizeof(WAVEHDR));
    }
    next_ = 0;
    filled_ = 0;
    return true;
}

void WaveOut::stop() {
    if (wave_ == nullptr) return;
    waveOutReset(wave_);
    for (std::size_t i = 0; i < kBlocks; ++i)
        waveOutUnprepareHeader(wave_, &hdr_[i], sizeof(WAVEHDR));
    waveOutClose(wave_);
    wave_ = nullptr;
}

void WaveOut::write(const float* l, const float* r, std::size_t n) {
    if (wave_ == nullptr) return;
    const float g = muted_ ? 0.0f : volume_;
    while (n > 0) {
        WAVEHDR& h = hdr_[next_];
        if ((h.dwFlags & WHDR_DONE) == 0 && filled_ == 0) return;   // 队列满（丢块保实时）
        const std::size_t room = kSamples - filled_;
        const std::size_t m = std::min(room, n);
        for (std::size_t i = 0; i < m; ++i) {
            const float lv = std::clamp(l[i] * g, -1.0f, 1.0f);
            const float rv = std::clamp(r[i] * g, -1.0f, 1.0f);
            pcm_[next_][filled_ * 2] = std::int16_t(lv * 32767.0f);
            pcm_[next_][filled_ * 2 + 1] = std::int16_t(rv * 32767.0f);
            ++filled_;
        }
        l += m;
        r += m;
        n -= m;
        if (filled_ >= kSamples) {
            waveOutWrite(wave_, &h, sizeof(WAVEHDR));   // 重投（Reset 后 DONE 位复用）
            h.dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
            next_ = (next_ + 1) % kBlocks;
            filled_ = 0;
        }
    }
}

} // namespace hackrftool::audio
