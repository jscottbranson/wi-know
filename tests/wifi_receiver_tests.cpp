// Original synthetic MAC headers; no captured client data or upstream code.
#include "settings.hpp"
#include "wiknow/core.hpp"
#include "wiknow/remote_id.hpp"
#include "wiknow/wifi_capture.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>
using namespace wiknow;
using Bytes = std::vector<uint8_t>;
static const std::array<uint8_t, 6> ra{0xb4, 0x1e, 0x52, 1, 2, 3};
static const std::array<uint8_t, 6> ta{0x10, 0x11, 0x12, 4, 5, 6};
static const std::array<uint8_t, 6> third{0x20, 0x21, 0x22, 7, 8, 9};
static Bytes frame(uint8_t ds = 0, bool qos = false, size_t body = 0) {
    Bytes bytes((qos ? 26 : 24) + body, 0);
    bytes[0] = qos ? 0x88 : 0x08;
    bytes[1] = ds;
    std::copy(ra.begin(), ra.end(), bytes.begin() + 4);
    std::copy(ta.begin(), ta.end(), bytes.begin() + 10);
    std::copy(third.begin(), third.end(), bytes.begin() + 16);
    return bytes;
}
static Observation parse(const Bytes &bytes) {
    Observation o{};
    o.rssi = -42;
    assert(parse_wifi(bytes.data(), bytes.size(), o));
    return o;
}
static Rule rule(Field field, std::string_view value) {
    Rule r{};
    r.radio = Radio::wifi;
    r.enabled = true;
    r.predicates[0] = {field, value};
    r.predicate_count = 1;
    return r;
}
int main() {
    const auto receiver = rule(Field::wifi_receiver_oui, "b4:1e:52");
    const auto exact = rule(Field::wifi_receiver_mac, "b4:1e:52:01:02:03");
    for (bool qos : {false, true})
        for (uint8_t ds : {0, 1, 2}) {
            auto bytes = frame(ds, qos, 1500);
            for (bool protected_body : {false, true}) {
                bytes[1] = ds | (protected_body ? 0x40 : 0);
                assert(wifi_capture_size(bytes.data(), bytes.size() + 4, true) == (qos ? 26 : 24));
                auto o = parse(bytes);
                assert(o.address == ta && o.rssi == -42 && o.payload_size == 0);
                assert(o.ssid[0] == 0 && o.name[0] == 0 && remote_id_messages(o).count == 0);
                assert(matches(receiver, o) && matches(exact, o));
                assert(!matches(rule(Field::oui, "b4:1e:52"), o));
                assert(matches(rule(Field::oui, "10:11:12"), o));
                assert(matches(rule(Field::wifi_subtype, qos ? "qos_data" : "data"), o));
                WifiAddresses a{};
                assert(wifi_addresses(o, a));
                assert(a.receiver == ra && a.transmitter == ta);
                assert(a.source == (ds == 2 ? third : ta));
                assert(a.destination == (ds == 1 ? third : ra));
                assert(a.bssid == (ds == 1 ? ra : ds == 2 ? ta : third));
                Report report{};
                report.has_observation = true;
                report.observation = o;
                Packet packet{};
                assert(pack_report(report, packet) && packet.size == 132);
                Report restored{};
                assert(unpack_report(packet, restored));
                assert(matches(receiver, restored.observation));
                assert(restored.observation.wifi_header == o.wifi_header);
                // Corrupted or forged header/metadata must not generate address evidence.
                auto bad = packet;
                bad.payload[116] ^= 1; // TA byte in retained header
                assert(!unpack_report(bad, restored));
                bad = packet;
                bad.payload[105] = 27;
                assert(!unpack_report(bad, restored));
                bad = packet;
                bad.payload[104] = 8;
                assert(!unpack_report(bad, restored));
                bad = packet;
                bad.payload[105] = 0;
                std::fill(bad.payload.begin() + 106, bad.payload.begin() + 132, 0);
                assert(!unpack_report(bad, restored));
                bad = packet;
                bad.payload[27] = 1;
                assert(!unpack_report(bad, restored));
                bad = packet;
                bad.payload[102] = 1;
                bad.payload[132] = 0;
                ++bad.size;
                assert(!unpack_report(bad, restored));
                auto inconsistent = o;
                inconsistent.wifi_header_size = 0;
                report.observation = inconsistent;
                assert(!pack_report(report, packet));
                assert(!matches(receiver, inconsistent));
                Tracker tracker;
                auto &track = tracker.observe(o, {});
                Gps gps;
                char json[log_line_capacity];
                assert(observation_json(o, track, {&receiver}, {}, gps, json, sizeof(json)));
                assert(std::strstr(json, "\"match_evidence\":\"indirect_receiver\""));
                assert(std::strstr(json, "\"rssi_role\":\"transmitter\""));
                assert(std::strstr(json, "\"address\":\"10:11:12:04:05:06\""));
                assert(std::strstr(json, "\"wifi_receiver\":\"b4:1e:52:01:02:03\""));
                assert(std::strstr(json, "\"wifi_payload_format\":\"header_only_v1\""));
            }
        }
    // Explicitly reject unsupported forms, rather than guessing address roles.
    for (bool qos : {false, true}) {
        auto original = frame(0, qos);
        for (uint8_t flags : {3, 4, 8, 0x80}) {
            auto bytes = original;
            bytes[1] = flags;
            Observation o{};
            assert(!parse_wifi(bytes.data(), bytes.size(), o));
            assert(wifi_capture_size(bytes.data(), bytes.size() + 4, true) == 0);
        }
        auto bytes = original;
        bytes[22] = 1;
        Observation o{};
        assert(!parse_wifi(bytes.data(), bytes.size(), o));
        for (size_t n = 0; n < original.size(); ++n)
            assert(!parse_wifi(original.data(), n, o));
        for (uint8_t subtype : {0x18, 0x48, 0xc8, 0x09}) {
            bytes = original;
            bytes[0] = subtype;
            assert(!parse_wifi(bytes.data(), bytes.size(), o));
        }
        if (qos) {
            bytes = original;
            bytes[24] = 0x80;
            assert(!parse_wifi(bytes.data(), bytes.size(), o));
        }
    }
    auto bytes = frame();
    for (uint8_t first : {0xff, 0x01, 0x33}) {
        bytes[4] = first;
        auto o = parse(bytes);
        assert(!matches(receiver, o));
        assert(!matches(rule(Field::wifi_receiver_oui, "ff:1e:52"), o));
    }
    bytes[4] = 0xb6; // exact local address allowed, OUI inference forbidden
    auto local = parse(bytes);
    assert(!matches(rule(Field::wifi_receiver_oui, "b6:1e:52"), local));
    assert(matches(rule(Field::wifi_receiver_mac, "b6:1e:52:01:02:03"), local));
    assert(!matches(rule(Field::wifi_receiver_mac, "b6-1e-52-01-02-03"), local));
    std::fill(bytes.begin() + 4, bytes.begin() + 10, 0);
    assert(!matches(rule(Field::wifi_receiver_mac, "00:00:00:00:00:00"), parse(bytes)));
    // Directed probe responses retain receiver evidence without changing SSID/RID parsing.
    bytes = frame();
    bytes[0] = 0x50;
    bytes.resize(36);
    auto response = parse(bytes);
    assert(matches(receiver, response));
    assert(response.wifi_subtype == WifiSubtype::probe_response);
    Observation reused = response;
    const uint8_t empty_ad[]{0};
    assert(parse_ble(empty_ad, sizeof(empty_ad), reused));
    assert(!reused.wifi_header_size && !matches(receiver, reused));
    Rule rules[]{receiver, rule(Field::oui, "10:11:12")};
    rules[0].priority = 200;
    rules[1].priority = 10;
    assert(best_match(rules, 2, response).rule == &rules[0]);
    assert(best_direct_match(rules, 2, response).rule == &rules[1]);
    rules[0].require_all = false;
    rules[0].predicates[1] = {Field::oui, "10:11:12"};
    rules[0].predicate_count = 2;
    assert(indirect_rule(rules[0])); // mixed any rules remain conservatively indirect
    assert(!best_direct_match(rules, 1, response).rule);
    for (const auto &r : config::rules)
        if (r.id.substr(0, 15) == "flock-receiver-") {
            assert(r.confidence == "possible" && !r.follow && indirect_rule(r));
            auto data = parse(frame());
            assert(matches(r, data) == (r.id == "flock-receiver-data"));
            data.wifi_header[4] = 0x00;
            assert(!matches(r, data));
        }
    WifiDataAdmission admission;
    assert(!admission.accept(0, 4));
    assert(admission.accept(0, 8));
    assert(!admission.accept(49, 8));
    assert(admission.accept(50, 5));
    assert(admission.accept(10, 8)); // clock reset
    assert(!wifi_capture_size(nullptr, 100, true));
    bytes = frame();
    assert(!wifi_capture_size(bytes.data(), 28, false));
}
