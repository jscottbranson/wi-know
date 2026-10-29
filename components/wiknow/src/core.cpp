#include "wiknow/core.hpp"
#include "wiknow/remote_id.hpp"
#include "wiknow/wifi_capture.hpp"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace wiknow {
bool FollowingDetector::observe(const Observation &o, Fix fix, double &distance_m,
                                uint64_t received_ms) {
    evidence_ = {};
    if (config_.scoring_enabled)
        return observe_scored(o, fix, distance_m, received_ms);
    if (!config_.enabled)
        return false;
    auto equal = [&](const DeviceIdentity &id) {
        return id.radio == o.radio && id.address_type == o.address_type && id.address == o.address;
    };
    for (size_t i = 0; i < ignored_count_; ++i)
        if (equal(ignored_[i]))
            return false;
    Entry *entry = nullptr, *slot = &entries_[0];
    for (auto &e : entries_) {
        if (e.used && equal(e.identity)) {
            entry = &e;
            break;
        }
        if (!e.used || (slot->used && e.last_seen < slot->last_seen))
            slot = &e;
    }
    if (entry && o.at_ms <= entry->last_seen)
        return false; // duplicate or delayed worker report
    if (!fix.valid || !std::isfinite(fix.latitude) || !std::isfinite(fix.longitude) ||
        std::abs(fix.latitude) > 90 || std::abs(fix.longitude) > 180) {
        if (entry)
            *entry = {};
        return false;
    }
    if (!entry || o.at_ms - entry->last_seen > config_.max_gap_ms) {
        entry = entry ? entry : slot;
        *entry = {};
        entry->used = true;
        entry->identity = {o.radio, o.address_type, o.address};
        entry->origin = fix;
        entry->last_seen = o.at_ms;
        return false;
    }
    entry->last_seen = o.at_ms;
    constexpr double radians = 3.14159265358979323846 / 180.0;
    double lat = (fix.latitude - entry->origin.latitude) * radians;
    double lon = (fix.longitude - entry->origin.longitude) * radians;
    double a = std::sin(lat / 2) * std::sin(lat / 2) +
               std::cos(fix.latitude * radians) * std::cos(entry->origin.latitude * radians) *
                   std::sin(lon / 2) * std::sin(lon / 2);
    distance_m = 6371000.0 * 2 * std::asin(std::sqrt(std::clamp(a, 0.0, 1.0)));
    if (distance_m < config_.distance_m) {
        entry->waiting = false;
        return false;
    }
    if (!entry->waiting) {
        entry->waiting = true;
        entry->beyond_since = o.at_ms;
    }
    if (!entry->alerted && o.at_ms - entry->beyond_since >= config_.delay_ms) {
        entry->alerted = true;
        return true;
    }
    return false;
}

bool FollowingDetector::observe_scored(const Observation &o, Fix fix, double &distance_m,
                                       uint64_t received_ms) {
    if (!config_.enabled)
        return false;
    auto equal = [&](const DeviceIdentity &id) {
        return id.radio == o.radio && id.address_type == o.address_type && id.address == o.address;
    };
    for (size_t i = 0; i < ignored_count_; ++i)
        if (equal(ignored_[i]))
            return false;
    // Arrival clock is independent of potentially delayed distributed capture times.
    if (received_ms && last_received_ && received_ms < last_received_) {
        entries_ = {};
        evidence_ = {"following_reset", "clock_discontinuity"};
    }
    if (received_ms)
        last_received_ = received_ms;
    Entry *entry = nullptr, *slot = &entries_[0];
    for (auto &e : entries_) {
        if (e.used && equal(e.identity)) {
            entry = &e;
            break;
        }
        if (!e.used || (slot->used && e.last_seen < slot->last_seen))
            slot = &e;
    }
    if (entry && o.at_ms <= entry->last_seen)
        return false;
    const char *reset = nullptr;
    if (received_ms && (o.at_ms > received_ms || received_ms - o.at_ms > config_.max_gap_ms))
        reset = "late_capture";
    else if (!fix.valid || !std::isfinite(fix.latitude) || !std::isfinite(fix.longitude) ||
             std::abs(fix.latitude) > 90 || std::abs(fix.longitude) > 180 ||
             o.timing_uncertainty_ms > config_.max_fix_age_ms ||
             (fix.at_ms > o.at_ms ? fix.at_ms - o.at_ms : o.at_ms - fix.at_ms) >
                 config_.max_fix_age_ms)
        reset = "invalid_or_stale_gps";
    auto distance = [](Fix a, Fix b) {
        constexpr double radians = 3.14159265358979323846 / 180.0;
        double lat = (a.latitude - b.latitude) * radians;
        double lon = (a.longitude - b.longitude) * radians;
        double h = std::sin(lat / 2) * std::sin(lat / 2) +
                   std::cos(a.latitude * radians) * std::cos(b.latitude * radians) *
                       std::sin(lon / 2) * std::sin(lon / 2);
        return 6371000.0 * 2 * std::asin(std::sqrt(std::clamp(h, 0.0, 1.0)));
    };
    if (!reset && entry && o.at_ms - entry->last_seen <= config_.max_gap_ms) {
        double step = distance(fix, entry->previous);
        double elapsed = fix.at_ms >= entry->previous.at_ms
                             ? double(fix.at_ms - entry->previous.at_ms) / 1000.0
                             : -1;
        if (elapsed < 0 || step > 2.0 * config_.gps_jitter_m + elapsed * config_.max_speed_mps)
            reset = "gps_jump";
    }
    if (reset) {
        if (entry) {
            evidence_ = {"following_reset", reset, entry->hits, entry->score,
                         o.at_ms - entry->first_seen};
            *entry = {};
        }
        return false;
    }
    if (!entry || o.at_ms - entry->last_seen > config_.max_gap_ms) {
        const char *reason = evidence_.reason ? evidence_.reason
                             : entry ? "continuity_gap"
                                     : (slot->used ? "capacity_replacement" : "new_encounter");
        entry = entry ? entry : slot;
        *entry = {};
        entry->used = true;
        entry->identity = {o.radio, o.address_type, o.address};
        entry->origin = entry->previous = fix;
        entry->first_seen = entry->last_seen = entry->last_hit = entry->decay_at = o.at_ms;
        entry->hits = entry->score = 1;
        evidence_ = {"following_score", reason, 1, 1, 0, 0};
        return false;
    }
    entry->last_seen = o.at_ms;
    entry->previous = fix;
    distance_m = distance(fix, entry->origin);
    const uint64_t decay = (o.at_ms - entry->decay_at) / std::max<uint32_t>(1, config_.score_decay_ms);
    entry->score -= uint32_t(std::min<uint64_t>(decay, entry->score));
    entry->decay_at += decay * std::max<uint32_t>(1, config_.score_decay_ms);
    const bool hit = o.at_ms - entry->last_hit >= config_.hit_interval_ms;
    if (hit) {
        entry->last_hit = o.at_ms;
        if (entry->hits < UINT32_MAX)
            ++entry->hits;
        if (entry->score < config_.score_threshold)
            ++entry->score;
    }
    const uint64_t duration = o.at_ms - entry->first_seen;
    const bool persistent = duration >= config_.min_duration_ms &&
                            entry->hits >= config_.min_hits &&
                            entry->score >= config_.score_threshold;
    if (hit || decay)
        evidence_ = {"following_score", decay ? "decay_and_sighting" : "sighting",
                     entry->hits,       entry->score,
                     duration,          distance_m};
    if (persistent && !entry->persistent) {
        entry->persistent = true;
        evidence_ = {"device_persistence",
                     "duration_hits_score",
                     entry->hits,
                     entry->score,
                     duration,
                     distance_m};
    }
    // Radial displacement, not accumulated path length. Both fixes may jitter.
    if (distance_m < config_.distance_m + 2.0 * config_.gps_jitter_m) {
        if (entry->waiting &&
            (!evidence_.event || std::strcmp(evidence_.event, "device_persistence") != 0))
            evidence_ = {"following_score", "inside_radius", entry->hits,
                         entry->score,      duration,        distance_m};
        entry->waiting = false;
        return false;
    }
    if (!entry->waiting) {
        entry->waiting = true;
        entry->beyond_since = o.at_ms;
        if (!evidence_.event || std::strcmp(evidence_.event, "device_persistence") != 0)
            evidence_ = {"following_score", "beyond_radius", entry->hits,
                         entry->score,      duration,        distance_m};
    }
    if (!entry->alerted && persistent && o.at_ms - entry->beyond_since >= config_.delay_ms) {
        entry->alerted = true;
        evidence_ = {"following_alert", "movement_duration_hits_score",
                     entry->hits,       entry->score,
                     duration,          distance_m};
        return true;
    }
    return false;
}

namespace {
bool hex(std::string_view text, uint8_t *out, size_t &size, size_t capacity) {
    size = 0;
    int high = -1;
    for (char c : text) {
        if (c == ':' || c == '-' || c == ' ')
            continue;
        int n = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                       : -1;
        if (n < 0)
            return false;
        if (high < 0)
            high = n;
        else {
            if (size == capacity)
                return false;
            out[size++] = uint8_t(high * 16 + n);
            high = -1;
        }
    }
    return high < 0 && size > 0;
}
void text_copy(std::array<char, 33> &out, const uint8_t *src, size_t n) {
    out.fill(0);
    n = std::min(n, out.size() - 1);
    std::copy(src, src + n, out.begin());
}
bool starts(std::string_view a, std::string_view b) {
    return !b.empty() && a.size() >= b.size() && a.substr(0, b.size()) == b;
}
std::string_view text_view(const std::array<char, 33> &text) {
    return {text.data(), size_t(std::find(text.begin(), text.end(), '\0') - text.begin())};
}
// ASCII-only case folding leaves non-ASCII UTF-8 bytes unchanged.
bool contains_ci(std::string_view text, std::string_view value) {
    if (value.empty())
        return false;
    const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
    return std::search(text.begin(), text.end(), value.begin(), value.end(),
                       [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
}
// Ranges apply to actual advertised service UUIDs, including Bluetooth-base
// 128-bit encodings, never arbitrary bytes in manufacturer/application data.
bool service_range(std::string_view value, const Observation &o) {
    uint8_t bounds[4];
    size_t size{};
    if (o.radio != Radio::ble || value.size() != 9 || value[4] != '-' ||
        !hex(value, bounds, size, 4) || size != 4)
        return false;
    const unsigned low = (bounds[0] << 8) | bounds[1];
    const unsigned high = (bounds[2] << 8) | bounds[3];
    if (low > high)
        return false;
    bool found = false;
    constexpr uint8_t base[] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00,
                                0x00, 0x80, 0x00, 0x10, 0x00, 0x00};
    for (size_t at = 0; at < o.payload_size;) {
        const size_t n = o.payload[at];
        if (!n)
            break;
        if (at + n + 1 > o.payload_size)
            return false;
        const uint8_t type = o.payload[at + 1];
        const bool short_uuid = type == 2 || type == 3 || type == 0x16;
        const bool long_uuid = type == 6 || type == 7 || type == 0x21;
        if (short_uuid || long_uuid) {
            const size_t width = short_uuid ? 2 : 16;
            const bool data = type == 0x16 || type == 0x21;
            if (data ? n - 1 < width : (n - 1) % width != 0)
                return false;
            const size_t limit = data ? width : n - 1;
            for (size_t i = 0; i < limit; i += width) {
                const auto *p = o.payload.data() + at + 2 + i;
                if (long_uuid && (!std::equal(base, base + 12, p) || p[14] || p[15]))
                    continue;
                const size_t offset = short_uuid ? 0 : 12;
                const unsigned id = p[offset] | (p[offset + 1] << 8);
                found = found || (id >= low && id <= high);
            }
        }
        at += n + 1;
    }
    return found;
}
// Canonical UUID text uses display order; BLE transmits all 16 octets reversed.
bool uuid128(std::string_view text, uint8_t *bytes) {
    if (text.size() != 36)
        return false;
    for (size_t i = 0; i < text.size(); ++i) {
        const bool separator = i == 8 || i == 13 || i == 18 || i == 23;
        const char c = text[i];
        if (separator
                ? c != '-'
                : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    size_t size{};
    return hex(text, bytes, size, 16) && size == 16;
}
bool service_uuid128(std::string_view value, const Observation &o) {
    uint8_t bytes[16];
    if (o.radio != Radio::ble || !uuid128(value, bytes))
        return false;
    bool found = false;
    for (size_t at = 0; at < o.payload_size;) {
        const size_t n = o.payload[at];
        if (!n)
            break; // AD terminator, as in parse_ble().
        if (at + 1 + n > o.payload_size)
            return false;
        const uint8_t type = o.payload[at + 1];
        const auto *data = o.payload.data() + at + 2;
        if (type == 0x06 || type == 0x07 || type == 0x21) {
            const size_t length = n - 1;
            if (type == 0x21 ? length < 16 : length % 16 != 0)
                return false; // Reject a partial entry even after a complete match.
            const size_t limit = type == 0x21 ? 16 : length;
            for (size_t i = 0; i < limit; i += 16)
                found = std::equal(bytes, bytes + 16,
                                   std::reverse_iterator<const uint8_t *>(data + i + 16)) ||
                        found;
        }
        at += n + 1;
    }
    return found;
}
bool valid_ble_payload(const uint8_t *data, size_t length) {
    if (!data || length > max_payload)
        return false;
    for (size_t at = 0; at < length;) {
        const size_t n = data[at];
        if (!n)
            break; // AD terminator; remaining captured bytes are still raw evidence.
        if (n > length - at - 1)
            return false;
        at += n + 1;
    }
    return true;
}
bool ble_payload_contains(std::string_view value, const Observation &o) {
    if (o.radio != Radio::ble || value.empty() || value.size() % 2 ||
        value.size() > max_ble_payload_pattern * 2 ||
        !valid_ble_payload(o.payload.data(), o.payload_size))
        return false;
    // Unlike legacy hex predicates, raw patterns require contiguous byte pairs.
    for (const char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    uint8_t bytes[max_ble_payload_pattern];
    size_t size{};
    if (!hex(value, bytes, size, sizeof(bytes)))
        return false;
    const auto end = o.payload.begin() + o.payload_size;
    // Raw contiguous evidence may cross AD boundaries; never examine metadata
    // or unused capacity, and never join separate observations.
    return std::search(o.payload.begin(), end, bytes, bytes + size) != end;
}
bool predicate(const Predicate &p, const Observation &o) {
    uint8_t bytes[32]{};
    size_t size{};
    switch (p.field) {
    case Field::rid_valid:
    case Field::rid_message_type:
    case Field::rid_basic_id:
    case Field::rid_basic_id_type:
    case Field::rid_position_valid:
        return false; // Evaluated together against one decoded message in matches().
    case Field::wifi_receiver_mac:
    case Field::wifi_receiver_oui: {
        WifiAddresses addresses{};
        if (!wifi_addresses(o, addresses) || (addresses.receiver[0] & 1) ||
            std::all_of(addresses.receiver.begin(), addresses.receiver.end(),
                        [](uint8_t b) { return b == 0; }))
            return false;
        if (p.field == Field::wifi_receiver_oui) {
            if (addresses.receiver[0] & 2)
                return false;
            return hex(p.value, bytes, size, 3) && size == 3 &&
                   std::equal(bytes, bytes + 3, addresses.receiver.begin());
        }
        if (p.value.size() != 17)
            return false;
        for (size_t i = 2; i < p.value.size(); i += 3)
            if (p.value[i] != ':')
                return false;
        return hex(p.value, bytes, size, 6) && size == 6 &&
               std::equal(bytes, bytes + 6, addresses.receiver.begin());
    }
    case Field::wifi_subtype:
        if (o.radio != Radio::wifi)
            return false;
        return (p.value == "probe_request" && o.wifi_subtype == WifiSubtype::probe_request) ||
               (p.value == "probe_response" && o.wifi_subtype == WifiSubtype::probe_response) ||
               (p.value == "beacon" && o.wifi_subtype == WifiSubtype::beacon) ||
               (p.value == "nan_action" && o.wifi_subtype == WifiSubtype::nan_action) ||
               (p.value == "data" && o.wifi_subtype == WifiSubtype::data) ||
               (p.value == "qos_data" && o.wifi_subtype == WifiSubtype::qos_data);
    case Field::wifi_ssid_wildcard:
    case Field::wifi_requested_ssid_prefix:
    case Field::wifi_ie_signature:
    case Field::wifi_ie_fingerprint: {
        if (o.radio != Radio::wifi || o.wifi_subtype != WifiSubtype::probe_request ||
            !valid_probe_ies(o.payload.data(), o.payload_size))
            return false;
        if (p.field == Field::wifi_requested_ssid_prefix) {
            for (size_t at = 0; at < o.payload_size; at += 2 + o.payload[at + 1])
                if (o.payload[at] == 0)
                    return starts({reinterpret_cast<const char *>(o.payload.data() + at + 2),
                                   o.payload[at + 1]},
                                  p.value);
            return false;
        }
        if (p.field == Field::wifi_ssid_wildcard) {
            for (size_t at = 0; at < o.payload_size; at += 2 + o.payload[at + 1])
                if (o.payload[at] == 0)
                    return p.value == (o.payload[at + 1] == 0 ? "true" : "false");
            return false; // A missing SSID is neither directed nor wildcard.
        }
        if (p.field == Field::wifi_ie_signature) {
            // Ordered tag IDs; vendor IDs include up to eight leading body bytes.
            // The existing strict IE validator rejects malformed or duplicate SSIDs.
            auto remaining = p.value;
            bool matched = false;
            for (size_t at = 0; at < o.payload_size; at += 2 + o.payload[at + 1]) {
                const auto tag = o.payload[at];
                if (tag == 0)
                    continue;
                const size_t comma = remaining.find(',');
                const auto token = remaining.substr(0, comma);
                char expected[24]{};
                size_t length =
                    size_t(std::snprintf(expected, sizeof(expected), "%u", unsigned(tag)));
                if (tag == 221) {
                    expected[length++] = ':';
                    constexpr char digits[] = "0123456789abcdef";
                    for (size_t i = 0; i < std::min<size_t>(8, o.payload[at + 1]); ++i) {
                        const auto byte = o.payload[at + 2 + i];
                        expected[length++] = digits[byte >> 4];
                        expected[length++] = digits[byte & 15];
                    }
                }
                if (token != std::string_view(expected, length))
                    return false;
                matched = true;
                remaining = comma == std::string_view::npos ? std::string_view{}
                                                            : remaining.substr(comma + 1);
            }
            return matched && remaining.empty();
        }
        // Full canonical IE bytes, not a hash or a substring. No variable bytes normalized.
        if (p.value.size() != size_t(o.payload_size) * 2 || p.value.empty())
            return false;
        for (size_t at = 0; at < o.payload_size; ++at) {
            size_t n{};
            uint8_t byte{};
            if (!hex(p.value.substr(at * 2, 2), &byte, n, 1) || n != 1 || byte != o.payload[at])
                return false;
        }
        return true;
    }
    case Field::ssid_prefix:
        return o.wifi_subtype != WifiSubtype::probe_request && starts(text_view(o.ssid), p.value);
    case Field::ssid_contains_ci:
        return o.radio == Radio::wifi && o.wifi_subtype != WifiSubtype::probe_request &&
               contains_ci(text_view(o.ssid), p.value);
    case Field::name_contains_ci:
        return o.radio == Radio::ble && contains_ci(text_view(o.name), p.value);
    case Field::name_exact:
        return o.radio == Radio::ble && text_view(o.name) == p.value;
    case Field::service_uuid16_range:
        return service_range(p.value, o);
    case Field::name_prefix:
        return starts(text_view(o.name), p.value);
    case Field::mac:
        // Strict six-octet colon notation, also enforced by the JSON validator.
        // Unlike OUI inference, exact address evidence is valid for local/random MACs.
        if (p.value.size() != 17)
            return false;
        for (size_t i = 0; i < p.value.size(); ++i) {
            const char c = p.value[i];
            if (i % 3 == 2
                    ? c != ':'
                    : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        }
        return hex(p.value, bytes, size, 6) && size == 6 &&
               std::equal(bytes, bytes + 6, o.address.begin());
    case Field::oui:
        if ((o.radio == Radio::ble && o.address_type != 0) ||
            (o.radio == Radio::wifi && (o.address[0] & 2)))
            return false;
        return hex(p.value, bytes, size, 3) && size == 3 &&
               std::equal(bytes, bytes + 3, o.address.begin());
    case Field::service_uuid128:
        return service_uuid128(p.value, o);
    case Field::ble_payload_contains:
        return ble_payload_contains(p.value, o);
    case Field::manufacturer_prefix:
    case Field::service_uuid16:
        if (o.radio != Radio::ble || !hex(p.value, bytes, size, sizeof(bytes)))
            return false;
        for (size_t at = 0; at < o.payload_size;) {
            size_t n = o.payload[at];
            if (!n)
                break;
            if (at + 1 + n > o.payload_size)
                return false;
            uint8_t type = o.payload[at + 1];
            const auto *data = o.payload.data() + at + 2;
            if (p.field == Field::manufacturer_prefix && type == 0xff && n - 1 >= size &&
                std::equal(bytes, bytes + size, data))
                return true;
            if (p.field == Field::service_uuid16 && size == 2 &&
                (type == 2 || type == 3 || type == 0x16)) {
                size_t limit = type == 0x16 ? std::min<size_t>(2, n - 1) : n - 1;
                for (size_t i = 0; i + 1 < limit; i += 2)
                    if (data[i] == bytes[1] && data[i + 1] == bytes[0])
                        return true;
            }
            at += 1 + n;
        }
        return false;
    }
    return false;
}
uint16_t get16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
uint32_t get32(const uint8_t *p) {
    return uint32_t(get16(p)) | uint32_t(get16(p + 2)) << 16;
}
void put16(uint8_t *p, uint16_t n) {
    p[0] = uint8_t(n);
    p[1] = uint8_t(n >> 8);
}
void put32(uint8_t *p, uint32_t n) {
    put16(p, uint16_t(n));
    put16(p + 2, uint16_t(n >> 16));
}
uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffff;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int j = 0; j < 8; ++j)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}
uint64_t difference(uint64_t a, uint64_t b) {
    return a > b ? a - b : b - a;
}
bool number(const char *s, double &value) {
    if (!s || !*s)
        return false;
    char *end{};
    value = std::strtod(s, &end);
    return *end == 0 && std::isfinite(value);
}
bool coordinate(const char *s, const char *direction, bool latitude, double &out) {
    double raw{};
    if (!number(s, raw) || raw < 0 || raw > (latitude ? 9000 : 18000) ||
        std::strlen(direction) != 1)
        return false;
    int degrees = int(raw / 100);
    double minutes = raw - degrees * 100;
    if (minutes >= 60 || degrees > (latitude ? 90 : 180))
        return false;
    out = degrees + minutes / 60;
    if (out > (latitude ? 90 : 180))
        return false;
    if (*direction == (latitude ? 'S' : 'W'))
        out = -out;
    else if (*direction != (latitude ? 'N' : 'E'))
        return false;
    return true;
}
int64_t days_since_epoch(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned y = unsigned(year - era * 400);
    const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    return int64_t(era) * 146097 + int64_t(y * 365 + y / 4 - y / 100 + doy) - 719468;
}
bool utc_value(const char *date, const char *time, int64_t &out) {
    if (std::strlen(date) != 6 || std::strlen(time) < 6)
        return false;
    for (int i = 0; i < 6; ++i)
        if (date[i] < '0' || date[i] > '9' || time[i] < '0' || time[i] > '9')
            return false;
    auto two = [](const char *p) { return (p[0] - '0') * 10 + p[1] - '0'; };
    int day = two(date), month = two(date + 2), year = 2000 + two(date + 4);
    int hour = two(time), minute = two(time + 2);
    double seconds{};
    if (!number(time + 4, seconds) || hour > 23 || minute > 59 || seconds < 0 || seconds >= 60 ||
        month < 1 || month > 12)
        return false;
    const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int limit =
        lengths[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    if (day < 1 || day > limit)
        return false;
    out = (days_since_epoch(year, unsigned(month), unsigned(day)) * 86400 + hour * 3600 +
           minute * 60) *
              1000 +
          int64_t(std::llround(seconds * 1000));
    return true;
}
struct Json {
    char *out;
    size_t capacity, used{};
    bool ok{true};
    void add(const char *format, ...) {
        if (!ok)
            return;
        va_list args;
        va_start(args, format);
        int n = std::vsnprintf(out + used, capacity - used, format, args);
        va_end(args);
        if (n < 0 || size_t(n) >= capacity - used) {
            ok = false;
            return;
        }
        used += size_t(n);
    }
    // Keep valid UTF-8 readable; escape individual invalid bytes so radio input
    // can never produce malformed JSON or be confused with a literal '?'.
    void string(std::string_view s) {
        add("\"");
        for (size_t i = 0; i < s.size(); ++i) {
            const auto c = static_cast<unsigned char>(s[i]);
            if (c == '"' || c == '\\')
                add("\\%c", c);
            else if (c < 32)
                add("\\u%04x", unsigned(c));
            else if (c < 128)
                add("%c", c);
            else {
                size_t n = c >= 0xc2 && c <= 0xdf   ? 2
                           : c >= 0xe0 && c <= 0xef ? 3
                           : c >= 0xf0 && c <= 0xf4 ? 4
                                                    : 0;
                bool valid = n && i + n <= s.size();
                for (size_t k = 1; valid && k < n; ++k)
                    valid = (static_cast<unsigned char>(s[i + k]) & 0xc0) == 0x80;
                if (valid) {
                    const auto second = static_cast<unsigned char>(s[i + 1]);
                    valid = !(c == 0xe0 && second < 0xa0) && !(c == 0xed && second >= 0xa0) &&
                            !(c == 0xf0 && second < 0x90) && !(c == 0xf4 && second >= 0x90);
                }
                if (valid) {
                    for (size_t k = 0; k < n; ++k)
                        add("%c", static_cast<unsigned char>(s[i + k]));
                    i += n - 1;
                } else
                    add("\\u%04x", unsigned(c));
            }
        }
        add("\"");
    }
    void fix(Fix f) {
        if (!f.valid || !std::isfinite(f.latitude) || !std::isfinite(f.longitude) ||
            std::abs(f.latitude) > 90 || std::abs(f.longitude) > 180)
            add("null");
        else
            add("{\"latitude\":%.7f,\"longitude\":%.7f,\"fix_at_ms\":%llu}", f.latitude,
                f.longitude, (unsigned long long)f.at_ms);
    }
};
} // namespace

bool valid_probe_ies(const uint8_t *data, size_t length) {
    if (!data || length > max_payload)
        return false;
    bool ssid = false;
    for (size_t at = 0; at < length;) {
        if (length - at < 2 || size_t(data[at + 1]) > length - at - 2)
            return false;
        const size_t n = data[at + 1];
        if (data[at] == 0) {
            if (ssid || n > 32)
                return false;
            ssid = true;
        }
        // Vendor/extension IEs must include their identifying bytes.
        if ((data[at] == 221 && n < 4) || (data[at] == 255 && n < 1))
            return false;
        at += 2 + n;
    }
    return true;
}
bool wifi_addresses(const Observation &o, WifiAddresses &out) {
    out = {};
    if (o.radio != Radio::wifi || o.address_type != 0 || !o.wifi_header_size)
        return false;
    const auto *h = o.wifi_header.data();
    const bool data =
        o.wifi_subtype == WifiSubtype::data || o.wifi_subtype == WifiSubtype::qos_data;
    if (data) {
        if (wifi_data_header_size(h, o.wifi_header_size) != o.wifi_header_size ||
            o.wifi_header_size > o.wifi_header.size() ||
            o.wifi_subtype != (h[0] == 0x08 ? WifiSubtype::data : WifiSubtype::qos_data) ||
            o.payload_size || o.ssid[0] || o.name[0])
            return false;
    } else if (o.wifi_header_size != 24 || (h[0] & 15) || (h[1] & 0xc7) || (h[22] & 15) ||
               uint8_t(o.wifi_subtype) != (h[0] >> 4) ||
               (o.wifi_subtype != WifiSubtype::probe_request &&
                o.wifi_subtype != WifiSubtype::probe_response &&
                o.wifi_subtype != WifiSubtype::beacon && o.wifi_subtype != WifiSubtype::nan_action))
        return false;
    if (!std::equal(h + 10, h + 16, o.address.begin()))
        return false;
    auto copy = [](std::array<uint8_t, 6> &to, const uint8_t *from) {
        std::copy(from, from + 6, to.begin());
    };
    copy(out.receiver, h + 4);
    copy(out.transmitter, h + 10);
    const uint8_t ds = h[1] & 3;
    copy(out.destination, h + (ds == 1 ? 16 : 4));
    copy(out.source, h + (ds == 2 ? 16 : 10));
    copy(out.bssid, h + (ds == 1 ? 4 : ds == 2 ? 10 : 16));
    return true;
}
bool parse_wifi(const uint8_t *frame, size_t length, Observation &out) {
    // Clear parse-owned state even when an Observation is reused after a failed parse.
    out.wifi_subtype = WifiSubtype::unknown;
    out.wifi_header_size = 0;
    out.wifi_header.fill(0);
    out.ssid.fill(0);
    out.name.fill(0);
    out.payload_size = 0;
    if (frame && length >= 24 && (frame[0] & 0x0c) == 8) {
        const size_t header = wifi_data_header_size(frame, length);
        if (!header)
            return false;
        out.radio = Radio::wifi;
        out.address_type = 0;
        out.wifi_subtype = frame[0] == 0x08 ? WifiSubtype::data : WifiSubtype::qos_data;
        std::copy(frame + 10, frame + 16, out.address.begin());
        std::copy(frame, frame + header, out.wifi_header.begin());
        out.wifi_header_size = uint8_t(header);
        return true;
    }
    if (!frame || length < 24 || length > 512 || (frame[0] & 15) || (frame[1] & 0xc7) ||
        (frame[22] & 15))
        return false; // No DS, fragmentation, protection, or HT-control headers.
    const auto subtype = WifiSubtype(frame[0] >> 4);
    if (subtype != WifiSubtype::probe_request && subtype != WifiSubtype::probe_response &&
        subtype != WifiSubtype::beacon && subtype != WifiSubtype::nan_action &&
        subtype != WifiSubtype::data && subtype != WifiSubtype::qos_data)
        return false;
    out.radio = Radio::wifi;
    out.address_type = 0;
    std::copy(frame + 10, frame + 16, out.address.begin());
    if (subtype == WifiSubtype::nan_action) {
        constexpr uint8_t destination[]{0x51, 0x6f, 0x9a, 1, 0, 0};
        if (length < 30 || std::memcmp(frame + 4, destination, sizeof(destination)) ||
            length - 24 > max_payload)
            return false;
        out.payload_size = uint16_t(length - 24);
        std::copy(frame + 24, frame + length, out.payload.begin());
        if (!remote_id_messages(out).count) {
            out.payload_size = 0;
            return false;
        }
        out.wifi_subtype = subtype;
        std::copy(frame, frame + 24, out.wifi_header.begin());
        out.wifi_header_size = 24;
        return true;
    }
    const bool probe = subtype == WifiSubtype::probe_request;
    const size_t start = probe ? 24 : 36;
    if (length < start || (probe && !valid_probe_ies(frame + start, length - start)))
        return false;
    bool ssid = false;
    for (size_t at = start; at < length;) {
        if (length - at < 2 || size_t(frame[at + 1]) > length - at - 2)
            return false;
        size_t n = frame[at + 1];
        if (frame[at] == 0) {
            if (ssid || n > 32)
                return false;
            ssid = true;
            text_copy(out.ssid, frame + at + 2, n);
        }
        if (!probe && frame[at] == 221 && n >= 4 && frame[at + 2] == 0xfa &&
            frame[at + 3] == 0x0b && frame[at + 4] == 0xbc && frame[at + 5] == 0x0d) {
            if (out.payload_size)
                return false;
            std::copy(frame + at + 2, frame + at + 2 + n, out.payload.begin());
            out.payload_size = uint16_t(n);
        }
        at += 2 + n;
    }
    if (probe) {
        out.payload_size = uint16_t(length - start);
        std::copy(frame + start, frame + length, out.payload.begin());
    }
    out.wifi_subtype = subtype;
    std::copy(frame, frame + 24, out.wifi_header.begin());
    out.wifi_header_size = 24;
    return true;
}
bool parse_ble(const uint8_t *data, size_t length, Observation &out) {
    out.wifi_subtype = WifiSubtype::unknown;
    out.wifi_header_size = 0;
    out.wifi_header.fill(0);
    out.ssid.fill(0);
    out.radio = Radio::ble;
    out.payload_size = 0;
    out.name.fill(0);
    if (!valid_ble_payload(data, length))
        return false;
    out.payload_size = uint16_t(length);
    std::copy(data, data + length, out.payload.begin());
    bool complete = false;
    for (size_t at = 0; at < length;) {
        size_t n = data[at];
        if (!n)
            break;
        if (at + 1 + n > length)
            return false;
        if (data[at + 1] == 9 || (data[at + 1] == 8 && !complete)) {
            text_copy(out.name, data + at + 2, n - 1);
            complete = data[at + 1] == 9;
        }
        at += n + 1;
    }
    return true;
}
bool matches(const Rule &r, const Observation &o) {
    if (o.payload_size > o.payload.size() || !r.enabled || r.radio != o.radio ||
        !r.predicate_count || r.predicate_count > r.predicates.size())
        return false;
    bool has_rid = false;
    for (size_t i = 0; i < r.predicate_count; ++i) {
        if (is_remote_id_field(r.predicates[i].field)) {
            has_rid = true;
            continue;
        }
        const bool hit = predicate(r.predicates[i], o);
        if (r.require_all && !hit)
            return false;
        if (!r.require_all && hit)
            return true;
    }
    if (!has_rid)
        return r.require_all;
    const auto decoded = remote_id_messages(o);
    for (size_t message = 0; message < decoded.count; ++message) {
        bool all = true;
        for (size_t i = 0; i < r.predicate_count; ++i) {
            const auto &p = r.predicates[i];
            if (!is_remote_id_field(p.field))
                continue;
            const bool hit = remote_id_predicate(p, decoded.messages[message]);
            if (!r.require_all && hit)
                return true;
            all = all && hit;
        }
        if (r.require_all && all)
            return true;
    }
    return false;
}
Match best_match(const Rule *rules, size_t count, const Observation &o) {
    Match best{};
    for (size_t i = 0; i < count; ++i)
        if (matches(rules[i], o) && (!best.rule || rules[i].priority > best.rule->priority))
            best.rule = &rules[i];
    return best;
}
bool indirect_rule(const Rule &rule) {
    for (size_t i = 0; i < std::min(rule.predicate_count, rule.predicates.size()); ++i)
        if (rule.predicates[i].field == Field::wifi_receiver_mac ||
            rule.predicates[i].field == Field::wifi_receiver_oui)
            return true;
    return false;
}
Match best_direct_match(const Rule *rules, size_t count, const Observation &o) {
    Match best{};
    for (size_t i = 0; i < count; ++i)
        if (!indirect_rule(rules[i]) && matches(rules[i], o) &&
            (!best.rule || rules[i].priority > best.rule->priority))
            best.rule = &rules[i];
    return best;
}
bool Membership::seen(Node node, uint64_t now) {
    if (!node.id)
        return false;
    Node *slot = nullptr;
    for (auto &n : nodes_)
        if (n.id == node.id) {
            slot = &n;
            break;
        }
    if (!slot)
        for (auto &n : nodes_)
            if (!n.online) {
                slot = &n;
                break;
            }
    if (!slot)
        return false;
    bool changed = !slot->online || slot->id != node.id || slot->boot != node.boot ||
                   slot->role != node.role || slot->band24 != node.band24 ||
                   slot->band5 != node.band5;
    node.online = true;
    node.last_seen = now;
    *slot = node;
    return changed;
}
bool Membership::expire(uint64_t now, uint32_t lease) {
    bool changed = false;
    for (auto &n : nodes_)
        if (n.online && now >= n.last_seen && now - n.last_seen >= lease) {
            n.online = false;
            changed = true;
        }
    return changed;
}
Plan Membership::plan(const Channel *channels, size_t count) const {
    Plan p{};
    std::array<Node, max_nodes> sorted = nodes_;
    std::sort(sorted.begin(), sorted.end(),
              [](const Node &a, const Node &b) { return a.id < b.id; });
    for (const auto &n : sorted)
        if (n.online && n.role == Role::child)
            p.assignments[p.count++].node = n.id;
    // Constrained channels first avoids wasting scarce band-specific capacity.
    std::array<Channel, max_channels> work{};
    count = std::min(count, max_channels);
    std::copy(channels, channels + count, work.begin());
    auto capable = [&](uint16_t id, Channel c) {
        for (const auto &n : nodes_)
            if (n.id == id)
                return c.number <= 14 ? n.band24 : n.band5;
        return false;
    };
    auto eligible = [&](Channel c) {
        size_t k = 0;
        for (size_t i = 0; i < p.count; ++i)
            k += capable(p.assignments[i].node, c);
        return k;
    };
    // Stable insertion sort is bounded to 32 channels and never allocates.
    for (size_t i = 1; i < count; ++i) {
        Channel value = work[i];
        size_t j = i;
        while (j && eligible(value) < eligible(work[j - 1])) {
            work[j] = work[j - 1];
            --j;
        }
        work[j] = value;
    }
    for (size_t c = 0; c < count; ++c) {
        size_t best = p.count;
        for (size_t i = 0; i < p.count; ++i)
            if (capable(p.assignments[i].node, work[c]) &&
                (best == p.count || p.assignments[i].count < p.assignments[best].count))
                best = i;
        if (best == p.count)
            p.uncovered[p.uncovered_count++] = work[c];
        else {
            auto &a = p.assignments[best];
            a.channels[a.count++] = work[c];
        }
    }
    return p;
}
bool Gps::feed(char byte, uint64_t now) {
    if (byte == '$') {
        length_ = 0;
        collecting_ = true;
        return false;
    }
    if (!collecting_)
        return false;
    if (byte == '\r')
        return false;
    if (byte == '\n') {
        collecting_ = false;
        line_[length_] = 0;
        return sentence(now);
    }
    if (length_ + 1 >= line_.size()) {
        collecting_ = false;
        length_ = 0;
        return false;
    }
    line_[length_++] = byte;
    return false;
}
bool Gps::sentence(uint64_t now) {
    char *star = std::strchr(line_.data(), '*');
    if (!star || std::strlen(star + 1) != 2)
        return false;
    uint8_t expected[1]{};
    size_t size{};
    if (!hex(star + 1, expected, size, 1))
        return false;
    uint8_t check = 0;
    for (char *p = line_.data(); p < star; ++p)
        check ^= uint8_t(*p);
    if (check != expected[0])
        return false;
    *star = 0;
    std::array<char *, 20> fields{};
    size_t count = 1;
    fields[0] = line_.data();
    for (char *p = line_.data(); *p; ++p)
        if (*p == ',') {
            *p = 0;
            if (count == fields.size())
                return false;
            fields[count++] = p + 1;
        }
    if (std::strlen(fields[0]) != 5 || std::strcmp(fields[0] + 2, "RMC") || count < 10 ||
        std::strcmp(fields[2], "A"))
        return false;
    int64_t epoch{};
    if (!utc_value(fields[9], fields[1], epoch))
        return false;
    has_time_ = true;
    time_at_ = now;
    utc_at_ = epoch;
    Fix fix{};
    fix.at_ms = now;
    fix.valid = coordinate(fields[3], fields[4], true, fix.latitude) &&
                coordinate(fields[5], fields[6], false, fix.longitude);
    if (fix.valid) {
        fixes_[next_fix_] = fix;
        next_fix_ = (next_fix_ + 1) % fixes_.size();
    }
    return true;
}
Fix Gps::nearest(uint64_t capture, uint32_t max_age) const {
    Fix best{};
    uint64_t delta = UINT64_MAX;
    for (auto f : fixes_)
        if (f.valid) {
            uint64_t d = difference(capture, f.at_ms);
            if (d <= max_age && d < delta) {
                best = f;
                delta = d;
            }
        }
    return best;
}
Fix Gps::tag(const Observation &o, uint32_t max_age, uint32_t max_uncertainty) const {
    if (o.timing_uncertainty_ms > max_uncertainty || o.timing_uncertainty_ms > max_age)
        return {};
    // The entire possible capture-time interval must be within the fix-age limit.
    return nearest(o.at_ms, max_age - o.timing_uncertainty_ms);
}
bool Gps::utc(uint64_t at, int64_t &out) const {
    if (!has_time_)
        return false;
    // Avoid unsigned wrap when associating buffered observations before last sync.
    out = utc_at_ + (at >= time_at_ ? int64_t(at - time_at_) : -int64_t(time_at_ - at));
    return true;
}
bool same_signal(const Observation &a, const Observation &b) {
    return a.node == b.node && a.boot == b.boot && a.radio == b.radio &&
           a.address_type == b.address_type && a.address == b.address && a.channel == b.channel;
}
Track &Tracker::observe(const Observation &o, Fix fix) {
    Track *t = nullptr;
    for (auto &x : tracks_)
        if (x.used && same_signal(x.latest, o)) {
            t = &x;
            break;
        }
    if (!t)
        for (auto &x : tracks_)
            if (!x.used) {
                t = &x;
                break;
            }
    if (!t)
        t = &*std::min_element(tracks_.begin(), tracks_.end(), [](const Track &a, const Track &b) {
            return a.latest.at_ms < b.latest.at_ms;
        });
    if (!t->used || !same_signal(t->latest, o)) {
        *t = {};
        t->used = true;
        t->first_seen = o.at_ms;
        t->window_start = o.at_ms;
        t->peak = o;
        t->peak_fix = fix;
        t->peak_changed = true;
    } else
        t->peak_changed = false;
    // Priority queues and transport delays can reorder valid captures. Preserve
    // their historical evidence without feeding them into current trend windows.
    t->first_seen = std::min(t->first_seen, o.at_ms);
    ++t->count;
    if (o.rssi > t->peak.rssi) {
        t->peak = o;
        t->peak_fix = fix;
        t->peak_changed = true;
    }
    if (o.at_ms < t->latest.at_ms)
        return *t;
    if (t->count && o.at_ms - t->latest.at_ms > config_.stale_ms) {
        t->sample_count = 0;
        t->rssi_histogram.fill(0);
        t->previous_valid = false;
        t->trend = Trend::unknown;
        t->candidate = Trend::unknown;
        t->confirmations = 0;
        t->window_start = o.at_ms;
    }
    if (config_.window_ms && o.at_ms - t->window_start >= config_.window_ms) {
        if (t->sample_count >= config_.minimum_samples && config_.minimum_samples) {
            size_t accumulated = 0;
            int median = -128;
            for (size_t i = 0; i < t->rssi_histogram.size(); ++i) {
                accumulated += t->rssi_histogram[i];
                if (accumulated > t->sample_count / 2) {
                    median = int(i) - 127;
                    break;
                }
            }
            if (t->previous_valid) {
                int delta = median - t->previous_median;
                Trend next = delta > config_.deadband_db    ? Trend::increasing
                             : delta < -config_.deadband_db ? Trend::decreasing
                                                            : Trend::steady;
                if (next == t->candidate) {
                    if (t->confirmations < 255)
                        ++t->confirmations;
                } else {
                    t->candidate = next;
                    t->confirmations = 1;
                }
                if (t->confirmations >= config_.confirmations)
                    t->trend = next;
            }
            t->previous_valid = true;
            t->previous_median = median;
        } else {
            t->previous_valid = false;
            t->trend = Trend::unknown;
            t->candidate = Trend::unknown;
            t->confirmations = 0;
        }
        t->sample_count = 0;
        t->rssi_histogram.fill(0);
        t->window_start = o.at_ms;
    }
    if (o.rssi >= -127 && o.rssi <= 0 && t->sample_count < UINT32_MAX) {
        ++t->rssi_histogram[size_t(int(o.rssi) + 127)];
        ++t->sample_count;
    }
    t->latest = o;
    return *t;
}
void Tracker::reset_trend(Track &t) {
    t.sample_count = 1;
    t.rssi_histogram.fill(0);
    ++t.rssi_histogram[size_t(std::clamp(int(t.latest.rssi), -127, 0) + 127)];
    t.window_start = t.latest.at_ms;
    t.previous_valid = false;
    t.trend = t.candidate = Trend::unknown;
    t.confirmations = 0;
}
Trend Tracker::trend(const Track &t, uint64_t now) const {
    return !t.used || now < t.latest.at_ms || now - t.latest.at_ms > config_.stale_ms
               ? Trend::unknown
               : t.trend;
}
const char *trend_name(Trend t) {
    switch (t) {
    case Trend::increasing:
        return "increasing";
    case Trend::decreasing:
        return "decreasing";
    case Trend::steady:
        return "steady";
    default:
        return "unknown";
    }
}

size_t encode_packet(const Packet &p, uint8_t *out, size_t cap) {
    size_t n = 20 + p.size;
    if (p.size > p.payload.size() || cap < n)
        return 0;
    out[0] = 0x57;
    out[1] = 0x4b;
    out[2] = protocol_version;
    out[3] = uint8_t(p.type);
    put16(out + 4, p.node);
    put32(out + 6, p.boot);
    put32(out + 10, p.sequence);
    put16(out + 14, p.size);
    std::copy(p.payload.begin(), p.payload.begin() + p.size, out + 16);
    put32(out + 16 + p.size, crc(out + 2, 14 + p.size));
    return n;
}
bool Decoder::feed(uint8_t byte, Packet &out) {
    if (size_ == 0 && byte != 0x57)
        return false;
    if (size_ == 1 && byte != 0x4b) {
        size_ = byte == 0x57 ? 1 : 0;
        return false;
    }
    data_[size_++] = byte;
    if (size_ < 16)
        return false;
    size_t payload = get16(data_.data() + 14);
    if (data_[2] != protocol_version || data_[3] < 1 || data_[3] > 8 || payload > 512) {
        ++errors_;
        size_ = 0;
        return false;
    }
    if (size_ < 20 + payload)
        return false;
    size_ = 0;
    if (get32(data_.data() + 16 + payload) != crc(data_.data() + 2, 14 + payload)) {
        ++errors_;
        return false;
    }
    out.type = MessageType(data_[3]);
    out.node = get16(data_.data() + 4);
    out.boot = get32(data_.data() + 6);
    out.sequence = get32(data_.data() + 10);
    out.size = uint16_t(payload);
    std::copy(data_.begin() + 16, data_.begin() + 16 + payload, out.payload.begin());
    return true;
}
bool RetainedObservation::retain(const Observation &observation) {
    if (pending_)
        return false;
    observation_ = observation;
    pending_ = true;
    return true;
}
bool RetainedObservation::acknowledge(ObservationAck ack) {
    if (!pending_ || !ack.valid || ack.boot != observation_.boot ||
        ack.sequence != observation_.sequence)
        return false;
    pending_ = false;
    return true;
}
void RetainedObservation::report(Report &out, uint64_t now) const {
    out.has_observation = pending_;
    if (pending_) {
        out.observation = observation_;
        out.capture_age_ms = uint32_t(std::min<uint64_t>(
            UINT32_MAX, now >= observation_.at_ms ? now - observation_.at_ms : 0));
    }
}
bool ReportReceipt::accept(const Observation &observation) {
    if (ack_.valid && ack_.boot == observation.boot && ack_.sequence == observation.sequence)
        return false;
    ack_ = {true, observation.boot, observation.sequence};
    return true;
}
bool pack_command(const Command &c, Packet &p) {
    if (c.count > max_channels)
        return false;
    p.type = MessageType::poll;
    p.size = uint16_t(32 + c.count);
    auto *b = p.payload.data();
    put32(b, c.lease_ms);
    put32(b + 4, c.dwell_ms);
    b[8] = c.count;
    for (size_t i = 0; i < c.count; ++i)
        b[9 + i] = c.channels[i].number;
    size_t at = 9 + c.count;
    b[at++] = c.follow;
    b[at++] = uint8_t(c.target_radio);
    b[at++] = c.target_channel;
    b[at++] = c.target_address_type;
    std::copy(c.target.begin(), c.target.end(), b + at);
    at += c.target.size();
    b[at++] = c.ack.valid;
    put32(b + at, c.ack.boot);
    put32(b + at + 4, c.ack.sequence);
    put32(b + at + 8, c.assignment_id);
    return true;
}
bool unpack_command(const Packet &p, Command &c) {
    if (p.type != MessageType::poll || p.size < 32)
        return false;
    const auto *b = p.payload.data();
    uint8_t n = b[8];
    if (n > max_channels || p.size != 32 + n || get32(b) < 100 || get32(b) > 60000 ||
        get32(b + 4) < 10 || get32(b + 4) > 60000)
        return false;
    c = {};
    c.lease_ms = get32(b);
    c.dwell_ms = get32(b + 4);
    c.count = n;
    for (size_t i = 0; i < n; ++i) {
        if (!b[9 + i])
            return false;
        c.channels[i].number = b[9 + i];
    }
    size_t at = 9 + n;
    if (b[at] > 1 || b[at + 1] > 1 || b[at + 3] > 1)
        return false;
    c.follow = b[at++];
    c.target_radio = Radio(b[at++]);
    c.target_channel = b[at++];
    c.target_address_type = b[at++];
    std::copy(b + at, b + at + 6, c.target.begin());
    at += c.target.size();
    if (b[at] > 1)
        return false;
    c.ack.valid = b[at++];
    c.ack.boot = get32(b + at);
    c.ack.sequence = get32(b + at + 4);
    c.assignment_id = get32(b + at + 8);
    return true;
}
bool pack_report(const Report &r, Packet &p) {
    if (r.observation.payload_size > max_payload ||
        uint8_t(r.scan.state) > uint8_t(ScanState::expired) ||
        (r.scan.state != ScanState::scanning && r.scan.confirmed_channels) ||
        ((r.scan.state == ScanState::error) != (r.scan.failed_channel != 0)))
        return false;
    const auto subtype = r.observation.wifi_subtype;
    if (subtype != WifiSubtype::unknown && subtype != WifiSubtype::probe_request &&
        subtype != WifiSubtype::probe_response && subtype != WifiSubtype::beacon &&
        subtype != WifiSubtype::nan_action && subtype != WifiSubtype::data &&
        subtype != WifiSubtype::qos_data)
        return false;
    if (r.observation.radio == Radio::ble && subtype != WifiSubtype::unknown)
        return false;
    if (subtype == WifiSubtype::probe_request &&
        !valid_probe_ies(r.observation.payload.data(), r.observation.payload_size))
        return false;
    WifiAddresses addresses{};
    if ((r.observation.wifi_header_size && !wifi_addresses(r.observation, addresses)) ||
        ((subtype == WifiSubtype::data || subtype == WifiSubtype::qos_data) &&
         !r.observation.wifi_header_size))
        return false;
    p.type = MessageType::report;
    auto *b = p.payload.data();
    b[0] = uint8_t(r.role);
    b[1] = r.band24;
    b[2] = r.band5;
    b[3] = r.has_observation;
    put32(b + 4, r.dropped);
    put32(b + 8, r.scan.assignment_id);
    put32(b + 12, r.scan.confirmed_channels);
    b[16] = uint8_t(r.scan.state);
    b[17] = r.scan.failed_channel;
    p.size = 18;
    if (!r.has_observation)
        return true;
    const auto &o = r.observation;
    put32(b + 18, r.capture_age_ms);
    put32(b + 22, o.sequence);
    b[26] = uint8_t(o.radio);
    b[27] = o.address_type;
    b[28] = o.channel;
    b[29] = uint8_t(o.rssi);
    std::copy(o.address.begin(), o.address.end(), b + 30);
    std::memcpy(b + 36, o.ssid.data(), 33);
    std::memcpy(b + 69, o.name.data(), 33);
    put16(b + 102, o.payload_size);
    std::copy(o.payload.begin(), o.payload.begin() + o.payload_size, b + 132);
    b[104] = uint8_t(o.wifi_subtype);
    b[105] = o.wifi_header_size;
    std::fill(b + 106, b + 132, 0);
    std::copy(o.wifi_header.begin(), o.wifi_header.begin() + o.wifi_header_size, b + 106);
    p.size = uint16_t(132 + o.payload_size);
    return true;
}
bool unpack_report(const Packet &p, Report &r) {
    if (p.type != MessageType::report || p.size < 18)
        return false;
    const auto *b = p.payload.data();
    if (b[0] < 1 || b[0] > 2 || b[1] > 1 || b[2] > 1 || b[3] > 1 ||
        b[16] > uint8_t(ScanState::expired))
        return false;
    r = {};
    r.role = Role(b[0]);
    r.band24 = b[1];
    r.band5 = b[2];
    r.has_observation = b[3];
    r.dropped = get32(b + 4);
    r.scan.assignment_id = get32(b + 8);
    r.scan.confirmed_channels = get32(b + 12);
    r.scan.state = ScanState(b[16]);
    r.scan.failed_channel = b[17];
    if ((r.scan.state != ScanState::scanning && r.scan.confirmed_channels) ||
        ((r.scan.state == ScanState::error) != (r.scan.failed_channel != 0)))
        return false;
    if (!r.has_observation)
        return p.size == 18;
    if (p.size < 132 || get16(b + 102) > max_payload || p.size != 132 + get16(b + 102) ||
        b[26] > 1 || b[27] > 1 || b[68] != 0 || b[101] != 0)
        return false;
    auto &o = r.observation;
    o.node = p.node;
    o.boot = p.boot;
    r.capture_age_ms = get32(b + 18);
    o.sequence = get32(b + 22);
    o.radio = Radio(b[26]);
    o.address_type = b[27];
    o.channel = b[28];
    o.rssi = int8_t(b[29]);
    if (o.rssi > 0 || o.rssi < -127)
        return false;
    std::copy(b + 30, b + 36, o.address.begin());
    std::memcpy(o.ssid.data(), b + 36, 33);
    std::memcpy(o.name.data(), b + 69, 33);
    if (b[104] != 255 && b[104] != 4 && b[104] != 5 && b[104] != 8 && b[104] != 13 &&
        b[104] != 16 && b[104] != 24)
        return false;
    o.wifi_subtype = WifiSubtype(b[104]);
    if (o.radio == Radio::ble && o.wifi_subtype != WifiSubtype::unknown)
        return false;
    o.payload_size = get16(b + 102);
    std::copy(b + 132, b + 132 + o.payload_size, o.payload.begin());
    o.wifi_header_size = b[105];
    if (o.wifi_header_size > o.wifi_header.size() ||
        !std::all_of(b + 106 + o.wifi_header_size, b + 132, [](uint8_t v) { return v == 0; }))
        return false;
    std::copy(b + 106, b + 106 + o.wifi_header_size, o.wifi_header.begin());
    WifiAddresses addresses{};
    if ((o.wifi_header_size && !wifi_addresses(o, addresses)) ||
        ((o.wifi_subtype == WifiSubtype::data || o.wifi_subtype == WifiSubtype::qos_data) &&
         !o.wifi_header_size))
        return false;
    return o.wifi_subtype != WifiSubtype::probe_request ||
           valid_probe_ies(o.payload.data(), o.payload_size);
}
bool observation_json(const Observation &o, const Track &t, Match match, Fix fix, const Gps &gps,
                      char *out, size_t cap) {
    if (!out || !cap)
        return false;
    Json j{out, cap};
    int64_t utc{};
    j.add("{\"schema_version\":1,\"node\":%u,\"boot\":%u,\"sequence\":%u,\"capture_ms\":%llu,\"utc_"
          "ms\":",
          unsigned(o.node), unsigned(o.boot), unsigned(o.sequence), (unsigned long long)o.at_ms);
    if (gps.utc(o.at_ms, utc))
        j.add("%lld", (long long)utc);
    else
        j.add("null");
    j.add(",\"time_source\":\"%s\",\"gps_sync_age_ms\":",
          gps.has_time() ? "gps_holdover" : "unknown");
    if (gps.has_time())
        j.add("%llu", (unsigned long long)difference(o.at_ms, gps.last_sync()));
    else
        j.add("null");
    j.add(",\"timing_uncertainty_ms\":%u,\"radio\":\"%s\",\"channel\":%u,\"address_type\":%u,"
          "\"address\":\"",
          unsigned(o.timing_uncertainty_ms), o.radio == Radio::wifi ? "wifi" : "ble",
          unsigned(o.channel), unsigned(o.address_type));
    for (size_t i = 0; i < 6; ++i)
        j.add("%s%02x", i ? ":" : "", unsigned(o.address[i]));
    j.add("\",\"rssi_dbm\":%d,\"ssid\":", int(o.rssi));
    j.string(text_view(o.ssid));
    j.add(",\"ssid_hex\":\"");
    for (unsigned char byte : text_view(o.ssid))
        j.add("%02x", unsigned(byte));
    j.add("\",\"name\":");
    j.string(text_view(o.name));
    j.add(",\"rule_id\":");
    if (match.rule)
        j.string(match.rule->id);
    else
        j.add("null");
    j.add(",\"category\":");
    if (match.rule)
        j.string(match.rule->category);
    else
        j.add("null");
    j.add(",\"rule_priority\":");
    if (match.rule)
        j.add("%u", unsigned(match.rule->priority));
    else
        j.add("null");
    j.add(",\"receiver_boot\":%u,\"confidence\":", unsigned(o.receiver_boot));
    if (match.rule)
        j.string(match.rule->confidence);
    else
        j.add("null");
    j.add(",\"ble_ad_hex\":\"");
    if (o.radio == Radio::ble)
        for (size_t i = 0; i < std::min<size_t>(o.payload_size, o.payload.size()); ++i)
            j.add("%02x", unsigned(o.payload[i]));
    j.add("\"");
    const auto remote_messages = remote_id_messages(o);
    if (remote_messages.count)
        j.add(",\"remote_id_transport\":\"%s\"",
              remote_id_transport_name(remote_messages.transport));
    j.add(",\"wifi_remote_id_hex\":\"");
    if (o.radio == Radio::wifi && o.wifi_subtype != WifiSubtype::probe_request)
        for (size_t i = 0; i < std::min<size_t>(o.payload_size, o.payload.size()); ++i)
            j.add("%02x", unsigned(o.payload[i]));
    j.add("\"");
    if (o.radio == Radio::wifi) {
        j.add(",\"wifi_subtype\":%u,\"wifi_payload_format\":\"%s\",\"wifi_ie_hex\":\"",
              unsigned(o.wifi_subtype),
              o.wifi_subtype == WifiSubtype::probe_request ? "probe_ie_v1"
              : (o.wifi_subtype == WifiSubtype::data || o.wifi_subtype == WifiSubtype::qos_data)
                  ? "header_only_v1"
                  : "remote_id");
        if (o.wifi_subtype == WifiSubtype::probe_request)
            for (size_t i = 0; i < std::min<size_t>(o.payload_size, o.payload.size()); ++i)
                j.add("%02x", unsigned(o.payload[i]));
        j.add("\"");
    }
    if (o.radio == Radio::wifi) {
        j.add(",\"rssi_role\":\"transmitter\",\"match_evidence\":\"%s\",\"wifi_header_hex\":\"",
              match.rule && indirect_rule(*match.rule) ? "indirect_receiver" : "transmitter");
        for (size_t i = 0; i < std::min<size_t>(o.wifi_header_size, o.wifi_header.size()); ++i)
            j.add("%02x", unsigned(o.wifi_header[i]));
        j.add("\"");
        WifiAddresses addresses{};
        if (wifi_addresses(o, addresses)) {
            auto address = [&](const char *key, const std::array<uint8_t, 6> &bytes) {
                j.add(",\"%s\":\"", key);
                for (size_t i = 0; i < bytes.size(); ++i)
                    j.add("%s%02x", i ? ":" : "", unsigned(bytes[i]));
                j.add("\"");
            };
            address("wifi_receiver", addresses.receiver);
            address("wifi_transmitter", addresses.transmitter);
            address("wifi_source", addresses.source);
            address("wifi_destination", addresses.destination);
            address("wifi_bssid", addresses.bssid);
        }
    }
    j.add(",\"location_role\":\"receiver\",\"receiver_fix\":");
    j.fix(fix);
    j.add(",\"peak_dbm\":%d,\"peak_capture_ms\":%llu,\"peak_receiver_fix\":", int(t.peak.rssi),
          (unsigned long long)t.peak.at_ms);
    j.fix(t.peak_fix);
    j.add(",\"peak_sequence\":%u,\"peak_timing_uncertainty_ms\":%u,\"peak_utc_ms\":",
          unsigned(t.peak.sequence), unsigned(t.peak.timing_uncertainty_ms));
    if (gps.utc(t.peak.at_ms, utc))
        j.add("%lld", (long long)utc);
    else
        j.add("null");
    j.add(",\"first_seen_ms\":%llu,\"last_seen_ms\":%llu,\"peak_changed\":%s,\"observations\":%llu}"
          "\n",
          (unsigned long long)t.first_seen, (unsigned long long)t.latest.at_ms,
          t.peak_changed ? "true" : "false", (unsigned long long)t.count);
    return j.ok;
}
bool drone_detection_json(const Observation &o, size_t index, Fix receiver, const Gps &gps,
                          char *out, size_t capacity) {
    const auto messages = remote_id_messages(o);
    if (!out || !capacity || index >= messages.count)
        return false;
    const uint8_t *m = messages.messages[index];
    const unsigned type = m[0] >> 4;
    Json j{out, capacity};
    j.add("{\"schema_version\":1,\"event\":\"drone_detection\",\"protocol\":\"remote_id\","
          "\"protocol_version\":2,\"authenticated\":false,\"node\":%u,\"boot\":%u,"
          "\"receiver_boot\":%u,\"sequence\":%u,\"capture_ms\":%llu,\"utc_ms\":",
          unsigned(o.node), unsigned(o.boot), unsigned(o.receiver_boot), unsigned(o.sequence),
          (unsigned long long)o.at_ms);
    int64_t utc{};
    if (gps.utc(o.at_ms, utc))
        j.add("%lld", (long long)utc);
    else
        j.add("null");
    j.add(",\"timing_uncertainty_ms\":%u,\"radio\":\"%s\",\"channel\":%u,"
          "\"address_type\":%u,\"address\":\"",
          unsigned(o.timing_uncertainty_ms), o.radio == Radio::wifi ? "wifi" : "ble",
          unsigned(o.channel), unsigned(o.address_type));
    for (size_t i = 0; i < 6; ++i)
        j.add("%s%02x", i ? ":" : "", unsigned(o.address[i]));
    j.add("\",\"rssi_dbm\":%d,\"receiver_fix\":", int(o.rssi));
    j.fix(receiver);
    j.add(",\"transport\":\"%s\"", remote_id_transport_name(messages.transport));
    j.add(",\"message_index\":%u,\"message_counter\":%u,\"message_type\":%u,\"raw_hex\":\"",
          unsigned(index), unsigned(messages.counters[index]), type);
    for (size_t i = 0; i < 25; ++i)
        j.add("%02x", unsigned(m[i]));
    j.add("\",\"data\":{");
    auto number = [&](const char *key, double value, bool valid) {
        j.add("\"%s\":", key);
        if (valid)
            j.add("%.7f", value);
        else
            j.add("null");
    };
    auto altitude = [&](const char *key, size_t offset) {
        auto raw = get16(m + offset);
        number(key, raw * 0.5 - 1000, raw != 0);
    };
    auto position = [&](size_t offset) {
        // Avoid implementation-defined unsigned-to-signed conversions.
        auto coordinate = [&](size_t at) {
            uint32_t raw = get32(m + at);
            return (raw <= 0x7fffffffu ? int64_t(raw) : int64_t(raw) - 0x100000000ll) / 1e7;
        };
        double lat = coordinate(offset), lon = coordinate(offset + 4);
        bool valid = std::abs(lat) <= 90 && std::abs(lon) <= 180 && (lat != 0 || lon != 0);
        number("latitude", lat, valid);
        j.add(",");
        number("longitude", lon, valid);
    };
    auto string = [&](const char *key, size_t offset, size_t size) {
        size_t n = 0;
        while (n < size && m[offset + n])
            ++n;
        j.add("\"%s\":", key);
        j.string(std::string_view(reinterpret_cast<const char *>(m + offset), n));
    };
    switch (type) {
    case 0:
        j.add("\"id_type\":%u,\"ua_type\":%u,\"uas_id_hex\":\"", m[1] >> 4, m[1] & 15);
        for (size_t i = 2; i < 22; ++i)
            j.add("%02x", unsigned(m[i]));
        j.add("\",\"uas_id\":");
        if ((m[1] >> 4) == 1 || (m[1] >> 4) == 2) {
            size_t n = 0;
            while (n < 20 && m[2 + n])
                ++n;
            j.string(std::string_view(reinterpret_cast<const char *>(m + 2), n));
        } else
            j.add("null");
        break;
    case 1: {
        j.add("\"status\":%u,", m[1] >> 4);
        position(5);
        j.add(",");
        unsigned direction = m[2] + ((m[1] & 2) ? 180 : 0);
        number("direction_deg", direction, direction <= 359);
        j.add(",");
        double speed = (m[1] & 1) ? m[3] * 0.75 + 63.75 : m[3] * 0.25;
        number("speed_horizontal_mps", speed, speed <= 254.25);
        j.add(",");
        int vertical = m[4] < 128 ? m[4] : int(m[4]) - 256;
        number("speed_vertical_mps", vertical * 0.5, std::abs(vertical) <= 124);
        j.add(",");
        altitude("altitude_baro_m", 13);
        j.add(",");
        altitude("altitude_geodetic_m", 15);
        j.add(",");
        altitude("height_m", 17);
        j.add(",\"height_reference\":%u,\"horizontal_accuracy_code\":%u,"
              "\"vertical_accuracy_code\":%u,\"baro_accuracy_code\":%u,"
              "\"speed_accuracy_code\":%u,\"timestamp_accuracy_code\":%u,",
              (m[1] >> 2) & 1, m[19] & 15, m[19] >> 4, m[20] >> 4, m[20] & 15, m[23] & 15);
        auto timestamp = get16(m + 21);
        number("seconds_after_hour", timestamp / 10.0, timestamp <= 36000);
        break;
    }
    case 2:
        // Authentication pages are retained, not verified or reassembled.
        j.add("\"auth_type\":%u,\"page\":%u", m[1] >> 4, m[1] & 15);
        break;
    case 3:
        j.add("\"description_type\":%u,", unsigned(m[1]));
        string("description", 2, 23);
        break;
    case 4:
        j.add("\"operator_location_type\":%u,\"classification_type\":%u,"
              "\"operator_position\":{",
              m[1] & 3, (m[1] >> 2) & 7);
        position(2);
        j.add("},\"area_count\":%u,\"area_radius_m\":%u,", unsigned(get16(m + 10)),
              unsigned(m[12]) * 10);
        altitude("area_ceiling_m", 13);
        j.add(",");
        altitude("area_floor_m", 15);
        j.add(",");
        altitude("operator_altitude_geodetic_m", 18);
        j.add(",\"category_eu\":%u,\"class_eu\":%u,\"timestamp_since_2019_s\":%u", m[17] >> 4,
              m[17] & 15, unsigned(get32(m + 20)));
        break;
    case 5:
        j.add("\"operator_id_type\":%u,", unsigned(m[1]));
        string("operator_id", 2, 20);
        break;
    }
    j.add("}}\n");
    return j.ok;
}
} // namespace wiknow
