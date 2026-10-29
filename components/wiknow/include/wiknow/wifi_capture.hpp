#pragma once
#include <cstddef>
#include <cstdint>

namespace wiknow {
// Three-address ordinary/QoS data only. Protected bodies are deliberately ignored.
// Reject retries, fragments, four-address WDS, HT control and A-MSDU (whose inner
// source/destination are not represented by the outer header).
inline size_t wifi_data_header_size(const uint8_t *frame, size_t length) {
    if (!frame || length < 24 || (frame[0] != 0x08 && frame[0] != 0x88) || (frame[1] & 0x8c) ||
        (frame[1] & 3) == 3 || (frame[22] & 15))
        return 0;
    const size_t header = frame[0] == 0x88 ? 26 : 24;
    if (length < header || (header == 26 && (frame[24] & 0x80)))
        return 0;
    return header;
}
// ESP-IDF sig_len includes four FCS bytes. Data retains ONLY its bounded MAC
// header even when the frame is larger than the management-frame capture cap.
inline size_t wifi_capture_size(const uint8_t *frame, size_t sig_len, bool receive_ok) {
    if (!frame || !receive_ok || sig_len < 28)
        return 0;
    if ((frame[0] & 0x0c) == 8)
        return wifi_data_header_size(frame, sig_len - 4);
    if (sig_len - 4 > 512)
        return 0;
    const bool nan = frame[0] == 0xd0 && sig_len >= 34 && frame[24] == 4 && frame[25] == 9 &&
                     frame[26] == 0x50 && frame[27] == 0x6f && frame[28] == 0x9a &&
                     frame[29] == 0x13;
    return frame[0] == 0x40 || frame[0] == 0x80 || frame[0] == 0x50 || nan ? sig_len - 4 : 0;
}
// At most 20 data headers/s and four data slots in the shared eight-slot queue;
// management traffic can use all slots. Clock reversal reopens the gate.
class WifiDataAdmission {
  public:
    bool accept(uint64_t now, size_t free_slots) {
        if (free_slots <= 4 || (seen_ && now >= last_ && now - last_ < 50))
            return false;
        seen_ = true;
        last_ = now;
        return true;
    }

  private:
    uint64_t last_{};
    bool seen_{};
};
} // namespace wiknow
