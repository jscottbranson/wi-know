#include "wiknow/remote_id.hpp"
#include <algorithm>
#include <cstring>

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
// Supported OpenDroneID NAN profile. Retain the complete action body, including
// all attributes, within the existing 255-byte observation. Unknown attributes
// are skipped only after validating their lengths; duplicate descriptors fail.
RemoteIdMessages nan_messages(const uint8_t *p, size_t size) {
    constexpr uint8_t header[]{4, 9, 0x50, 0x6f, 0x9a, 0x13};
    constexpr uint8_t service[]{0x88, 0x69, 0x19, 0x9d, 0x92, 9};
    if (size < sizeof(header) || std::memcmp(p, header, sizeof(header)))
        return {};
    RemoteIdMessages out{};
    out.transport = RemoteIdTransport::wifi_nan;
    const uint8_t *descriptor = nullptr, *extension = nullptr;
    for (size_t at = sizeof(header); at < size;) {
        if (size - at < 3)
            return {};
        const size_t n = size_t(p[at + 1]) | (size_t(p[at + 2]) << 8);
        if (n > size - at - 3)
            return {};
        const uint8_t *body = p + at + 3;
        if (p[at] == 3) {
            // Only a single service descriptor is supported: no partial evidence.
            if (descriptor || n < 14 || std::memcmp(body, service, sizeof(service)) ||
                body[6] != 1 || body[7] != 0 || body[8] != 0x10 || size_t(body[9]) != n - 10 ||
                (body[11] >> 4) != 15)
                return {};
            descriptor = body;
            if (!append(body + 11, n - 11, body[10], out))
                return {};
        } else if (p[at] == 14) {
            if (extension || n != 4 || body[0] != 1 || body[1] != 0 || body[2] != 2)
                return {};
            extension = body;
        }
        at += 3 + n;
    }
    if (!descriptor || !extension || extension[3] != descriptor[10])
        return {};
    return out;
}
} // namespace
const char *remote_id_transport_name(RemoteIdTransport transport) {
    switch (transport) {
    case RemoteIdTransport::ble:
        return "ble";
    case RemoteIdTransport::wifi_beacon:
        return "wifi_beacon";
    case RemoteIdTransport::wifi_nan:
        return "wifi_nan";
    }
    return "unknown";
}
RemoteIdMessages remote_id_messages(const Observation &o) {
    RemoteIdMessages out{};
    if (o.payload_size > o.payload.size())
        return out;
    const auto *p = o.payload.data();
    if (o.radio == Radio::wifi) {
        if (o.wifi_subtype == WifiSubtype::probe_request)
            return {}; // Probe IEs are not a supported Remote ID transport.
        if (o.payload_size && p[0] == 4)
            return nan_messages(p, o.payload_size);
        out.transport = RemoteIdTransport::wifi_beacon;
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
bool is_remote_id_field(Field field) {
    return field == Field::rid_valid || field == Field::rid_message_type ||
           field == Field::rid_basic_id || field == Field::rid_basic_id_type ||
           field == Field::rid_position_valid;
}
bool remote_id_predicate(const Predicate &p, const uint8_t *m) {
    if (!m || (m[0] & 15) != 2 || (m[0] >> 4) > 5)
        return false;
    const unsigned type = m[0] >> 4;
    switch (p.field) {
    case Field::rid_valid:
        return p.value == "true";
    case Field::rid_message_type: {
        constexpr std::string_view names[]{"basic_id", "location", "authentication",
                                           "self_id",  "system",   "operator_id"};
        return p.value == names[type];
    }
    case Field::rid_basic_id_type:
        return type == 0 && p.value.size() == 1 && p.value[0] >= '1' && p.value[0] <= '4' &&
               unsigned(p.value[0] - '0') == (m[1] >> 4);
    case Field::rid_basic_id: {
        // Only serial and CAA registration types contain text. Other types may be binary.
        if (type != 0 || ((m[1] >> 4) != 1 && (m[1] >> 4) != 2) || p.value.empty() ||
            p.value.size() > 20)
            return false;
        const auto *end = std::find(m + 2, m + 22, uint8_t(0));
        if (!std::all_of(m + 2, end, [](uint8_t c) { return c >= 32 && c <= 126; }) ||
            !std::all_of(end, m + 22, [](uint8_t c) { return c == 0; }))
            return false;
        return p.value == std::string_view(reinterpret_cast<const char *>(m + 2), end - (m + 2));
    }
    case Field::rid_position_valid: {
        // Aircraft Location only, never operator/receiver coordinates. Zero pair is unavailable.
        if (type != 1 || p.value != "true")
            return false;
        auto coordinate = [&](size_t at) {
            uint32_t raw = uint32_t(m[at]) | uint32_t(m[at + 1]) << 8 | uint32_t(m[at + 2]) << 16 |
                           uint32_t(m[at + 3]) << 24;
            return raw <= 0x7fffffffu ? int64_t(raw) : int64_t(raw) - 0x100000000ll;
        };
        const auto lat = coordinate(5), lon = coordinate(9);
        return lat >= -900000000ll && lat <= 900000000ll && lon >= -1800000000ll &&
               lon <= 1800000000ll && (lat != 0 || lon != 0);
    }
    default:
        return false;
    }
}
} // namespace wiknow
