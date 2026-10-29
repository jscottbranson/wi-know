#pragma once
#include "wiknow/core.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace platform {
enum class LogClass { unmatched, matched };

// Only files created by this logger belong to its retention policy.
inline bool log_file_id(const char *name, uint32_t &id) {
    if (std::strlen(name) != 14 || name[0] != 'W' ||
        (name[1] != 'K' && name[1] != 'M' && name[1] != 'U') || std::strcmp(name + 10, ".LOG") != 0)
        return false;
    uint32_t value = 0;
    for (size_t i = 2; i < 10; ++i) {
        if (name[i] < '0' || name[i] > '9')
            return false;
        value = value * 10 + uint32_t(name[i] - '0');
    }
    id = value;
    return true;
}

inline bool following_alert_json(const wiknow::Observation &o, wiknow::Fix fix,
                                 const wiknow::Gps &gps, double displacement_m, uint32_t delay_ms,
                                 char *out, size_t capacity,
                                 const wiknow::FollowingEvidence *evidence = nullptr,
                                 uint32_t jitter_m = 0) {
    if (!out || !capacity || !std::isfinite(displacement_m) || displacement_m < 0)
        return false;
    char location[160] = "null", utc_text[32] = "null";
    int64_t utc = 0;
    if (gps.utc(o.at_ms, utc))
        std::snprintf(utc_text, sizeof(utc_text), "%lld", static_cast<long long>(utc));
    if (fix.valid && std::isfinite(fix.latitude) && std::isfinite(fix.longitude) &&
        std::abs(fix.latitude) <= 90 && std::abs(fix.longitude) <= 180)
        std::snprintf(location, sizeof(location),
                      "{\"latitude\":%.7f,\"longitude\":%.7f,\"fix_at_ms\":%llu}", fix.latitude,
                      fix.longitude, static_cast<unsigned long long>(fix.at_ms));
    char scoring[320] = "";
    if (evidence) {
        if (!evidence->event || !evidence->reason)
            return false;
        std::snprintf(
            scoring, sizeof(scoring),
            ",\"identity_basis\":\"radio_address_type_address\","
            "\"reason\":\"%s\",\"hits\":%u,\"score\":%u,\"duration_ms\":%llu,"
            "\"gps_jitter_allowance_m\":%u,\"displacement_basis\":\"radial_from_first_fix\"",
            evidence->reason, unsigned(evidence->hits), unsigned(evidence->score),
            static_cast<unsigned long long>(evidence->duration_ms), unsigned(jitter_m));
    }
    int size = std::snprintf(
        out, capacity,
        "{\"schema_version\":1,\"event\":\"%s\",\"node\":%u,\"boot\":%u,"
        "\"receiver_boot\":%u,\"sequence\":%u,\"radio\":\"%s\","
        "\"address\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"address_type\":%u,"
        "\"channel\":%u,\"rssi_dbm\":%d,\"at_ms\":%llu,\"capture_ms\":%llu,"
        "\"utc_ms\":%s,\"time_source\":\"%s\",\"timing_uncertainty_ms\":%u,"
        "\"location_role\":\"receiver\",\"receiver_fix\":%s,"
        "\"displacement_m\":%.1f,\"delay_ms\":%u%s}\n",
        evidence ? evidence->event : "following_alert", unsigned(o.node), unsigned(o.boot),
        unsigned(o.receiver_boot), unsigned(o.sequence),
        o.radio == wiknow::Radio::wifi ? "wifi" : "ble", unsigned(o.address[0]),
        unsigned(o.address[1]), unsigned(o.address[2]), unsigned(o.address[3]),
        unsigned(o.address[4]), unsigned(o.address[5]), unsigned(o.address_type),
        unsigned(o.channel), int(o.rssi), static_cast<unsigned long long>(o.at_ms),
        static_cast<unsigned long long>(o.at_ms), utc_text,
        gps.has_time() ? "gps_holdover" : "unknown", unsigned(o.timing_uncertainty_ms), location,
        displacement_m, unsigned(delay_ms), scoring);
    return size >= 0 && size_t(size) < capacity;
}
} // namespace platform
