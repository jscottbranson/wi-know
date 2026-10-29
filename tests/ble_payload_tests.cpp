#include "wiknow/ble_reassembly.hpp"
#include "wiknow/core.hpp"
#include "wiknow/observation_queue.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>

using namespace wiknow;
using Bytes = std::vector<uint8_t>;

static Rule rule(std::string_view pattern) {
    Rule r{};
    r.id = "raw-payload-fixture";
    r.radio = Radio::ble;
    r.enabled = true;
    r.priority = 100;
    r.predicates[0] = {Field::ble_payload_contains, pattern};
    r.predicate_count = 1;
    return r;
}
static Observation observation(const Bytes &bytes) {
    Observation o{};
    assert(parse_ble(bytes.data(), bytes.size(), o));
    o.node = 1;
    o.address = {2, 1, 2, 3, 4, 5};
    o.rssi = -42;
    o.at_ms = 100;
    return o;
}
static void verify(const Bytes &bytes, std::string_view pattern, bool expected) {
    auto o = observation(bytes);
    const auto r = rule(pattern);
    for (uint8_t address_type : {0, 1}) {
        o.address_type = address_type;
        assert(matches(r, o) == expected);
        Report report{};
        report.has_observation = true;
        report.observation = o;
        Packet packet{};
        packet.node = o.node;
        assert(pack_report(report, packet));
        Report decoded{};
        assert(unpack_report(packet, decoded));
        assert(decoded.observation.payload == o.payload);
        assert(decoded.observation.payload_size == o.payload_size);
        assert(decoded.observation.address_type == address_type);
        assert(matches(r, decoded.observation) == expected);
    }
}
int main() {
    const Bytes data{7, 0xff, 0xaa, 0, 0x80, 0xff, 0xaa, 0xbb};
    verify(data, "07ff", true); // Offset zero includes AD length/type bytes.
    verify(data, "aa0080", true); // Binary NUL and non-UTF-8 bytes, middle.
    verify(data, "AABB", true); // Final byte is inside the captured extent.
    verify(data, "07ffaa0080ffaabb", true); // Exact payload boundary.
    verify(data, "07ffaa0080ffaabb00", false);
    verify(data, "aabb00", false);
    verify(data, "abcd", false);
    verify({5, 0xff, 0xaa, 0xaa, 0xaa, 0xbb}, "aaaabb", true); // Overlap.
    verify({2, 0xff, 0xaa, 2, 0xff, 0xbb}, "aa02ffbb", true); // Cross AD elements.
    verify({0, 0xff, 0xaa, 0xbb}, "00ffaabb", true); // Raw bytes after AD terminator.

    // A tag is just bytes, with no vendor identity or advertised-field claim.
    verify({10, 0xff, 'B', 'W', 'C', 'D', 'E', 'V', 'I', 'C', 'E'},
           "425743444556494345", true);
    auto o = observation(data);
    for (const auto bad : {"", "a", "aag0", "aa bb", "aa:bb", "aa-bb", "0xaabb", "aa\nbb"})
        assert(!matches(rule(bad), o));
    const std::string too_long(66, 'a');
    assert(!matches(rule(too_long), o));
    const std::string maximum(64, 'a');
    Bytes max_pattern{33, 0xff};
    max_pattern.insert(max_pattern.end(), 32, 0xaa);
    verify(max_pattern, maximum, true);
    Bytes full_payload{254, 0xff};
    full_payload.resize(max_payload, 0xaa);
    full_payload.back() = 0xbb;
    verify(full_payload, "aabb", true);
    verify(full_payload, "bb00", false);

    // Metadata and stale bytes beyond payload_size are outside the search domain.
    o = observation({2, 0xff, 0});
    o.address = {0xaa, 0xbb, 0, 0, 0, 0};
    o.payload[3] = 0xaa;
    o.payload[4] = 0xbb;
    std::strcpy(o.name.data(), "AB");
    assert(!matches(rule("aabb"), o));
    assert(!matches(rule("4142"), o));
    o = observation(data);
    o.radio = Radio::wifi;
    auto wifi_rule = rule("aabb");
    wifi_rule.radio = Radio::wifi;
    assert(!matches(wifi_rule, o));
    o = observation(data);
    auto r = rule("aabb");
    r.predicates[1] = {Field::name_prefix, "LAB"};
    r.predicate_count = 2;
    assert(!matches(r, o));
    r.require_all = false;
    assert(matches(r, o));
    r.require_all = true;
    const Bytes named{4, 9, 'L', 'A', 'B', 3, 0xff, 0xaa, 0xbb};
    o = observation(named);
    assert(matches(r, o));
    r.enabled = false;
    assert(!matches(r, o));

    // A malformed trailing AD invalidates even an earlier raw hit, including
    // manually constructed/forwarded observations that bypass parse_ble().
    o = observation(data);
    o.payload[o.payload_size++] = 4;
    assert(!matches(rule("aabb"), o));
    Report malformed{};
    malformed.has_observation = true;
    malformed.observation = o;
    Packet packet{};
    assert(pack_report(malformed, packet));
    Report forwarded{};
    assert(unpack_report(packet, forwarded));
    assert(!matches(rule("aabb"), forwarded.observation));
    assert(!parse_ble(o.payload.data(), o.payload_size, o));
    assert(o.payload_size == 0 && o.name[0] == 0);
    assert(!matches(rule("aabb"), o));
    auto partial = data;
    partial.pop_back();
    assert(!parse_ble(partial.data(), partial.size(), o));
    assert(!matches(rule("aa0080"), o));
    o = observation(named);
    Bytes oversized(max_payload + 1, 0);
    assert(!parse_ble(oversized.data(), oversized.size(), o));
    assert(o.payload_size == 0 && o.name[0] == 0);
    o = observation(named);
    assert(!parse_ble(nullptr, 1, o));
    assert(o.payload_size == 0 && o.name[0] == 0);
    o.payload_size = max_payload + 1;
    assert(!matches(rule("aabb"), o));
    const uint8_t empty = 0;
    assert(parse_ble(&empty, 0, o));
    assert(!matches(rule("00"), o));

    // Two independent complete sightings never combine, even for one address.
    verify({0, 0xaa}, "aabb", false);
    verify({0, 0xbb}, "aabb", false);
    for (size_t split = 1; split < named.size(); ++split) {
        BleAssembler assembler;
        BleAdvertisement complete{};
        BleFragment f{};
        f.key.sid = 1;
        f.capture.at_ms = 100;
        f.data = named.data();
        f.size = split;
        f.status = BleDataStatus::incomplete;
        assert(assembler.accept(f, complete) == BleAssemblyResult::pending);
        f.data += split;
        f.size = named.size() - split;
        f.status = BleDataStatus::complete;
        assert(assembler.accept(f, complete) == BleAssemblyResult::complete);
        verify(Bytes(complete.data.begin(), complete.data.begin() + complete.size), "aabb", true);
    }
    // Separate scan responses and abandoned fragments cannot manufacture hits.
    BleAssembler assembler;
    BleAdvertisement complete{};
    const Bytes prefix{3, 0xff, 0xaa}, suffix{0xbb};
    BleFragment f{};
    f.key.sid = 1;
    f.capture.at_ms = 100;
    f.data = prefix.data();
    f.size = prefix.size();
    f.status = BleDataStatus::incomplete;
    assert(assembler.accept(f, complete) == BleAssemblyResult::pending);
    const Bytes scan_response{0, 0xbb};
    auto scan = f;
    scan.key.properties = 0x08;
    scan.status = BleDataStatus::complete;
    scan.data = scan_response.data();
    scan.size = scan_response.size();
    assert(assembler.accept(scan, complete) == BleAssemblyResult::complete);
    verify(Bytes(complete.data.begin(), complete.data.begin() + complete.size), "aabb", false);
    f.data = suffix.data();
    f.size = suffix.size();
    f.status = BleDataStatus::truncated;
    assert(assembler.accept(f, complete) == BleAssemblyResult::rejected);
    assembler.reset();
    assert(assembler.accept(scan, complete) == BleAssemblyResult::complete);
    verify(Bytes(complete.data.begin(), complete.data.begin() + complete.size), "aabb", false);

    // Worker admission uses best_match, preserving payload rules under ordinary
    // traffic saturation; the forwarded observation retains reproducible bytes.
    ObservationQueue queue;
    const auto raw_rule = rule("aabb");
    o = observation({0});
    for (size_t i = 0; i < ObservationQueue::ordinary_capacity; ++i)
        assert(queue.push(o, best_match(&raw_rule, 1, o).rule != nullptr));
    assert(!queue.push(o, false));
    o = observation(named);
    assert(queue.push(o, best_match(&raw_rule, 1, o).rule != nullptr));
    Observation first{};
    assert(queue.pop(first));
    const auto match = best_match(&raw_rule, 1, first);
    assert(match.rule == &raw_rule);
    Tracker tracker;
    const auto &track = tracker.observe(first, {});
    char json[log_line_capacity];
    assert(observation_json(first, track, match, {}, Gps{}, json, sizeof(json)));
    assert(std::strstr(json, "04094c414203ffaabb"));
    assert(std::strstr(json, "raw-payload-fixture"));
}
