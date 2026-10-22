#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>

namespace RecentLog {
using Clock=std::chrono::steady_clock;
inline constexpr auto Retention=std::chrono::seconds(30);
inline constexpr size_t Capacity=300;
struct Entry {
    std::uint64_t id=0;
    Clock::time_point created;
    std::string text;
};
inline void Prune(std::deque<Entry>& entries,Clock::time_point now=Clock::now()) {
    // UI notifications can arrive before an older backend snapshot.
    entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const Entry& e){
        return now-e.created>=Retention;
    }),entries.end());
    while(entries.size()>Capacity)entries.pop_front();
}
class Buffer {
    std::deque<Entry> entries;
    std::uint64_t nextId=0;
public:
    void Append(const std::string& text,Clock::time_point now=Clock::now()) {
        entries.push_back({++nextId,now,text});Prune(entries,now);
    }
    void Expire(Clock::time_point now=Clock::now()){Prune(entries,now);}
    std::deque<Entry> Snapshot(Clock::time_point now=Clock::now()) {
        Expire(now);return entries;
    }
};
}
// One locked snapshot preserves both event identity and original timestamps.
std::deque<RecentLog::Entry> GetRecentBackendLogs();
