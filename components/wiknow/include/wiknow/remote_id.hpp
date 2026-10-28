#pragma once
#include "wiknow/core.hpp"

namespace wiknow {
enum class RemoteIdTransport : uint8_t { ble, wifi_beacon, wifi_nan };
const char *remote_id_transport_name(RemoteIdTransport transport);
// Views into Observation.payload; do not outlive or modify the observation.
struct RemoteIdMessages {
    std::array<const uint8_t *, 9> messages{};
    std::array<uint8_t, 9> counters{};
    size_t count{};
    RemoteIdTransport transport{RemoteIdTransport::ble};
};
// ASTM/OpenDroneID version 2: BLE service data, Wi-Fi beacon IE and NAN action body.
// Reject malformed envelopes atomically. Unsupported versions are not detections.
RemoteIdMessages remote_id_messages(const Observation &observation);
// RID predicates in an all-rule must be satisfied by the same decoded message.
// No cross-message or cached identity/position correlation is performed.
bool is_remote_id_field(Field field);
bool remote_id_predicate(const Predicate &predicate, const uint8_t *message);
bool drone_detection_json(const Observation &observation, size_t message_index, Fix receiver,
                          const Gps &gps, char *out, size_t capacity);
} // namespace wiknow
