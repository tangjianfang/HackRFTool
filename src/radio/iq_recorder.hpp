// IQ 录制器：USB 回调线程只入队（有界，满则丢最旧），独立写线程落盘。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hackrftool::radio {

// 采集参数快照（与 RadioConfig 字段对应；独立结构避免测试目标依赖 libhackrf 头）
struct SidecarInfo {
    double center_hz = 0.0;
    double sample_rate_hz = 0.0;
    unsigned lna_db = 0;
    unsigned vga_db = 0;
    bool amp = false;
};

// 在 IQ 文件旁写 <path>.txt 参数侧车（频率/采样率/增益/格式/时间），
// 供离线分析工具对上采集条件。失败返回 false。
[[nodiscard]] bool write_capture_sidecar(const std::wstring& iq_path,
                                         const SidecarInfo& info);

class IqRecorder {
public:
    ~IqRecorder();

    IqRecorder() = default;
    IqRecorder(const IqRecorder&) = delete;
    IqRecorder& operator=(const IqRecorder&) = delete;

    // 打开文件并启动写线程；已录制中返回 false
    [[nodiscard]] bool start(const std::wstring& path);
    // USB 回调线程调用：入队（队列上限 64 块，满丢最旧并计数）
    void write(const std::int8_t* data, std::size_t bytes);
    // 停止：冲刷剩余块、合并写线程
    bool stop();
    [[nodiscard]] bool recording() const noexcept { return running_.load(); }
    [[nodiscard]] std::uint64_t bytes_written() const noexcept { return written_.load(); }
    [[nodiscard]] std::uint64_t dropped_blocks() const noexcept { return dropped_.load(); }

private:
    void writer_loop();

    std::atomic<bool> running_{false};
    std::atomic<bool> quit_{false};
    std::atomic<std::uint64_t> written_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::thread writer_;
    std::mutex mutex_;
    std::deque<std::vector<std::int8_t>> queue_;
    std::FILE* file_ = nullptr;   // 仅写线程访问
};

} // namespace hackrftool::radio
