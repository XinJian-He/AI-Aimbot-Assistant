#pragma once
#include <atomic>

// KMBox reports input state on events. Silence is not a button-up event.
// -1 denotes no report in this listener session, rather than a pressed bit.
class KMBoxButtonState {
    std::atomic<int> buttons{-1};
public:
    void Reset(){buttons.store(-1,std::memory_order_release);}
    void Accept(unsigned char value){buttons.store(value,std::memory_order_release);}
    int Read(unsigned char mask) const {
        const int value=buttons.load(std::memory_order_acquire);
        return value<0?-1:((value&mask)?1:0);
    }
};
