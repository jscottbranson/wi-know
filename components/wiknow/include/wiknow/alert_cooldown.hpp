#pragma once
#include <cstdint>

namespace wiknow {
// Shared by rule and following alerts. Call only when an eligible alert requests a beep.
class AlertCooldown {
  public:
    bool request(uint64_t now, uint32_t interval_ms) {
        if (has_alerted_ && (now < last_alert_ || now - last_alert_ < interval_ms))
            return false;
        has_alerted_ = true;
        last_alert_ = now;
        return true;
    }

  private:
    bool has_alerted_{};
    uint64_t last_alert_{};
};
} // namespace wiknow
