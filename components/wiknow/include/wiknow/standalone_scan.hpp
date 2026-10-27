#pragma once
#include <cstddef>
#include <cstdint>

namespace wiknow {
// Advance only after a whole dwell; slow callers never skip channels to catch up.
class StandaloneScan {
  public:
    bool due(uint64_t now) const {
        return !started_ || now >= deadline_;
    }
    bool ble() const {
        return ble_;
    }
    size_t channel_index() const {
        return cursor_;
    }
    void started(uint64_t now, uint32_t wifi_ms, uint32_t ble_ms) {
        started_ = true;
        deadline_ = now + (ble_ ? ble_ms : wifi_ms);
    }
    void advance(bool ble_enabled, size_t channels) {
        if (ble_) {
            ble_ = false;
            cursor_ = (cursor_ + 1) % channels;
        } else if (ble_enabled) {
            ble_ = true;
        } else {
            cursor_ = (cursor_ + 1) % channels;
        }
    }
    bool has_started() const {
        return started_;
    }

  private:
    bool started_{}, ble_{};
    size_t cursor_{};
    uint64_t deadline_{};
};
} // namespace wiknow
