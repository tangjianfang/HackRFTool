#include "telemetry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

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
    path_ = path;
    max_bytes_ = max_bytes;
    written_ = 0;
    if (path.empty()) return;
    // 追加模式：进程重启不截断历史（轮转在写入侧按大小触发）
    file_ = _wfopen(path.c_str(), L"ab");
}

void Logger::close() noexcept {
    std::lock_guard<std::mutex> g(mtx_);
    close_locked();
}

void Logger::close_locked() noexcept {
    if (file_ != nullptr) {
        std::fclose(static_cast<std::FILE*>(file_));
        file_ = nullptr;
    }
}

// 轮转：hackrftool.jsonl 超限时改名 .1（旧 .1→.2，.2 丢弃），共保 3 份
static void rotate(const std::wstring& path) {
    const std::wstring p2 = path + L".2";
    const std::wstring p1 = path + L".1";
    _wremove(p2.c_str());
    (void)_wrename(p1.c_str(), p2.c_str());
    (void)_wrename(path.c_str(), p1.c_str());
}

void Logger::write(
    Level level, std::string_view cat, std::string_view event,
    std::initializer_list<std::pair<std::string, std::string>> kv) {
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
    if (file_ == nullptr) return;
    written_ += line.size();
    if (written_ > max_bytes_) {
        close_locked();
        rotate(path_);
        file_ = _wfopen(path_.c_str(), L"wb");
        written_ = 0;
        if (file_ == nullptr) return;
    }
    std::fwrite(line.data(), 1, line.size(), static_cast<std::FILE*>(file_));
    std::fflush(static_cast<std::FILE*>(file_));   // 崩溃也不丢已记事件
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
