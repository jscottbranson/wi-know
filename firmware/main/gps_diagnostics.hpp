#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace platform {
// Observe receiver time without changing the parser's validity decisions. Retain
// the checksum so serial captures can diagnose corrupt as well as invalid TIM.
class GpsDiagnostics {
  public:
    void feed(char byte, uint64_t now) {
        ++received_bytes;
        if (byte == '$') {
            length_ = 0;
            collecting_ = true;
        }
        if (!collecting_ || byte == '\r')
            return;
        if (byte == '\n') {
            collecting_ = false;
            line_[length_] = 0;
            if (!std::strncmp(line_, "$PCAS60,", 8)) {
                std::memcpy(last_tim, line_, length_ + 1);
                last_tim_at = now;
                ++tim_sentences;
            }
        } else if (length_ + 1 < sizeof(line_)) {
            line_[length_++] = byte;
        } else {
            collecting_ = false;
        }
    }
    uint64_t received_bytes{}, transmitted_bytes{}, tim_sentences{}, last_tim_at{};
    char last_tim[160]{};

  private:
    char line_[160]{};
    size_t length_{};
    bool collecting_{};
};
} // namespace platform
