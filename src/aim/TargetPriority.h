#pragma once
#include <algorithm>
#include <vector>

namespace target_priority {
inline int Rank(const std::vector<int>& order, int label) {
    return static_cast<int>(std::find(order.begin(), order.end(), label) - order.begin());
}
template<class Tracks>
int BestRank(const std::vector<int>& order, const Tracks& tracks) {
    int best = static_cast<int>(order.size());
    for (const auto& track : tracks) best = std::min(best, Rank(order, track.label));
    return best;
}
inline bool KeepLock(const std::vector<int>& order, int lockedLabel, int bestRank) {
    const int rank = Rank(order, lockedLabel);
    return rank < static_cast<int>(order.size()) && rank <= bestRank;
}
} // namespace target_priority
