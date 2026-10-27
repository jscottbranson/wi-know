#pragma once
#include "wiknow/core.hpp"

namespace wiknow {
// Views into Observation.payload; do not outlive or modify the observation.
struct RemoteIdMessages {
    std::array<const uint8_t *, 9> messages{};
    std::array<uint8_t, 9> counters{};
    size_t count{};
};
// ASTM/OpenDroneID protocol version 2, BLE service data and Wi-Fi beacon vendor IE.
// Reject malformed envelopes atomically. Unsupported versions are not detections.
RemoteIdMessages remote_id_messages(const Observation &observation);
bool drone_detection_json(const Observation &observation, size_t message_index, Fix receiver,
                          const Gps &gps, char *out, size_t capacity);
} // namespace wiknow
