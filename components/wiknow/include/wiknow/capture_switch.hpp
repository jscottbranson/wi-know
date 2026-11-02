#pragma once
#include <cstdint>

namespace wiknow {
// Owned by the logging controller; startup samples immediately, later edges debounce.
class CaptureSwitch {
    bool enabled_, base_all_, on_{}, candidate_{};
    uint32_t debounce_;
    uint64_t since_{};

  public:
    CaptureSwitch(bool enabled, bool base_all, uint32_t debounce)
        : enabled_(enabled), base_all_(base_all), debounce_(debounce) {}
    void start(bool high, uint64_t now) {
        on_ = candidate_ = enabled_ && high;
        since_ = now;
    }
    bool update(bool high, uint64_t now) {
        if (!enabled_)
            return false;
        if (high != candidate_) {
            candidate_ = high;
            since_ = now;
        }
        if (candidate_ == on_ || now - since_ < debounce_)
            return false;
        on_ = candidate_;
        return true;
    }
    bool on() const {
        return on_;
    }
    bool all() const {
        return base_all_ || on_;
    }
    bool retain(bool matched, bool remote_id = false) const {
        return matched || remote_id || all();
    }
};
} // namespace wiknow
