#pragma once
#include <cstddef>
#include <cstdint>

namespace wiknow {
// Volatile Air530Z output configuration: RMC and TIM only, once per solution.
// Reserved fields stay empty. Do not save to module flash or reset its RTC.
class GpsTimeRequest {
  public:
    static constexpr char command[] = "$PCAS03,0,0,0,0,1,0,0,0,0,0,,,0,0,,,,1*32\r\n";
    const char *pending(uint64_t now, size_t &size) const {
        size = now >= next_at_ ? sizeof(command) - 1 - offset_ : 0;
        return command + offset_;
    }
    void sent(uint64_t now, size_t size) {
        if (size > sizeof(command) - 1 - offset_)
            return;
        offset_ += size;
        if (offset_ == sizeof(command) - 1) {
            offset_ = 0;
            // Retry during startup; periodic refresh recovers a later module reset.
            next_at_ = now + (attempts_ < 5 ? 1000 : 30000);
            if (attempts_ < 5)
                ++attempts_;
        }
    }

  private:
    size_t offset_{};
    uint64_t next_at_{};
    unsigned attempts_{};
};
} // namespace wiknow
