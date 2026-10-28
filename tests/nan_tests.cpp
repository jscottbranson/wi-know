#include "fixtures/nan_reference.hpp"
#include "wiknow/remote_id.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>
using namespace wiknow;
using Bytes = std::vector<uint8_t>;
static Bytes fixture() {
    return {std::begin(nan_reference), std::end(nan_reference)};
}
static Observation parse(const Bytes &frame) {
    Observation o{};
    o.node = 3;
    o.boot = 17;
    o.sequence = 29;
    o.at_ms = 1234;
    o.rssi = -55;
    o.channel = 6;
    assert(parse_wifi(frame.data(), frame.size(), o));
    return o;
}
static void rejected(const Bytes &frame) {
    Observation o{};
    assert(!parse_wifi(frame.data(), frame.size(), o));
}
int main() {
    auto frame = fixture();
    auto o = parse(frame);
    auto messages = remote_id_messages(o);
    assert(messages.count == 2 && messages.transport == RemoteIdTransport::wifi_nan);
    assert(messages.counters[0] == 7 && messages.counters[1] == 7);
    assert(o.address == (std::array<uint8_t, 6>{2, 0x11, 0x22, 0x33, 0x44, 0x55}));
    assert(o.payload_size == frame.size() - 24);
    assert(std::equal(frame.begin() + 24, frame.end(), o.payload.begin()));
    // Full evidence, identity, time and transport survive child/parent bus encoding.
    Report report{};
    report.has_observation = true;
    report.capture_age_ms = 25;
    report.observation = o;
    Packet packet{};
    packet.node = o.node;
    packet.boot = o.boot;
    assert(pack_report(report, packet));
    Report decoded{};
    assert(unpack_report(packet, decoded));
    const auto &forwarded = decoded.observation;
    assert(forwarded.payload_size == o.payload_size && forwarded.payload == o.payload);
    assert(forwarded.address == o.address && forwarded.node == 3 && forwarded.boot == 17 &&
           forwarded.sequence == 29 && decoded.capture_age_ms == 25 && forwarded.channel == 6);
    assert(remote_id_messages(forwarded).count == 2);
    char line[log_line_capacity];
    assert(drone_detection_json(forwarded, 0, {}, Gps{}, line, sizeof(line)));
    assert(std::strstr(line, "\"transport\":\"wifi_nan\""));
    assert(std::strstr(line, "\"uas_id\":\"NAN-TEST-001\""));
    assert(drone_detection_json(forwarded, 1, {}, Gps{}, line, sizeof(line)));
    assert(std::strstr(line, "\"latitude\":42.1234567"));
    Tracker tracker;
    assert(observation_json(o, tracker.observe(o, {}), {}, {}, Gps{}, line, sizeof(line)));
    assert(std::strstr(line, "\"remote_id_transport\":\"wifi_nan\""));
    assert(std::strstr(line, "\"wifi_remote_id_hex\":\"0409506f9a1303"));
    // Every truncated prefix fails, including after an otherwise valid message pack.
    for (size_t n = 0; n < frame.size(); ++n)
        rejected(Bytes(frame.begin(), frame.begin() + n));
    // Envelope, service ID, descriptor, pack header/messages, extension and bounds.
    for (size_t at : {0,  4,  24, 25, 26, 29, 30, 31, 32, 33,  39,  40,  41,
                      42, 44, 45, 46, 47, 72, 97, 98, 99, 100, 101, 102, 103}) {
        auto bad = frame;
        bad[at] ^= 0x80;
        rejected(bad);
    }
    for (uint8_t flags : {1, 2, 4, 0x40, 0x80}) {
        auto bad = frame;
        bad[1] = flags;
        rejected(bad);
    }
    auto bad = frame;
    bad[22] = 1; // noninitial MAC fragment
    rejected(bad);
    bad = frame;
    bad[44] = 0xf3; // unsupported protocol version
    rejected(bad);
    bad = frame;
    bad[47] = 0x03; // unsupported inner version
    rejected(bad);
    bad = frame;
    bad.insert(bad.end(), frame.begin() + 30, frame.begin() + 97); // duplicate descriptor
    rejected(bad);
    bad = frame;
    bad.insert(bad.end(), frame.begin() + 97, frame.end()); // duplicate extension
    rejected(bad);
    // Unknown, well-formed attributes are retained; truncated attributes invalidate all.
    auto extra = frame;
    extra.insert(extra.end(), {0x80, 2, 0, 0xab, 0xcd});
    assert(parse(extra).payload_size == extra.size() - 24);
    extra.pop_back();
    rejected(extra);
    // Maximum nine-message pack exactly fills 255-byte action-body representation.
    Bytes largest(frame.begin(), frame.begin() + 47);
    largest[31] = 239;
    largest[42] = 229;
    largest[46] = 9;
    for (size_t i = 0; i < 9; ++i)
        largest.insert(largest.end(), frame.begin() + 72, frame.begin() + 97);
    largest.insert(largest.end(), frame.begin() + 97, frame.end());
    o = parse(largest);
    assert(o.payload_size == 255 && remote_id_messages(o).count == 9);
    report.observation = o;
    assert(pack_report(report, packet) && unpack_report(packet, decoded));
    assert(decoded.observation.payload == o.payload && decoded.observation.payload_size == 255);
    for (size_t i = 0; i < 9; ++i)
        assert(drone_detection_json(decoded.observation, i, {}, Gps{}, line, sizeof(line)));
    assert(observation_json(o, tracker.observe(o, {}), {}, {}, Gps{}, line, sizeof(line)));
    largest.push_back(0); // never silently truncate an oversized action body
    rejected(largest);
    // Parser is bounded and rejects invalid trailing attribute lengths atomically.
    for (unsigned length = 0; length <= 65535; length += 257) {
        bad = frame;
        bad[31] = uint8_t(length);
        bad[32] = uint8_t(length >> 8);
        rejected(bad);
    }
}
