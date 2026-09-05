#include "audio/waveout.hpp"

#include <algorithm>
#include <cstring>

namespace hackrftool::audio {

std::vector<std::wstring> WaveOut::enum_devices() {
    std::vector<std::wstring> out;
    const UINT n = waveOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR)
            out.emplace_back(caps.szPname);
    }
    return out;
}

WaveOut::~WaveOut() {
    stop();
    DeleteCriticalSection(&cs_);
}

void CALLBACK WaveOut::proc(HWAVEOUT, UINT msg, DWORD_PTR inst, DWORD_PTR,
                            DWORD_PTR p1, DWORD_PTR) noexcept {
    if (msg != WOM_DONE) return;
    auto* self = reinterpret_cast<WaveOut*>(inst);
    // p1 = 完成的 WAVEHDR*：本实现块与 header 一一对应，无需查表
    (void)p1;
    InterlockedIncrement(&self->free_blocks_);
}

bool WaveOut::start(int device_id) {
    if (wave_ != nullptr) return true;
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 48000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = 48000 * 4;
    const UINT dev = device_id < 0 ? UINT(WAVE_MAPPER) : UINT(device_id);
    if (waveOutOpen(&wave_, dev, &fmt,
                    reinterpret_cast<DWORD_PTR>(&WaveOut::proc),
                    reinterpret_cast<DWORD_PTR>(this),
                    CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        wave_ = nullptr;
        return false;
    }
    for (std::size_t i = 0; i < kBlocks; ++i) {
        pcm_[i].assign(kBlockSamples * 2, 0);
        hdr_[i].lpData = reinterpret_cast<LPSTR>(pcm_[i].data());
        hdr_[i].dwBufferLength = DWORD(kBlockSamples * 2 * sizeof(std::int16_t));
        waveOutPrepareHeader(wave_, &hdr_[i], sizeof(WAVEHDR));
    }
    free_blocks_ = LONG(kBlocks);
    staging_.clear();
    staging_.reserve(kBlockSamples * 2);
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

bool WaveOut::submit_block() {
    // 等空闲块（≤200ms；仍无则返回 false 由调用方丢弃保实时）
    for (int wait = 0; wait < 40 && free_blocks_ <= 0; ++wait) Sleep(5);
    if (free_blocks_ <= 0) return false;
    LONG took = 0;
    // 找一个非队列中的块（dwFlags 无 WHDR_INQUEUE）
    for (std::size_t i = 0; i < kBlocks; ++i) {
        WAVEHDR& h = hdr_[i];
        if ((h.dwFlags & WHDR_INQUEUE) != 0) continue;
        EnterCriticalSection(&cs_);
        if ((h.dwFlags & WHDR_INQUEUE) != 0) {   // 双检（回调并发）
            LeaveCriticalSection(&cs_);
            continue;
        }
        std::memcpy(pcm_[i].data(), staging_.data(),
                    kBlockSamples * 2 * sizeof(std::int16_t));
        h.dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
        LeaveCriticalSection(&cs_);
        took = InterlockedDecrement(&free_blocks_);
        const MMRESULT wr = waveOutWrite(wave_, &h, sizeof(WAVEHDR));
        dbg_werr_ = LONG(wr);
        if (wr != MMSYSERR_NOERROR) {
            InterlockedIncrement(&free_blocks_);   // 提交失败退还
            return false;
        }
        InterlockedIncrement(&dbg_submitted_);
        break;
    }
    staging_.clear();
    return took >= 0 || true;   // 提交成功（free 计数仅用于同步）
}

void WaveOut::write(const float* l, const float* r, std::size_t n) {
    if (wave_ == nullptr) return;
    const float g = muted_ ? 0.0f : volume_;
    while (n > 0) {
        const std::size_t room = kBlockSamples * 2 - staging_.size();
        const std::size_t m = std::min(room / 2, n);
        for (std::size_t i = 0; i < m; ++i) {
            staging_.push_back(
                std::int16_t(std::clamp(l[i] * g, -1.0f, 1.0f) * 32767.0f));
            staging_.push_back(
                std::int16_t(std::clamp(r[i] * g, -1.0f, 1.0f) * 32767.0f));
        }
        l += m;
        r += m;
        n -= m;
        if (staging_.size() >= kBlockSamples * 2) (void)submit_block();
    }
}

} // namespace hackrftool::audio
