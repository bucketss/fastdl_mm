#include "logger.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace {
std::tm localNow() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return local;
}

bool sameDay(const std::tm& left, const std::tm& right) {
    return left.tm_year == right.tm_year && left.tm_yday == right.tm_yday;
}

std::string dateStamp(const std::tm& day) {
    // Sized for the widest int each field could print, not for a valid date.
    std::array<char, 40> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02d",
        day.tm_year + 1900, day.tm_mon + 1, day.tm_mday);
    return buffer.data();
}

// Midnight of the given day, as a comparable time_t.
std::time_t midnight(int year, int month, int dayOfMonth) {
    std::tm value{};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = dayOfMonth;
    value.tm_isdst = -1;
    return std::mktime(&value);
}

// Parses "YYYY-MM-DD" out of a rotated file name.
bool parseStamp(const std::string& stamp, std::time_t& out) {
    if (stamp.size() != 10 || stamp[4] != '-' || stamp[7] != '-') return false;
    for (std::size_t i = 0; i < stamp.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (std::isdigit(static_cast<unsigned char>(stamp[i])) == 0) return false;
    }
    const int year = std::stoi(stamp.substr(0, 4));
    const int month = std::stoi(stamp.substr(5, 2));
    const int day = std::stoi(stamp.substr(8, 2));
    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    out = midnight(year, month, day);
    return out != static_cast<std::time_t>(-1);
}
} // namespace

AsyncLogger::~AsyncLogger() {
    stop();
}

std::filesystem::path AsyncLogger::dailyPath(const std::tm& day) const {
    const auto stem = basePath_.stem().u8string();
    const auto extension = basePath_.extension().u8string();
    const auto name = stem + "-" + dateStamp(day) + extension;
    const auto parent = basePath_.parent_path();
    return parent.empty() ? std::filesystem::u8path(name)
                          : parent / std::filesystem::u8path(name);
}

void AsyncLogger::purge(const std::tm& today) const {
    if (keepDays_ == 0) return;

    std::tm oldest = today;
    oldest.tm_mday -= static_cast<int>(keepDays_) - 1;
    oldest.tm_hour = 0;
    oldest.tm_min = 0;
    oldest.tm_sec = 0;
    oldest.tm_isdst = -1;
    const std::time_t cutoff = std::mktime(&oldest);
    if (cutoff == static_cast<std::time_t>(-1)) return;

    auto parent = basePath_.parent_path();
    if (parent.empty()) parent = ".";
    const auto stem = basePath_.stem().u8string();
    const auto extension = basePath_.extension().u8string();
    const auto prefix = stem + "-";

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) continue;
        const auto name = entry.path().filename().u8string();
        if (name.size() != prefix.size() + 10 + extension.size()) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (!extension.empty() &&
            name.compare(name.size() - extension.size(), extension.size(), extension) != 0) {
            continue;
        }
        std::time_t stamp = 0;
        if (!parseStamp(name.substr(prefix.size(), 10), stamp)) continue;
        if (stamp < cutoff) {
            std::error_code removeError;
            std::filesystem::remove(entry.path(), removeError);
        }
    }
}

std::filesystem::path AsyncLogger::pathForToday() const {
    return dailyPath(localNow());
}

bool AsyncLogger::start(const std::filesystem::path& basePath, unsigned int keepDays) {
    stop();
    std::error_code error;
    if (basePath.has_parent_path()) {
        std::filesystem::create_directories(basePath.parent_path(), error);
        if (error) {
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        basePath_ = basePath;
        keepDays_ = keepDays;
        dropped_ = 0;
    }

    std::ofstream probe(dailyPath(localNow()), std::ios::app);
    if (!probe) {
        return false;
    }
    probe.close();

    // Cleared last: stop() left it set, and while it is set write() discards
    // instead of queueing lines no thread would ever drain.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = false;
    }
    thread_ = std::thread(&AsyncLogger::run, this);
    return true;
}

void AsyncLogger::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
}

void AsyncLogger::write(std::string line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
        return;
    }
    if (queue_.size() >= kMaxQueuedLines) {
        ++dropped_;
        return;
    }
    queue_.push_back(std::move(line));
    ready_.notify_one();
}

void AsyncLogger::run() {
    std::ofstream output;
    std::tm openDay{};
    bool opened = false;

    for (;;) {
        std::deque<std::string> batch;
        std::size_t dropped = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            batch.swap(queue_);
            dropped = dropped_;
            dropped_ = 0;
            if (batch.empty() && stopping_) {
                break;
            }
        }

        const std::tm today = localNow();
        if (!opened || !output || !sameDay(today, openDay)) {
            output.close();
            output.clear();
            output.open(dailyPath(today), std::ios::app);
            // Only count the file as open if it really opened. Claiming it
            // regardless would skip every later reopen until the date changed,
            // so one transient failure would silently kill logging for a day.
            opened = output.is_open();
            if (opened) {
                openDay = today;
                purge(today);
            }
        }
        if (!output) {
            // Carry the loss forward so it is reported once a file opens again.
            std::lock_guard<std::mutex> lock(mutex_);
            dropped_ += dropped + batch.size();
            continue;
        }

        if (dropped != 0) {
            output << "[fastdl] dropped " << dropped << " log lines (queue full)\n";
        }
        for (const auto& line : batch) {
            output << line << '\n';
        }
        output.flush();
        if (!output) {
            std::lock_guard<std::mutex> lock(mutex_);
            dropped_ += batch.size();
        }
    }
}
