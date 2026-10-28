#include "fixtures/nan_reference.hpp"
#include "wiknow/alert_cooldown.hpp"
#include "wiknow/remote_id.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
using namespace wiknow;
using Message = std::array<uint8_t, 25>;
static Message basic(std::string_view id, uint8_t type = 1) {
    Message m{};
    m[0] = 2;
    m[1] = uint8_t(type << 4 | 2);
    assert(id.size() <= 20);
    std::memcpy(m.data() + 2, id.data(), id.size());
    return m;
}
static void put32(Message &m, size_t at, uint32_t v) {
    for (size_t i = 0; i < 4; ++i)
        m[at + i] = uint8_t(v >> (i * 8));
}
static Observation observe(Radio radio, const std::vector<Message> &messages) {
    std::vector<uint8_t> pack{0xf2, 25, uint8_t(messages.size())};
    for (const auto &m : messages)
        pack.insert(pack.end(), m.begin(), m.end());
    Observation o{};
    if (radio == Radio::ble) {
        std::vector<uint8_t> ad{uint8_t(pack.size() + 5), 0x16, 0xfa, 0xff, 0x0d, 7};
        ad.insert(ad.end(), pack.begin(), pack.end());
        assert(parse_ble(ad.data(), ad.size(), o));
    } else {
        std::vector<uint8_t> frame(36, 0);
        frame[0] = 0x80;
        frame.insert(frame.end(), {221, uint8_t(pack.size() + 5), 0xfa, 0x0b, 0xbc, 0x0d, 7});
        frame.insert(frame.end(), pack.begin(), pack.end());
        assert(parse_wifi(frame.data(), frame.size(), o));
    }
    o.node = 1;
    o.boot = 2;
    o.sequence = 3;
    o.at_ms = 100;
    return o;
}
static Rule rule(Radio radio, std::initializer_list<Predicate> predicates, bool all = true) {
    Rule r{};
    r.id = "test-rid";
    r.category = "drones";
    r.confidence = "broadcast";
    r.radio = radio;
    r.enabled = r.buzzer = r.follow = true;
    r.priority = 100;
    r.require_all = all;
    r.predicate_count = predicates.size();
    std::copy(predicates.begin(), predicates.end(), r.predicates.begin());
    return r;
}
static void nan_rules() {
    // Exact OpenDroneID transmitter fixture, also used by the NAN capture/decoder tests.
    const std::vector<uint8_t> frame(std::begin(nan_reference), std::end(nan_reference));
    Observation o{};
    assert(parse_wifi(frame.data(), frame.size(), o));
    const auto valid = rule(Radio::wifi, {{Field::rid_valid, "true"}});
    const auto specific =
        rule(Radio::wifi, {{Field::rid_basic_id_type, "1"}, {Field::rid_basic_id, "NAN-TEST-001"}});
    const auto position = rule(
        Radio::wifi, {{Field::rid_message_type, "location"}, {Field::rid_position_valid, "true"}});
    assert(remote_id_messages(o).transport == RemoteIdTransport::wifi_nan);
    assert(matches(valid, o) && matches(specific, o) && matches(position, o));
    assert(!matches(rule(Radio::wifi, {{Field::rid_basic_id, "NAN-TEST-001"},
                                       {Field::rid_position_valid, "true"}}),
                    o));
    assert(
        matches(rule(Radio::wifi,
                     {{Field::rid_basic_id, "wrong"}, {Field::rid_position_valid, "true"}}, false),
                o));
    Report report{};
    report.has_observation = true;
    report.observation = o;
    Packet packet{};
    Report decoded{};
    assert(pack_report(report, packet) && unpack_report(packet, decoded));
    assert(matches(specific, decoded.observation) && matches(position, decoded.observation));
    auto disabled = valid;
    disabled.enabled = false;
    assert(!matches(disabled, o));
    // Exercise matching independently of the capture parser: forwarded malformed
    // action bodies and unrelated services must never satisfy a decoded predicate.
    for (size_t n = 0; n < o.payload_size; ++n) {
        auto bad = o;
        bad.payload_size = n;
        assert(!matches(valid, bad) && !matches(specific, bad));
    }
    for (size_t offset : {size_t(24), size_t(31), size_t(33), size_t(44), size_t(47)}) {
        auto bad = o;
        bad.payload[offset - 24] ^= 0x80; // header, attribute length, service, pack, inner message
        assert(!matches(valid, bad));
    }
    auto bad = o;
    bad.payload[44 - 24] = 0xf3; // Unsupported pack version.
    assert(!matches(valid, bad));
    bad = o;
    bad.payload[47 - 24] = 0x03; // Unsupported Basic ID invalidates the entire envelope.
    assert(!matches(valid, bad) && !matches(position, bad));
    bad = o;
    std::fill(bad.payload.begin() + 72 - 24 + 5, bad.payload.begin() + 72 - 24 + 13, 0);
    assert(matches(valid, bad) && matches(specific, bad) && !matches(position, bad));
}
int main() {
    nan_rules();
    const auto id = basic("WI-KNOW-TEST-001");
    Message location{};
    location[0] = 0x12;
    put32(location, 5, 421234567);
    put32(location, 9, uint32_t(-831234567));
    for (auto radio : {Radio::ble, Radio::wifi}) {
        const auto valid = rule(radio, {{Field::rid_valid, "true"}});
        const auto specific = rule(
            radio, {{Field::rid_basic_id_type, "1"}, {Field::rid_basic_id, "WI-KNOW-TEST-001"}});
        const auto position = rule(
            radio, {{Field::rid_message_type, "location"}, {Field::rid_position_valid, "true"}});
        auto o = observe(radio, {id, location});
        assert(matches(valid, o) && matches(specific, o) && matches(position, o));
        // The existing serialized observation path preserves matching after bus delivery.
        Report report{};
        report.has_observation = true;
        report.observation = o;
        Packet packet{};
        Report decoded{};
        assert(pack_report(report, packet) && unpack_report(packet, decoded));
        assert(matches(specific, decoded.observation));
        // No implicit joining across messages in a pack, even within the same capture.
        assert(!matches(rule(radio, {{Field::rid_basic_id, "WI-KNOW-TEST-001"},
                                     {Field::rid_position_valid, "true"}}),
                        o));
        assert(!matches(rule(radio, {{Field::rid_basic_id, "WI-KNOW-TEST-001"},
                                     {Field::rid_message_type, "location"}}),
                        o));
        assert(matches(rule(radio,
                            {{Field::rid_basic_id, "wrong"}, {Field::rid_position_valid, "true"}},
                            false),
                       o));
        // Multiple Basic IDs cannot lend their type to another ID value.
        assert(
            !matches(specific, observe(radio, {basic("OTHER", 1), basic("WI-KNOW-TEST-001", 2)})));
        assert(!matches(specific, observe(radio, {location}))); // no cached ID from prior capture
        assert(!matches(position, observe(radio, {id})));       // no cached location
        assert(!matches(valid, Observation{}));
        auto disabled = valid;
        disabled.enabled = false;
        assert(!matches(disabled, o));
        auto unsupported = id;
        unsupported[0] = 3;
        assert(!matches(valid, observe(radio, {id, unsupported})));
        unsupported[0] = 0x62;
        assert(!matches(valid, observe(radio, {unsupported})));
        auto bad = o;
        --bad.payload_size;
        assert(!matches(valid, bad));
        bad = o;
        bad.payload[radio == Radio::ble ? 2 : 0] = 0xab;
        assert(!matches(valid, bad));
        // Generic RID UUID alone is not a structurally valid decoded message.
        const uint8_t uuid_only[]{3, 3, 0xfa, 0xff};
        Observation unrelated{};
        assert(parse_ble(uuid_only, sizeof(uuid_only), unrelated));
        assert(!matches(rule(Radio::ble, {{Field::rid_valid, "true"}}), unrelated));
        assert(matches(rule(Radio::ble, {{Field::service_uuid16, "FFFA"}}), unrelated));
        assert(!matches(
            rule(Radio::ble, {{Field::service_uuid16, "FFFA"}, {Field::rid_valid, "true"}}),
            unrelated));
        // All/any combine ordinary and RID predicates as explicitly configured.
        assert(matches(
            rule(Radio::ble, {{Field::service_uuid16, "FFFA"}, {Field::rid_valid, "true"}}, false),
            unrelated));
        auto zero = location;
        put32(zero, 5, 0);
        put32(zero, 9, 0);
        assert(!matches(position, observe(radio, {zero})));
        put32(zero, 5, 1); // equator/prime-meridian individually are real locations
        assert(matches(position, observe(radio, {zero})));
        put32(zero, 5, 900000001);
        assert(!matches(position, observe(radio, {zero})));
        zero = location;
        put32(zero, 9, uint32_t(-1800000001));
        assert(!matches(position, observe(radio, {zero})));
        zero = location;
        zero[0] = 0x42; // operator position never satisfies aircraft position
        assert(!matches(position, observe(radio, {zero})));
        assert(!matches(specific, observe(radio, {basic("WI-KNOW-TEST-001X")})));
        assert(!matches(specific, observe(radio, {basic("wi-know-test-001")})));
        assert(!matches(specific, observe(radio, {basic("WI-KNOW-TEST-001", 3)})));
        auto suffix = id;
        suffix[21] = 'x'; // Reject hidden nonzero bytes after terminator.
        assert(!matches(specific, observe(radio, {suffix})));
        assert(matches(rule(radio, {{Field::rid_basic_id, "12345678901234567890"}}),
                       observe(radio, {basic("12345678901234567890")})));
        for (unsigned type = 0; type <= 5; ++type) {
            constexpr std::string_view names[]{"basic_id", "location", "authentication",
                                               "self_id",  "system",   "operator_id"};
            Message m{};
            m[0] = uint8_t(type << 4 | 2);
            assert(matches(rule(radio, {{Field::rid_message_type, names[type]}}),
                           observe(radio, {m})));
        }
        // The shared rule pipeline retains priority, action flags, classification, and cooldown.
        Rule rules[]{valid, specific};
        rules[1].id = "test-specific";
        rules[1].priority = 150;
        auto best = best_match(rules, 2, o);
        assert(best.rule == &rules[1] && best.rule->buzzer && best.rule->follow);
        Tracker tracker;
        AlertCooldown cooldown;
        Gps gps;
        char json[log_line_capacity];
        auto &first = tracker.observe(o, {});
        assert(cooldown.request(100, 10000));
        assert(observation_json(o, first, best, {}, gps, json, sizeof(json)));
        assert(std::strstr(json, "test-specific") && std::strstr(json, "broadcast"));
        unsigned classifications = 0;
        for (const auto &r : rules) {
            if (matches(r, o)) {
                assert(observation_json(o, first, Match{&r}, {}, gps, json, sizeof(json)));
                assert(std::strstr(json, "\"sequence\":3"));
                ++classifications;
            }
        }
        assert(classifications == 2 && first.count == 1);
        ++o.sequence;
        o.at_ms = 200;
        auto &continued = tracker.observe(o, {});
        assert(matches(specific, o) && continued.count == 2);
        assert(!cooldown.request(200, 10000));
        assert(!cooldown.request(10099, 10000));
        assert(cooldown.request(10100, 10000));
        assert(!cooldown.request(100, 10000)); // stale time cannot underflow the cooldown
        assert(!cooldown.request(10100, 10000));
    }
}
