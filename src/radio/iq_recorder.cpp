#include "radio/iq_recorder.hpp"

#include <chrono>
#include <cstdio>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace hackrftool::radio {

namespace {
constexpr std::size_t kMaxQueueBlocks = 64;   // ≈16 MB @262144B/块
}

bool write_capture_sidecar(const std::wstring& iq_path, const SidecarInfo& cfg) {
    std::wstring path = iq_path + L".txt";
    if (std::FILE* fp = _wfopen(path.c_str(), L"w")) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        std::fprintf(fp,
                     "# HackRFTool IQ capture\n"
                     "center_mhz=%.1f\n"
                     "sample_rate_msps=%.0f\n"
                     "lna_db=%u\n"
                     "vga_db=%u\n"
                     "amp=%d\n"
                     "format=int8 interleaved I/Q (.cs8)\n"
                     "captured=%04u-%02u-%02u %02u:%02u:%02u\n",
                     cfg.center_hz / 1e6, cfg.sample_rate_hz / 1e6,
                     cfg.lna_db, cfg.vga_db, cfg.amp ? 1 : 0, st.wYear,
                     st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::fclose(fp);
        return true;
    }
    return false;
}

IqRecorder::~IqRecorder() { stop(); }

bool IqRecorder::start(const std::wstring& path) {
    if (running_.load()) return false;
    file_ = _wfopen(path.c_str(), L"wb");
    if (file_ == nullptr) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }
    written_.store(0);
    dropped_.store(0);
    quit_.store(false);
    running_.store(true);
    writer_ = std::thread([this] { writer_loop(); });
    return true;
}

void IqRecorder::write(const std::int8_t* data, std::size_t bytes) {
    if (!running_.load() || bytes == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kMaxQueueBlocks) {
        queue_.pop_front();
        dropped_.fetch_add(1);
    }
    queue_.emplace_back(data, data + bytes);
}

bool IqRecorder::stop() {
    if (!running_.exchange(false)) return false;
    quit_.store(true);
    if (writer_.joinable()) writer_.join();
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    return true;
}

void IqRecorder::writer_loop() {
    while (true) {
        std::vector<std::int8_t> block;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!queue_.empty()) {
                block = std::move(queue_.front());
                queue_.pop_front();
            }
        }
        if (!block.empty()) {
            if (std::fwrite(block.data(), 1, block.size(), file_) == block.size())
                written_.fetch_add(block.size());
            continue;   // 优先清空队列
        }
        if (quit_.load()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace hackrftool::radio
