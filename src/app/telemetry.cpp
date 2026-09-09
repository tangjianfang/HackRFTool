#include "telemetry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace hackrftool::log {

std::uint64_t now_ms() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

std::string to_jsonl(const Event& e) {
    static const char* kLv[] = {"debug", "info", "warn", "error"};
    std::string out = "{\"ts\":";
    out += std::to_string(e.ts);
    out += ",\"level\":\"";
    out += kLv[int(e.level)];
    out += "\",\"cat\":\"";
    out += json_escape(e.cat);
    out += "\",\"event\":\"";
    out += json_escape(e.event);
    out += '"';
    for (const auto& [k, v] : e.kv) {
        out += ",\"";
        out += json_escape(k);
        out += "\":\"";
        out += json_escape(v);
        out += '"';
    }
    out += "}\n";
    return out;
}

void Logger::open(const std::wstring& path, std::size_t max_bytes) {
    std::lock_guard<std::mutex> g(mtx_);
    close_locked();   // 复用锁内关闭（见下）
    if (path.empty()) return;
    // spdlog 轮转 sink（#105）：追加模式续写，超限改名 .1/.2（与原手写
    // rotate 语义一致：主文件 + 2 份归档）；宽路径转 UTF-8（sink 收窄串）
    std::string path8;
    path8.reserve(path.size());
    for (const wchar_t w : path)
        path8 += static_cast<char>(w < 128 ? w : '?');
    auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        path8, max_bytes, 2);
    auto spd = std::make_shared<spdlog::logger>("telemetry", std::move(sink));
    spd->set_pattern("%v");                        // 原始 JSONL 行（自带 ts/level）
    spd->set_level(spdlog::level::trace);
    spd->flush_on(spdlog::level::trace);           // 每条 fflush——崩溃也不丢
    spd_ = new std::shared_ptr<spdlog::logger>(std::move(spd));
}

void Logger::close() noexcept {
    std::lock_guard<std::mutex> g(mtx_);
    close_locked();
}

void Logger::close_locked() noexcept {
    // 堆上 shared_ptr 析构经 spdlog 关闭句柄（noexcept 下不抛）
    delete reinterpret_cast<std::shared_ptr<spdlog::logger>*>(spd_);
    spd_ = nullptr;
}

void Logger::write(
    Level level, std::string_view cat, std::string_view event,
    std::initializer_list<std::pair<std::string, std::string>> kv) {
    // #104：debug 级门控——关闭时零成本丢弃（构串都不做）
    if (level == Level::debug && !debug_.load(std::memory_order_relaxed))
        return;
    Event e;
    e.ts = now_ms();
    e.level = level;
    e.cat.assign(cat);
    e.event.assign(event);
    for (const auto& p : kv) e.kv.push_back(p);
    const std::string line = to_jsonl(e);
    std::lock_guard<std::mutex> g(mtx_);
    ring_.push_back(std::move(e));
    if (ring_.size() > kRing)
        ring_.erase(ring_.begin(), ring_.end() - static_cast<long>(kRing));
    ++total_;
    // #105：文件落盘走 spdlog 轮转 sink（%v 原始行；行尾换行由 sink 补，
    // 构串时去掉 to_jsonl 的 '\n' 防双换行）
    if (spd_ == nullptr) return;
    (*reinterpret_cast<const std::shared_ptr<spdlog::logger>*>(spd_))
        ->info(std::string_view(line).substr(0, line.size() - 1));
}

std::vector<Event> Logger::tail(std::size_t n) const {
    std::lock_guard<std::mutex> g(mtx_);
    const std::size_t take = std::min(n, ring_.size());
    return {ring_.end() - static_cast<long>(take), ring_.end()};
}

std::size_t Logger::count_event(std::string_view cat,
                                std::string_view event) const {
    std::lock_guard<std::mutex> g(mtx_);
    return static_cast<std::size_t>(std::count_if(
        ring_.begin(), ring_.end(), [&](const Event& e) {
            return e.cat == cat && e.event == event;
        }));
}

Logger& Logger::instance() {
    static Logger g;
    return g;
}

} // namespace hackrftool::log
