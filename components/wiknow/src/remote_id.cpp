#include "wiknow/remote_id.hpp"

namespace wiknow {
namespace {
bool append(const uint8_t *data, size_t size, uint8_t counter, RemoteIdMessages &out) {
    if (!size || (data[0] & 15) != 2)
        return false;
    size_t count = 1;
    if ((data[0] >> 4) == 15) {
        if (size < 3 || data[1] != 25 || data[2] == 0 || data[2] > 9 || size != 3u + 25u * data[2])
            return false;
        count = data[2];
        data += 3;
    } else if (size != 25)
        return false;
    if (out.count + count > out.messages.size())
        return false;
    for (size_t i = 0; i < count; ++i, data += 25) {
        if ((data[0] & 15) != 2 || (data[0] >> 4) > 5)
            return false;
        out.messages[out.count] = data;
        out.counters[out.count++] = counter;
    }
    return true;
}
} // namespace
RemoteIdMessages remote_id_messages(const Observation &o) {
    RemoteIdMessages out{};
    if (o.payload_size > o.payload.size())
        return out;
    const auto *p = o.payload.data();
    if (o.radio == Radio::wifi) {
        // Payload holds the complete vendor IE body: OUI, type, counter, message pack.
        if (o.payload_size >= 6 && p[0] == 0xfa && p[1] == 0x0b && p[2] == 0xbc && p[3] == 0x0d &&
            (p[5] >> 4) == 15 && append(p + 5, o.payload_size - 5, p[4], out))
            return out;
        return {};
    }
    for (size_t at = 0; at < o.payload_size;) {
        size_t n = p[at];
        if (!n)
            break;
        if (at + n + 1 > o.payload_size)
            return {};
        // Service Data UUID 0xFFFA, application code 0x0D, message counter.
        if (n >= 4 && p[at + 1] == 0x16 && p[at + 2] == 0xfa && p[at + 3] == 0xff &&
            p[at + 4] == 0x0d) {
            if (n < 6 || !append(p + at + 6, n - 5, p[at + 5], out))
                return {};
        }
        at += n + 1;
    }
    return out;
}
} // namespace wiknow
