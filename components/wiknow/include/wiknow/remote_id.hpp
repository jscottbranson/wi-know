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
bool drone_detection_json(const Observation &observation, size_t message_index, Fix receiver,
                          const Gps &gps, char *out, size_t capacity);
} // namespace wiknow
