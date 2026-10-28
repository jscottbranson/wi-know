#pragma once
#include <cstddef>
#include <cstdint>

namespace wiknow {
// ESP-IDF sig_len includes four FCS bytes. Only complete bounded management
// frames reach the worker; parsing (including IE validation) remains deferred.
inline size_t wifi_capture_size(const uint8_t *frame, size_t sig_len, bool receive_ok) {
    if (!frame || !receive_ok || sig_len < 28 || sig_len - 4 > 512)
        return 0;
    const bool nan = frame[0] == 0xd0 && sig_len >= 34 && frame[24] == 4 && frame[25] == 9 &&
                     frame[26] == 0x50 && frame[27] == 0x6f && frame[28] == 0x9a &&
                     frame[29] == 0x13;
    return frame[0] == 0x40 || frame[0] == 0x80 || frame[0] == 0x50 || nan ? sig_len - 4 : 0;
}
} // namespace wiknow
