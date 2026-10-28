// Original synthetic fixtures; no community fingerprint or third-party code.
#include "wiknow/core.hpp"
#include "wiknow/discovery.hpp"
#include "wiknow/observation_queue.hpp"
#include "wiknow/remote_id.hpp"
#include "wiknow/wifi_capture.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
using namespace wiknow;
using Bytes = std::vector<uint8_t>;
static Bytes frame(Bytes ies, uint8_t subtype = 0x40) {
    Bytes bytes(subtype == 0x40 ? 24 : 36);
    bytes[0] = subtype;
    const uint8_t transmitter[]{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    std::copy(transmitter, transmitter + 6, bytes.begin() + 10);
    bytes.insert(bytes.end(), ies.begin(), ies.end());
    return bytes;
}
static Observation parse(const Bytes &bytes) {
    Observation o{};
    o.node = 3;
    o.boot = 17;
    o.sequence = 29;
    o.at_ms = 1234;
    o.rssi = -55;
    o.channel = 6;
    assert(parse_wifi(bytes.data(), bytes.size(), o));
    return o;
}
static std::string fingerprint(const Observation &o) {
    std::string result;
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < o.payload_size; ++i) {
        result += hex[o.payload[i] >> 4];
        result += hex[o.payload[i] & 15];
    }
    return result;
}
static Rule rule(Field f, std::string_view value) {
    Rule r{};
    r.radio = Radio::wifi;
    r.enabled = true;
    r.predicates[0] = {f, value};
    r.predicate_count = 1;
    return r;
}
static void checksum(uint8_t *wire, size_t length) {
    uint32_t crc = 0xffffffff;
    for (size_t i = 2; i < length - 4; ++i) {
        crc ^= wire[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    crc = ~crc;
    for (size_t i = 0; i < 4; ++i)
        wire[length - 4 + i] = uint8_t(crc >> (8 * i));
}
int main() {
    const Bytes ies{0, 0, 1, 2, 0x82, 0x84, 221, 4, 0x12, 0x34, 0x56, 1, 1, 1, 0x8b};
    const auto bytes = frame(ies);
    auto o = parse(bytes);
    assert(o.wifi_subtype == WifiSubtype::probe_request && o.payload_size == ies.size());
    assert(o.at_ms == 1234 && o.rssi == -55 && o.channel == 6);
    const auto exact = fingerprint(o);
    Rule r = rule(Field::wifi_ie_fingerprint, exact);
    assert(matches(r, o));
    r.predicates[1] = {Field::wifi_subtype, "probe_request"};
    r.predicates[2] = {Field::wifi_ssid_wildcard, "true"};
    r.predicates[3] = {Field::oui, "001122"};
    r.predicate_count = 4;
    r.priority = 123;
    r.buzzer = true;
    r.follow = true;
    assert(matches(r, o) && best_match(&r, 1, o).rule == &r);
    auto near = o;
    near.payload[5] ^= 1;
    assert(!matches(r, near));
    auto reordered =
        parse(frame({1, 2, 0x82, 0x84, 0, 0, 221, 4, 0x12, 0x34, 0x56, 1, 1, 1, 0x8b}));
    assert(!matches(r, reordered));
    auto vendor = o;
    vendor.payload[11] ^= 1;
    assert(!matches(r, vendor));
    for (size_t i = 0; i < exact.size(); ++i) {
        auto changed = exact;
        changed[i] = exact[i] == '0' ? '1' : '0';
        assert(!matches(rule(Field::wifi_ie_fingerprint, changed), o));
    }
    assert(!matches(rule(Field::wifi_ie_fingerprint, exact.substr(0, exact.size() - 2)), o));
    assert(!matches(rule(Field::wifi_ie_fingerprint, "00:00"), o));
    r.require_all = false;
    assert(matches(r, near));
    r.require_all = true;
    const auto directed = parse(frame({0, 3, 'a', 0, 'b', 1, 1, 0x82}));
    assert(!matches(rule(Field::wifi_ssid_wildcard, "true"), directed));
    assert(matches(rule(Field::wifi_ssid_wildcard, "false"), directed));
    assert(!matches(rule(Field::ssid_prefix, "a"), directed));
    assert(fingerprint(directed) == "0003610062010182");
    const auto request = parse(frame({0, 9, 'F', 'l', 'o', 'c', 'k', '-', '1', '2', '3'}));
    auto hint = rule(Field::wifi_requested_ssid_prefix, "Flock-");
    assert(matches(hint, request) && !matches(rule(Field::ssid_prefix, "Flock-"), request));
    assert(!matches(rule(Field::wifi_requested_ssid_prefix, "flock-"), request));
    assert(!matches(rule(Field::wifi_requested_ssid_prefix, "Flock-1234"), request));
    assert(!matches(rule(Field::wifi_requested_ssid_prefix, ""), request));
    assert(matches(rule(Field::wifi_requested_ssid_prefix, "a"), directed));
    assert(!matches(rule(Field::wifi_requested_ssid_prefix, "ab"), directed));
    auto deceptive = request;
    std::strcpy(deceptive.ssid.data(), "unrelated");
    assert(matches(hint, deceptive)); // Raw bytes, not display text, are authoritative.
    deceptive.payload[3] = 'a';
    std::strcpy(deceptive.ssid.data(), "Flock-123");
    assert(!matches(hint, deceptive));
    auto local_request = request;
    local_request.address[0] |= 2;
    assert(matches(hint, local_request));
    auto request_ap = parse(frame({0, 9, 'F', 'l', 'o', 'c', 'k', '-', '1', '2', '3'}, 0x80));
    assert(!matches(hint, request_ap) && matches(rule(Field::ssid_prefix, "Flock-"), request_ap));
    Report hint_report;
    hint_report.has_observation = true;
    hint_report.observation = local_request;
    Packet hint_packet;
    assert(pack_report(hint_report, hint_packet));
    Report hint_decoded;
    assert(unpack_report(hint_packet, hint_decoded) && matches(hint, hint_decoded.observation));
    const auto missing = parse(frame({1, 1, 0x82}));
    for (auto value : {"true", "false"})
        assert(!matches(rule(Field::wifi_ssid_wildcard, value), missing));
    assert(!matches(hint, missing) && !matches(hint, o));
    const auto empty = parse(frame({}));
    assert(empty.payload_size == 0);
    assert(!matches(rule(Field::wifi_ie_fingerprint, ""), empty));
    for (auto malformed :
         {Bytes{0}, Bytes{0, 2, 'a'}, Bytes{0, 0, 0, 0}, Bytes{221, 3, 1, 2, 3}, Bytes{255, 0}}) {
        auto bad = frame(malformed);
        Observation rejected{};
        assert(!parse_wifi(bad.data(), bad.size(), rejected));
    }
    for (unsigned mask : {1u, 2u, 4u, 64u, 128u}) {
        auto bad = bytes;
        bad[1] = uint8_t(mask);
        Observation rejected{};
        assert(!parse_wifi(bad.data(), bad.size(), rejected));
    }
    for (unsigned index : {0u, 22u}) {
        auto bad = bytes;
        bad[index] |= 1;
        Observation rejected{};
        assert(!parse_wifi(bad.data(), bad.size(), rejected));
    }
    Bytes maximal{0, 0, 1, 251};
    maximal.resize(255, 0x82);
    auto full = parse(frame(maximal));
    assert(full.payload_size == 255);
    auto too_big = maximal;
    too_big.push_back(1);
    too_big.push_back(0);
    auto bad = frame(too_big);
    Observation rejected{};
    assert(!parse_wifi(bad.data(), bad.size(), rejected));
    auto captured = bytes;
    captured.insert(captured.end(), 4, 0xff);
    assert(wifi_capture_size(captured.data(), captured.size(), true) == bytes.size());
    assert(wifi_capture_size(captured.data(), captured.size(), false) == 0);
    assert(wifi_capture_size(captured.data(), 27, true) == 0);
    assert(wifi_capture_size(captured.data(), 517, true) == 0);
    assert(
        parse_wifi(captured.data(), wifi_capture_size(captured.data(), captured.size(), true), o));
    assert(!parse_wifi(captured.data(), captured.size(), o)); // FCS is not IE evidence.
    o = parse(bytes);
    for (auto subtype : {uint8_t(0x80), uint8_t(0x50)}) {
        auto ap = parse(frame({0, 3, 'a', 'p', '1'}, subtype));
        assert(!matches(r, ap) && ap.payload_size == 0);
        assert(ap.wifi_subtype == WifiSubtype(subtype >> 4));
    }
    assert(remote_id_messages(o).count == 0);
    Observation looks_like_rid{};
    looks_like_rid.payload_size = 33;
    const uint8_t rid_header[]{0xfa, 0x0b, 0xbc, 0x0d, 0, 0xf2, 25, 1, 2};
    std::copy(std::begin(rid_header), std::end(rid_header), looks_like_rid.payload.begin());
    assert(remote_id_messages(looks_like_rid).count == 1);
    looks_like_rid.wifi_subtype = WifiSubtype::probe_request;
    assert(remote_id_messages(looks_like_rid).count == 0);
    assert(parse_ble(ies.data(), 0, o));
    assert(o.wifi_subtype == WifiSubtype::unknown);
    assert(!matches(r, o));
    o = parse(bytes);
    ObservationQueue queue;
    for (size_t i = 0; i < ObservationQueue::ordinary_capacity; ++i)
        assert(queue.push(missing, false));
    assert(queue.push(o, best_match(&r, 1, o).rule));
    Observation first;
    assert(queue.pop(first) && matches(r, first));
    RetainedObservation retained;
    assert(retained.retain(first));
    Report report;
    retained.report(report, 1300);
    Packet packet;
    packet.node = o.node;
    packet.boot = o.boot;
    assert(pack_report(report, packet));
    assert(packet.size == 105 + o.payload_size && protocol_version == 3);
    uint8_t wire[max_frame];
    auto n = encode_packet(packet, wire, sizeof(wire));
    Decoder decoder;
    Packet decoded;
    bool ready = false;
    for (size_t i = 0; i < n; ++i)
        ready = decoder.feed(wire[i], decoded) || ready;
    assert(ready);
    Report forwarded;
    assert(unpack_report(decoded, forwarded));
    assert(matches(r, forwarded.observation));
    assert(forwarded.observation.payload == o.payload);
    assert(forwarded.observation.address == o.address &&
           forwarded.observation.sequence == o.sequence);
    assert(forwarded.capture_age_ms == 66 && forwarded.observation.channel == 6);
    assert(same_signal(o, forwarded.observation));
    ReportReceipt receipt;
    assert(receipt.accept(forwarded.observation));
    assert(!receipt.accept(forwarded.observation));
    assert(retained.acknowledge(receipt.acknowledgement()));
    wire[2] = 2;
    checksum(wire, n); // Valid CRC: rejection must be the protocol-version gate.
    decoder.reset();
    for (size_t i = 0; i < n; ++i)
        assert(!decoder.feed(wire[i], decoded));
    packet.payload[104] = 7;
    assert(!unpack_report(packet, forwarded));
    packet.payload[104] = 4;
    packet.payload[106] = 255;
    assert(!unpack_report(packet, forwarded));
    report.observation = full;
    assert(pack_report(report, packet) && packet.size == 360);
    Identity identity{};
    identity.parent[0] = 1;
    identity.worker[0] = 2;
    assert(pack_identity(identity, packet, MessageType::session_report));
    assert(packet.size == 392);
    Packet inner;
    Identity restored;
    assert(unpack_identity(packet, restored, &inner) && unpack_report(inner, forwarded));
    assert(forwarded.observation.payload_size == 255 &&
           forwarded.observation.wifi_subtype == WifiSubtype::probe_request);
    Tracker tracker;
    auto &track = tracker.observe(o, {});
    Gps gps;
    char json[log_line_capacity];
    assert(observation_json(o, track, {&r}, {}, gps, json, sizeof(json)));
    assert(std::strstr(json, "\"wifi_subtype\":4") &&
           std::strstr(json, "\"wifi_payload_format\":\"probe_ie_v1\""));
    assert(std::strstr(json, exact.c_str()) && std::strstr(json, "\"wifi_remote_id_hex\":\"\""));
    // Maximum raw evidence, escape-heavy strings, widest metadata/provenance.
    full.ssid.fill(char(1));
    full.ssid[32] = 0;
    full.name.fill(char(1));
    full.name[32] = 0;
    full.node = UINT16_MAX;
    full.boot = UINT32_MAX;
    full.sequence = UINT32_MAX;
    full.receiver_boot = UINT32_MAX;
    full.at_ms = UINT64_MAX;
    full.timing_uncertainty_ms = UINT32_MAX;
    std::string id(64, 'x'), category(40, 'x');
    r.id = id;
    r.category = category;
    r.confidence = "broadcast";
    auto &max_track = tracker.observe(full, {});
    assert(observation_json(full, max_track, {&r}, {}, gps, json, sizeof(json)));
}
