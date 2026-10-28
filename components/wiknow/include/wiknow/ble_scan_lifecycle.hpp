#pragma once
#include <cstdint>

namespace wiknow {
// Only the scheduler pauses Wi-Fi. A maintenance retry must not start BLE early
// when local-scanning intent changes during an active standalone Wi-Fi dwell.
inline bool ble_phase_wants_scan(bool standalone, bool ble_phase, bool universal, bool local_scan,
                                 bool wifi_receiving) {
    return !standalone || (!wifi_receiving && (ble_phase || (universal && !local_scan)));
}

// The controller's stop-command ACK can bypass advertising events already queued
// on the NimBLE host event queue. Keep scan callbacks inactive until a marker
// posted AFTER that ACK has drained those events. Every start path uses this gate.
// All methods run on the same host event queue; invalidation also synchronizes
// completed-report generations with the separate parser worker.
class BleScanLifecycle {
    uint32_t boundary_{}, posted_{};
    bool pending_{};

  public:
    bool pending() const {
        return pending_;
    }
    template <class Invalidate, class Post> void boundary(Invalidate invalidate, Post post) {
        invalidate();
        ++boundary_;
        if (!pending_) {
            pending_ = true;
            posted_ = boundary_;
            post();
        }
    }
    template <class Post> bool drained(Post post) {
        // A reset/sync can occur while an earlier marker is queued. That marker
        // cannot cover events queued after it, so append a fresh marker first.
        if (posted_ != boundary_) {
            posted_ = boundary_;
            post();
            return false;
        }
        pending_ = false;
        return true;
    }
    template <class Active, class Start>
    bool start(bool ready, bool desired, Active active, Start start_scan) {
        if (!ready || !desired || pending_)
            return false;
        return active() || start_scan();
    }
};
} // namespace wiknow
