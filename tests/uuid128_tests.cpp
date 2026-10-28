#include "wiknow/ble_reassembly.hpp"
#include "wiknow/core.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
using namespace wiknow;
using Bytes = std::vector<uint8_t>;
static constexpr auto uuid = "00112233-4455-6677-8899-aabbccddeeff";
static const Bytes wire{0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
                        0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00};
static Bytes ad(uint8_t type, const Bytes &data) {
    Bytes result{uint8_t(data.size() + 1), type};
    result.insert(result.end(), data.begin(), data.end());
    return result;
}
static Rule rule() {
    Rule r{};
    r.id = "uuid-fixture";
    r.radio = Radio::ble;
    r.enabled = true;
    r.predicates[0] = {Field::service_uuid128, uuid};
    r.predicate_count = 1;
    r.priority = 100;
    r.buzzer = true;
    r.follow = true;
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
static void verify(const Bytes &bytes, bool expected) {
    auto o = observation(bytes);
    const auto r = rule();
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
    for (uint8_t type : {0x06, 0x07, 0x21}) {
        verify(ad(type, wire), true);
        for (size_t i = 0; i < 16; ++i) {
            auto near = wire;
            near[i] ^= 1;
            verify(ad(type, near), false);
        }
        auto reversed = wire;
        std::reverse(reversed.begin(), reversed.end());
        verify(ad(type, reversed), false);
        for (size_t n = 0; n < 16; ++n)
            verify(ad(type, Bytes(wire.begin(), wire.begin() + n)), false);
    }
    Bytes multiple(16, 0);
    multiple.insert(multiple.end(), wire.begin(), wire.end());
    verify(ad(0x06, multiple), true);
    verify(ad(0x07, multiple), true);
    verify(ad(0x21, multiple), false); // No scanning application data.
    auto elements = ad(0x06, Bytes(16, 0));
    const auto second = ad(0x07, wire);
    elements.insert(elements.end(), second.begin(), second.end());
    verify(elements, true); // A later UUID AD, not only a later entry in one list.
    auto service = wire;
    service.insert(service.end(), {1, 2, 3});
    verify(ad(0x21, service), true);
    verify(ad(0x06, service), false);
    verify(ad(0x07, service), false);
    verify(ad(0xff, wire), false);
    verify(ad(0x16, wire), false);
    auto mixed = ad(0x09, {'L', 'A', 'B'});
    auto good = ad(0x07, wire);
    mixed.insert(mixed.end(), good.begin(), good.end());
    verify(mixed, true);
    auto bad_tail = ad(0x06, Bytes(15, 0));
    mixed.insert(mixed.end(), bad_tail.begin(), bad_tail.end());
    verify(mixed, false);
    auto o = observation(good);
    auto r = rule();
    r.predicates[0].value = "00112233-4455-6677-8899-AABBCCDDEEFF";
    assert(matches(r, o));
    for (auto bad : {"00112233445566778899aabbccddeeff", "00112233:4455-6677-8899-aabbccddeeff",
                     "00112233-4455-6677-8899-aabbccddeefg"}) {
        r.predicates[0].value = bad;
        assert(!matches(r, o));
    }
    r = rule();
    r.predicate_count = 2;
    r.predicates[1] = {Field::name_prefix, "LAB"};
    assert(!matches(r, o));
    r.require_all = false;
    assert(matches(r, o));
    mixed = ad(0x09, {'L', 'A', 'B'});
    mixed.insert(mixed.end(), good.begin(), good.end());
    o = observation(mixed);
    r.require_all = true;
    assert(matches(r, o));
    auto uuid16 = ad(0x03, {0x0f, 0x18});
    mixed.insert(mixed.end(), uuid16.begin(), uuid16.end());
    o = observation(mixed);
    r.predicates[1] = {Field::service_uuid16, "180F"};
    assert(matches(r, o));
    Rule rules[]{r, rule()};
    rules[0].priority = 1;
    const auto best = best_match(rules, 2, o);
    assert(best.rule == &rules[1]);
    assert(best.rule->buzzer && best.rule->follow);
    Tracker tracker;
    const auto &track = tracker.observe(o, {});
    char json[log_line_capacity];
    assert(observation_json(o, track, best, {}, Gps{}, json, sizeof(json)));
    assert(std::strstr(json, "ffeeddccbbaa99887766554433221100"));
    assert(std::strstr(json, "uuid-fixture"));
    // A malformed trailing AD must defeat a hit even for a manually constructed
    // or forwarded observation that has not passed the local parser.
    o = observation(good);
    o.payload[o.payload_size++] = 8;
    assert(!matches(rule(), o));
    assert(!parse_ble(o.payload.data(), o.payload_size, o));
    // The existing bounded assembler feeds the identical matcher at every split.
    for (uint8_t type : {0, 1}) {
        for (size_t split = 1; split < mixed.size(); ++split) {
            BleAssembler assembler;
            BleAdvertisement complete{};
            BleFragment f{};
            f.key.address_type = type;
            f.key.sid = 1;
            f.capture.at_ms = 100;
            f.data = mixed.data();
            f.size = split;
            f.status = BleDataStatus::incomplete;
            assert(assembler.accept(f, complete) == BleAssemblyResult::pending);
            f.data += split;
            f.size = mixed.size() - split;
            f.status = BleDataStatus::complete;
            assert(assembler.accept(f, complete) == BleAssemblyResult::complete);
            auto assembled =
                observation(Bytes(complete.data.begin(), complete.data.begin() + complete.size));
            assembled.address_type = complete.key.address_type;
            assert(matches(rule(), assembled));
        }
    }
    Bytes oversized(256, 0);
    Observation scratch{};
    assert(!parse_ble(oversized.data(), oversized.size(), scratch));
    auto terminated = Bytes{0};
    terminated.insert(terminated.end(), good.begin(), good.end());
    verify(terminated, false);
}
