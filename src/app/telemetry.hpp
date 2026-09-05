// 遥测日志（#59）：UI/点击/事件/数据全量结构化记录（JSONL），供脚本
// 断言与问题分析——替代视觉识别做验收（用户指令：视觉识别不准）。
// 纯 C++ 无 UI/硬件依赖；线程安全；文件轮转防占盘。
#pragma once

#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hackrftool::log {

enum class Level { debug = 0, info, warn, error };

// 一条结构化事件：ts(ms 模拟启动时钟) + level + cat + event + kv 数据
struct Event {
    std::uint64_t ts = 0;
    Level level = Level::info;
    std::string cat;    // UI/DSP/RADIO/AUDIO/SETTINGS/SCAN/LIFE
    std::string event;  // 点号命名，如 page.switch / gain.change
    std::vector<std::pair<std::string, std::string>> kv;
};

// JSON 值转义（引号/反斜杠/控制字符）——纯函数可单测
[[nodiscard]] std::string json_escape(std::string_view s);

// 事件序列化为单行 JSON（含 ts/level/cat/event/kv）——纯函数可单测
[[nodiscard]] std::string to_jsonl(const Event& e);

class Logger {
public:
    // path 为空则只进环形缓冲（测试/无盘场景）
    void open(const std::wstring& path, std::size_t max_bytes = 1 << 20);
    void close() noexcept;

    void write(Level level, std::string_view cat, std::string_view event,
               std::initializer_list<std::pair<std::string, std::string>> kv);

    // 环形缓冲快照（最近 n 条，按时间序）——自测断言/UI 导出用
    [[nodiscard]] std::vector<Event> tail(std::size_t n) const;
    [[nodiscard]] std::size_t count() const noexcept { return total_; }
    // 按 cat/event 过滤计数（验收断言"点击事件都记到了"用）
    [[nodiscard]] std::size_t count_event(std::string_view cat,
                                          std::string_view event) const;

    // 单实例：dsp/radio 层无 App 依赖也能埋点
    [[nodiscard]] static Logger& instance();

private:
    void close_locked() noexcept;     // 调用方持锁
    mutable std::mutex mtx_;
    std::vector<Event> ring_;          // 最近 kRing 条
    std::uint64_t total_ = 0;          // 累计条数（未清零）
    void* file_ = nullptr;             // FILE*（void* 防头文件带 cstdio）
    std::size_t max_bytes_ = 1 << 20;
    std::size_t written_ = 0;
    std::wstring path_;
    static constexpr std::size_t kRing = 600;
};

// 便捷入口：log_telemetry(info, "UI", "click", {{"id","101"}});
inline void log_telemetry(
    Level level, std::string_view cat, std::string_view event,
    std::initializer_list<std::pair<std::string, std::string>> kv = {}) {
    Logger::instance().write(level, cat, event, kv);
}

// 毫秒时钟（QueryTickCount 换算——Win 下 GetTickCount64，测试下可注入）
[[nodiscard]] std::uint64_t now_ms() noexcept;

} // namespace hackrftool::log
