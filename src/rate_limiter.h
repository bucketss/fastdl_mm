#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class RateLimiter {
public:
    struct Block {
        std::string ip;
        long long secondsLeft;
    };

    std::vector<Block> blockedClients();
    bool blocked(const std::string& ip);
    bool requestExceeded(const std::string& ip, unsigned int limit);
    void recordDenial(const std::string& ip, unsigned int limit, unsigned int blockSeconds);
    bool unblock(const std::string& ip);
    void clear();

private:
    using Clock = std::chrono::steady_clock;
    struct Client {
        std::deque<Clock::time_point> requests;
        std::deque<Clock::time_point> denials;
        Clock::time_point blockedUntil{};
        Clock::time_point lastSeen{};
    };

    static void trim(std::deque<Clock::time_point>& values, Clock::time_point cutoff);
    void cleanup(Clock::time_point now);

    std::mutex mutex_;
    std::unordered_map<std::string, Client> clients_;
    Clock::time_point nextCleanup_{};
};

