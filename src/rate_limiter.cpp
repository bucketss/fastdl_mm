#include "rate_limiter.h"

void RateLimiter::trim(std::deque<Clock::time_point>& values, Clock::time_point cutoff) {
    while (!values.empty() && values.front() <= cutoff) {
        values.pop_front();
    }
}

void RateLimiter::cleanup(Clock::time_point now) {
    if (nextCleanup_ != Clock::time_point{} && now < nextCleanup_) {
        return;
    }
    const auto stale = now - std::chrono::minutes(15);
    for (auto it = clients_.begin(); it != clients_.end();) {
        if (it->second.lastSeen < stale && it->second.blockedUntil <= now) {
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
    nextCleanup_ = now + std::chrono::minutes(10);
}

bool RateLimiter::blocked(const std::string& ip) {
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    cleanup(now);
    const auto it = clients_.find(ip);
    return it != clients_.end() && it->second.blockedUntil > now;
}

bool RateLimiter::requestExceeded(const std::string& ip, unsigned int limit) {
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    cleanup(now);
    auto& client = clients_[ip];
    client.lastSeen = now;
    trim(client.requests, now - std::chrono::minutes(1));
    // Rejected requests are not recorded, or a retrying client stays pinned at
    // the limit and can never drain back under it.
    if (limit == 0 || client.requests.size() >= limit) {
        return true;
    }
    client.requests.push_back(now);
    return false;
}

void RateLimiter::recordDenial(
    const std::string& ip, unsigned int limit, unsigned int blockSeconds) {
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    cleanup(now);
    auto& client = clients_[ip];
    client.lastSeen = now;
    trim(client.denials, now - std::chrono::minutes(1));
    client.denials.push_back(now);
    if (limit == 0 || client.denials.size() >= limit) {
        client.blockedUntil = now + std::chrono::seconds(blockSeconds);
        client.denials.clear();
    }
}

std::vector<RateLimiter::Block> RateLimiter::blockedClients() {
    const auto now = Clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Block> result;
    for (const auto& [ip, client] : clients_) {
        if (client.blockedUntil <= now) continue;
        const auto left = std::chrono::ceil<std::chrono::seconds>(client.blockedUntil - now);
        result.push_back({ip, static_cast<long long>(left.count())});
    }
    return result;
}

bool RateLimiter::unblock(const std::string& ip) {
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.erase(ip) != 0;
}

void RateLimiter::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.clear();
    nextCleanup_ = {};
}

