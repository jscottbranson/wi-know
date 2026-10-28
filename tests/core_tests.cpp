#include "settings.hpp"
#include "wiknow/core.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
using namespace wiknow;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d failed: %s\n", __FILE__, __LINE__, #condition);            \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)
static std::string nmea(const std::string &body) {
    unsigned sum = 0;
    for (unsigned char c : body)
        sum ^= c;
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "*%02X\r\n", sum);
    return "$" + body + suffix;
}
static bool feed(Gps &gps, const std::string &line, uint64_t at) {
    bool sync = false;
    for (char c : line)
        sync = gps.feed(c, at) || sync;
    return sync;
}
static Observation obs(int rssi, uint64_t at) {
    Observation o{};
    o.node = 1;
    o.boot = 10;
    o.radio = Radio::wifi;
    o.address = {0, 1, 2, 3, 4, 5};
    o.channel = 1;
    o.rssi = int8_t(rssi);
    o.at_ms = at;
    return o;
}
static void membership_test() {
    Membership m;
    Channel channels[] = {{1}, {6}, {11}, {36}, {40}};
    CHECK(m.plan(channels, 5).uncovered_count == 5);
    CHECK(m.seen(Node{2, 1, Role::child, true, true}, 0));
    CHECK(m.seen(Node{1, 1, Role::child, true, true}, 0));
    CHECK(m.seen(Node{3, 1, Role::follower, true, true}, 0));
    auto p = m.plan(channels, 5);
    CHECK(p.count == 2);
    CHECK(p.uncovered_count == 0);
    CHECK(p.assignments[0].node == 1);
    CHECK(p.assignments[0].count == 3);
    CHECK(p.assignments[1].count == 2);
    CHECK(!m.seen(Node{1, 1, Role::child, true, true}, 4000));
    CHECK(m.expire(5000, 5000));
    p = m.plan(channels, 5);
    CHECK(p.count == 1);
    CHECK(p.assignments[0].count == 5);
    m.seen(Node{1, 2, Role::child, true, false}, 5000);
    p = m.plan(channels, 5);
    CHECK(p.uncovered_count == 2);
    m.seen(Node{2, 1, Role::child, false, true}, 5000);
    p = m.plan(channels, 5);
    CHECK(p.uncovered_count == 0);
}
static void parser_match_test() {
    Observation o = obs(-50, 100);
    uint8_t frame[64]{};
    frame[0] = 0x80;
    frame[10] = 0;
    frame[11] = 0x11;
    frame[12] = 0x22;
    frame[36] = 0;
    frame[37] = 4;
    std::memcpy(frame + 38, "TEST", 4);
    CHECK(parse_wifi(frame, 42, o));
    CHECK(std::strcmp(o.ssid.data(), "TEST") == 0);
    CHECK(!parse_wifi(frame, 41, o));
    CHECK(!parse_wifi(frame, 20, o));
    CHECK(parse_wifi(frame, 42, o));
    Rule rule{};
    rule.id = "fixture";
    rule.radio = Radio::wifi;
    rule.enabled = true;
    rule.predicates[0] = {Field::oui, "00:11:22"};
    rule.predicates[1] = {Field::ssid_prefix, "TEST"};
    rule.predicate_count = 2;
    CHECK(matches(rule, o));
    o.address[0] = 2;
    CHECK(!matches(rule, o));
    rule.require_all = false;
    CHECK(matches(rule, o));
    rule.enabled = false;
    CHECK(!matches(rule, o));
    uint8_t ad[] = {5, 9, 'N', 'A', 'M', 'E', 3, 3, 0xfa, 0xff, 4, 0xff, 0x12, 0x34, 0x56};
    CHECK(parse_ble(ad, sizeof(ad), o));
    CHECK(std::strcmp(o.name.data(), "NAME") == 0);
    rule.radio = Radio::ble;
    rule.enabled = true;
    rule.require_all = true;
    rule.predicates[0] = {Field::service_uuid16, "FFFA"};
    rule.predicates[1] = {Field::manufacturer_prefix, "12:34"};
    CHECK(matches(rule, o));
    rule.predicates[1] = {Field::manufacturer_prefix, "12:35"};
    CHECK(!matches(rule, o));
    CHECK(!parse_ble(ad, sizeof(ad) - 1, o));
    for (size_t n = 0; n < sizeof(ad); ++n) {
        Observation scratch{};
        parse_ble(ad, n, scratch);
    }
}
static void mac_test() {
    Rule rule{};
    rule.id = "exact-address";
    rule.category = "lab";
    rule.enabled = true;
    rule.priority = 100;
    rule.buzzer = rule.follow = true;
    rule.predicate_count = 1;
    rule.predicates[0] = {Field::mac, "02:ab:34:cd:56:ef"};
    auto o = obs(-50, 100);
    o.address = {2, 0xab, 0x34, 0xcd, 0x56, 0xef};
    for (Radio radio : {Radio::wifi, Radio::ble}) {
        rule.radio = o.radio = radio;
        for (uint8_t type : {0, 1}) {
            if (radio == Radio::wifi && type != 0)
                continue;
            o.address_type = type;
            CHECK(matches(rule, o));
            for (size_t i = 0; i < o.address.size(); ++i) {
                o.address[i] ^= 1;
                CHECK(!matches(rule, o)); // includes same-OUI near misses
                o.address[i] ^= 1;
            }
            rule.predicates[0].value = "02:AB:34:CD:56:EF";
            CHECK(matches(rule, o));
            rule.predicates[0] = {Field::oui, "02:ab:34"};
            CHECK(matches(rule, o) == (radio == Radio::ble && type == 0));
            rule.predicates[0] = {Field::mac, "02:ab:34:cd:56:ef"};
            Rule choices[] = {rule, rule};
            choices[0].priority = 1;
            choices[0].buzzer = choices[0].follow = false;
            CHECK(best_match(choices, 2, o).rule == &choices[1]);
            // Exercise the exact retained worker -> wire -> parent path.
            RetainedObservation retained;
            CHECK(retained.retain(o));
            Report report{};
            retained.report(report, 150);
            Packet packet{};
            CHECK(pack_report(report, packet));
            uint8_t bytes[max_frame]{};
            const auto n = encode_packet(packet, bytes, sizeof(bytes));
            CHECK(n > 0 && bytes[2] == protocol_version);
            Decoder decoder;
            Packet decoded{};
            for (size_t i = 0; i < n; ++i)
                CHECK(decoder.feed(bytes[i], decoded) == (i + 1 == n));
            Report forwarded{};
            CHECK(unpack_report(decoded, forwarded));
            CHECK(forwarded.observation.address == o.address);
            CHECK(forwarded.observation.address_type == o.address_type);
            CHECK(best_match(choices, 2, forwarded.observation).rule == &choices[1]);
            CHECK(best_match(choices, 2, forwarded.observation).rule->buzzer);
            CHECK(best_match(choices, 2, forwarded.observation).rule->follow);
            Tracker tracker;
            Gps gps;
            auto &track = tracker.observe(forwarded.observation, {});
            char json[log_line_capacity];
            CHECK(observation_json(forwarded.observation, track, {&rule}, {}, gps,
                                   json, sizeof(json)));
            CHECK(std::strstr(json, "\"address\":\"02:ab:34:cd:56:ef\""));
            CHECK(std::strstr(json, "\"rule_id\":\"exact-address\""));
        }
    }
    for (const char *value : {"", "02:ab:34", "02:ab:34:cd:56", "02:ab:34:cd:56:ef:00",
                              "02:ab:34:cd:56:eg", "02:ab:34:cd:56:*", "02:ab:34:cd:56:ef/48",
                              "02ab34cd56ef", "02-ab-34-cd-56-ef", "02 ab 34 cd 56 ef",
                              " 02:ab:34:cd:56:ef", "02:ab:34:cd:56:ef ", "0:2ab:34:cd:56:ef"}) {
        rule.predicates[0].value = value;
        CHECK(!matches(rule, o));
    }
    rule.predicates[0].value = "02:ab:34:cd:56:ef";
    rule.predicates[1] = {Field::name_prefix, "fixture"};
    rule.predicate_count = 2;
    CHECK(!matches(rule, o));
    rule.require_all = false;
    CHECK(matches(rule, o));
    std::strcpy(o.name.data(), "fixture");
    rule.require_all = true;
    CHECK(matches(rule, o));
    o.address[5] ^= 1;
    CHECK(!matches(rule, o));
    rule.require_all = false;
    CHECK(matches(rule, o));
    rule.radio = Radio::wifi;
    CHECK(!matches(rule, o));
    // Wi-Fi matching uses transmitter address 2, never receiver or BSSID.
    uint8_t frame[36]{};
    frame[0] = 0x80;
    const uint8_t target[] = {2, 0xab, 0x34, 0xcd, 0x56, 0xef};
    std::memcpy(frame + 4, target, 6);
    std::memcpy(frame + 16, target, 6);
    CHECK(parse_wifi(frame, sizeof(frame), o));
    rule.predicate_count = 1;
    CHECK(!matches(rule, o));
    std::memcpy(frame + 10, target, 6);
    CHECK(parse_wifi(frame, sizeof(frame), o));
    CHECK(matches(rule, o));
}
static void gps_test() {
    Gps g;
    int64_t utc{};
    CHECK(!g.utc(0, utc));
    auto valid = nmea("GNRMC,120000.000,A,4200.000,N,08300.000,W,0.0,0.0,271026,,,A");
    CHECK(feed(g, valid, 1000));
    CHECK(g.utc(1000, utc));
    CHECK(utc == 1793102400000LL);
    CHECK(g.utc(500, utc));
    CHECK(utc == 1793102399500LL);
    auto f = g.nearest(1100, 200);
    CHECK(f.valid && f.latitude == 42.0 && f.longitude == -83.0);
    CHECK(!g.nearest(1500, 200).valid);
    auto corrupted = valid;
    corrupted[10] = '9';
    CHECK(!feed(g, corrupted, 2000));
    CHECK(!feed(g, nmea("GNRMC,120001.000,V,4200.000,N,08300.000,W,0,0,271026,,,A"), 2000));
    CHECK(!feed(g, nmea("GNRMC,120001.000,A,4200.000,N,08300.000,W,0,0,311126,,,A"), 2000));
    CHECK(feed(g, nmea("GNRMC,120002.000,A,9900.000,N,08300.000,W,0,0,271026,,,A"), 3000));
    CHECK(!g.nearest(3000, 100).valid);
    CHECK(!feed(g, "$" + std::string(300, 'x') + "\n", 4000));
    CHECK(feed(g, valid, 5000));
    // Finite but unrepresentable coordinates must be rejected before integer conversion.
    for (const auto &coordinates : {"1e100,N,08300.000,W", "4200.000,N,1e100,W",
                                    "9000.001,N,08300.000,W", "4200.000,N,18000.001,W"}) {
        Gps malformed;
        CHECK(feed(malformed,
                   nmea(std::string("GNRMC,120000.000,A,") + coordinates + ",0,0,271026,,,A"),
                   1000));
        CHECK(!malformed.nearest(1000, 100).valid);
    }
    Gps boundary;
    CHECK(feed(boundary, nmea("GNRMC,120000.000,A,9000.000,N,18000.000,W,0,0,271026,,,A"), 1000));
    CHECK(boundary.nearest(1000, 0).valid);
}
static void gps_tagging_test() {
    Gps gps;
    auto o = obs(-60, 1100);
    CHECK(!gps.tag(o, 3000, 250).valid);
    CHECK(feed(gps, nmea("GNRMC,120000.000,A,4200.000,N,08300.000,W,0,0,271026,,,A"), 1000));
    o.timing_uncertainty_ms = 250;
    auto fix = gps.tag(o, 350, 250);
    CHECK(fix.valid && fix.latitude == 42 && fix.longitude == -83);
    CHECK(!gps.tag(o, 349, 250).valid);
    CHECK(!gps.tag(o, 3000, 249).valid);
    CHECK(!gps.tag(o, 249, 250).valid);
    // Buffered captures can precede a received fix; use capture time, not arrival time.
    o.at_ms = 900;
    CHECK(gps.tag(o, 350, 250).valid);
    o.at_ms = 1100;
    Tracker tracker;
    tracker.observe(o, fix);
    CHECK(feed(gps, nmea("GNRMC,120001.000,A,4300.000,N,08400.000,W,0,0,271026,,,A"), 2000));
    o.at_ms = 2100;
    o.rssi = -70;
    fix = gps.tag(o, 350, 250);
    CHECK(fix.valid && fix.latitude == 43 && fix.longitude == -84);
    auto &track = tracker.observe(o, fix);
    char json[log_line_capacity];
    CHECK(observation_json(o, track, {}, fix, gps, json, sizeof(json)));
    CHECK(std::strstr(json, "\"receiver_fix\":{\"latitude\":43.0000000,\"longitude\":-84.0000000"));
    CHECK(std::strstr(json,
                      "\"peak_receiver_fix\":{\"latitude\":42.0000000,\"longitude\":-83.0000000"));
    CHECK(std::strstr(json, "\"utc_ms\":1793102401100"));
    // A stronger observation with stale GPS must not inherit the previous peak's location.
    o.at_ms = 6000;
    o.rssi = -40;
    fix = gps.tag(o, 3000, 250);
    CHECK(!fix.valid);
    auto &peak = tracker.observe(o, fix);
    CHECK(observation_json(o, peak, {}, fix, gps, json, sizeof(json)));
    CHECK(std::strstr(json, "\"receiver_fix\":null"));
    CHECK(std::strstr(json, "\"peak_receiver_fix\":null"));
}
static void tracking_test() {
    Tracker t(TrendConfig{1000, 3000, 3, 1, 3});
    Fix a{true, 42, -83, 0}, b{true, 43, -84, 1000};
    auto &first = t.observe(obs(-70, 0), a);
    CHECK(first.peak_changed);
    t.observe(obs(-70, 200), a);
    t.observe(obs(-69, 400), a);
    t.observe(obs(-60, 1000), b);
    t.observe(obs(-60, 1200), b);
    t.observe(obs(-60, 1400), b);
    auto &increasing = t.observe(obs(-65, 2000), {});
    CHECK(increasing.trend == Trend::increasing);
    CHECK(increasing.peak.rssi == -60);
    CHECK(increasing.peak_fix.latitude == 43);
    CHECK(!increasing.peak_changed);
    CHECK(t.trend(increasing, 6000) == Trend::unknown);
    auto &same = t.observe(obs(-50, 7000), {});
    CHECK(same.trend == Trend::unknown);
    CHECK(same.peak_changed);
    CHECK(!same.peak_fix.valid);
    auto previous_count = same.count;
    t.observe(obs(-40, 6000), a);
    CHECK(same.count == previous_count + 1 && same.peak.rssi == -40);
    CHECK(same.latest.at_ms == 7000 && same.peak.at_ms == 6000);
    CHECK(same.peak_fix.latitude == a.latitude && same.peak_changed);
    const auto samples = same.sample_count;
    t.observe(obs(-30, 0), b);
    CHECK(same.first_seen == 0 && same.latest.at_ms == 7000);
    CHECK(same.peak.rssi == -30 && same.peak_fix.latitude == b.latitude);
    CHECK(same.sample_count == samples && same.trend == Trend::unknown);
    auto other = obs(-20, 8000);
    other.node = 2;
    auto &second = t.observe(other, a);
    CHECK(second.count == 1 && second.peak.rssi == -20);
    CHECK(same.peak.rssi == -30);
}
static void trend_confirmation_test() {
    Tracker tracker(TrendConfig{1000, 3000, 3, 2, 3});
    Track *current = nullptr;
    for (int window = 0; window < 7; ++window) {
        int strength = window < 4 ? -50 - 10 * window : -80;
        for (int sample = 0; sample < 3; ++sample)
            current = &tracker.observe(obs(strength, uint64_t(window * 1000 + sample * 100)), {});
        if (window == 2)
            CHECK(current->trend == Trend::unknown);
        if (window == 3 || window == 5)
            CHECK(current->trend == Trend::decreasing);
        if (window == 6)
            CHECK(current->trend == Trend::steady);
    }
    int peak = current->peak.rssi;
    tracker.reset_trend(*current);
    CHECK(current->trend == Trend::unknown && current->peak.rssi == peak);
}
static void busy_trend_test() {
    Tracker tracker(TrendConfig{1000, 3000, 3, 1, 3});
    for (uint64_t i = 0; i < 100; ++i)
        tracker.observe(obs(-80, i * 9), {});
    for (uint64_t i = 0; i < 100; ++i)
        tracker.observe(obs(i < 31 ? -80 : -40, 1000 + i * 9), {});
    auto &track = tracker.observe(obs(-40, 2000), {});
    CHECK(track.trend == Trend::increasing);
    CHECK(track.previous_median == -40);
    CHECK(track.count == 201);
    tracker.reset_trend(track);
    CHECK(track.sample_count == 1);
    CHECK(track.rssi_histogram[87] == 1);
}
static void retained_delivery_test() {
    RetainedObservation worker;
    ReportReceipt parent;
    Observation first = obs(-50, 100);
    first.boot = 0;
    first.sequence = 0; // zero is a real identity, not an empty sentinel
    CHECK(worker.retain(first));
    CHECK(!worker.retain(obs(-40, 200)));
    CHECK(!worker.acknowledge({}));
    CHECK(!worker.acknowledge({true, 1, 0}));
    CHECK(!worker.acknowledge({true, 0, 1}));
    Report report{};
    worker.report(report, 150);
    CHECK(report.has_observation && report.capture_age_ms == 50);
    // A corrupt reply never reaches the parent's receipt state. The worker retains it.
    Packet packet{};
    CHECK(pack_report(report, packet));
    uint8_t frame[max_frame]{};
    const size_t length = encode_packet(packet, frame, sizeof(frame));
    CHECK(length > 0 && frame[2] == protocol_version);
    frame[length - 1] ^= 1;
    Decoder decoder;
    Packet decoded{};
    for (size_t i = 0; i < length; ++i)
        CHECK(!decoder.feed(frame[i], decoded));
    CHECK(!parent.acknowledgement().valid && worker.pending());
    worker.report(report, 250); // retry after a lost/corrupt reply uses the original capture
    CHECK(report.observation.at_ms == 100 && report.capture_age_ms == 150);
    CHECK(parent.accept(report.observation));
    CHECK(!parent.accept(report.observation)); // lost ACK: consume only once
    CHECK(worker.pending());
    CHECK(worker.acknowledge(parent.acknowledgement()));
    worker.report(report, 300);
    CHECK(!report.has_observation);
    CHECK(!worker.acknowledge(parent.acknowledgement()));
    // New worker boot may reuse the same sequence.
    first.boot = 1;
    CHECK(worker.retain(first));
    worker.report(report, 300);
    CHECK(parent.accept(report.observation));
    CHECK(!worker.acknowledge({true, 0, 0})); // ACK from before reboot
    CHECK(worker.acknowledge(parent.acknowledgement()));
    // Sequence rollover and priority delivery may both reorder observation serials.
    for (uint32_t serial : {UINT32_MAX, 0u, 7u, 3u}) {
        first.sequence = serial;
        CHECK(worker.retain(first));
        worker.report(report, 400);
        CHECK(parent.accept(report.observation));
        CHECK(!parent.accept(report.observation));
        CHECK(worker.acknowledge(parent.acknowledgement()));
    }
    CHECK(worker.retain(first));
    ReportReceipt restarted_parent;
    worker.report(report, 500);
    CHECK(restarted_parent.accept(report.observation));
    CHECK(worker.acknowledge(restarted_parent.acknowledgement()));
    // Ages saturate instead of wrapping during a long outage.
    CHECK(worker.retain(first));
    worker.report(report, uint64_t(UINT32_MAX) + 1000);
    CHECK(report.capture_age_ms == UINT32_MAX);
}
static void protocol_test() {
    CHECK(sequence_newer(1, 0));
    CHECK(sequence_newer(UINT32_MAX, UINT32_MAX - 1));
    CHECK(sequence_newer(0, UINT32_MAX));
    CHECK(sequence_newer(4, UINT32_MAX - 10));
    CHECK(!sequence_newer(0, 0));
    CHECK(!sequence_newer(UINT32_MAX, 0));
    CHECK(!sequence_newer(UINT32_MAX - 10, 4));
    CHECK(!sequence_newer(0x80000000u, 0));
    CHECK(!sequence_newer(0, 0x80000000u));
    Command c{};
    c.count = 2;
    c.channels[0] = {1};
    c.channels[1] = {36};
    c.follow = true;
    c.ack = {true, 0x12345678u, UINT32_MAX};
    c.target = {0, 1, 2, 3, 4, 5};
    Packet packet{};
    packet.node = 4;
    packet.boot = 42;
    packet.sequence = 9;
    CHECK(pack_command(c, packet));
    uint8_t bytes[max_frame]{};
    size_t n = encode_packet(packet, bytes, sizeof(bytes));
    CHECK(n > 0);
    Decoder decoder;
    Packet decoded{};
    bool complete = false;
    decoder.feed(0x11, decoded);
    for (size_t i = 0; i < n; ++i)
        complete = decoder.feed(bytes[i], decoded) || complete;
    CHECK(complete && decoded.node == 4 && decoded.boot == 42 && decoded.sequence == 9);
    Command result{};
    CHECK(unpack_command(decoded, result));
    CHECK(result.count == 2 && result.channels[1].number == 36 && result.target == c.target);
    CHECK(result.ack.valid && result.ack.boot == c.ack.boot &&
          result.ack.sequence == c.ack.sequence);
    for (uint16_t size = 0; size < decoded.size; ++size) {
        Packet truncated = decoded;
        truncated.size = size;
        CHECK(!unpack_command(truncated, result));
    }
    CHECK(unpack_command(decoded, result));
    // A command may arrive across many worker-loop polling deadlines.
    Decoder fragmented;
    for (size_t begin = 0; begin < n; begin += 7) {
        for (size_t i = begin; i < n && i < begin + 7; ++i)
            CHECK(fragmented.feed(bytes[i], decoded) == (i + 1 == n));
    }
    CHECK(unpack_command(decoded, result));
    bytes[n - 1] ^= 1;
    complete = false;
    for (size_t i = 0; i < n; ++i)
        complete = decoder.feed(bytes[i], decoded) || complete;
    CHECK(!complete && decoder.errors() == 1);
    Report report{};
    report.has_observation = true;
    report.observation = obs(-45, 500);
    report.capture_age_ms = 123;
    CHECK(pack_report(report, packet));
    Report roundtrip{};
    CHECK(unpack_report(packet, roundtrip));
    CHECK(roundtrip.observation.rssi == -45 && roundtrip.capture_age_ms == 123);
    packet.size = 5;
    CHECK(!unpack_report(packet, roundtrip));
    // Every truncation must be rejected without reading beyond bounds.
    CHECK(pack_report(report, packet));
    for (uint16_t size = 0; size < packet.size; ++size) {
        Packet truncated = packet;
        truncated.size = size;
        CHECK(!unpack_report(truncated, roundtrip));
    }
    c.count = 33;
    CHECK(!pack_command(c, packet));
    // Random input streams and parser boundary lengths must remain memory-safe.
    uint32_t random = 123;
    for (int i = 0; i < 100000; ++i) {
        random = random * 1664525 + 1013904223;
        decoder.feed(uint8_t(random >> 24), decoded);
    }
}
static void json_test() {
    Tracker tracker;
    Gps gps;
    auto o = obs(-50, 1000);
    std::strcpy(o.ssid.data(), "quote\"\\");
    auto &t = tracker.observe(o, {});
    char json[1536];
    CHECK(observation_json(o, t, {}, {}, gps, json, sizeof(json)));
    CHECK(std::strstr(json, "\"utc_ms\":null"));
    CHECK(std::strstr(json, "quote\\\"\\\\"));
    std::fputs(json, stdout);
    CHECK(!observation_json(o, t, {}, {}, gps, json, 10));
    // Public Observation values may not have passed through a radio parser.
    o.ssid.fill('S');
    o.name.fill('N');
    Rule rule{};
    rule.enabled = true;
    rule.radio = Radio::wifi;
    rule.predicate_count = 1;
    rule.predicates[0] = {Field::ssid_prefix, "SSS"};
    CHECK(matches(rule, o));
    rule.radio = o.radio = Radio::ble;
    rule.predicates[0] = {Field::name_prefix, "NNN"};
    CHECK(matches(rule, o));
    CHECK(observation_json(o, t, {}, {}, gps, json, sizeof(json)));
    CHECK(std::strstr(json, ("\"ssid\":\"" + std::string(33, 'S') + "\"").c_str()));
    CHECK(std::strstr(json, ("\"name\":\"" + std::string(33, 'N') + "\"").c_str()));
    rule.predicates[0] = {Field::manufacturer_prefix, "AA"};
    o.payload.fill(1);
    o.payload_size = 1000;
    CHECK(!matches(rule, o));
}
static void text_evidence_test() {
    auto o = obs(-50, 1000);
    const uint8_t ad[] = {6, 9, 'C', 'a', 'f', 0xc3, 0xa9};
    CHECK(parse_ble(ad, sizeof(ad), o));
    Rule rule{};
    rule.enabled = true;
    rule.radio = Radio::ble;
    rule.category = "other";
    rule.id = "unicode-name";
    rule.priority = 7;
    rule.predicate_count = 1;
    rule.predicates[0] = {Field::name_prefix, "Caf\xc3\xa9"};
    CHECK(matches(rule, o));
    Tracker tracker;
    Gps gps;
    auto &track = tracker.observe(o, {});
    char json[log_line_capacity];
    CHECK(observation_json(o, track, {&rule}, {}, gps, json, sizeof(json)));
    CHECK(std::strstr(json, "\"name\":\"Caf\xc3\xa9\""));
    CHECK(std::strstr(json, "\"category\":\"other\""));
    CHECK(std::strstr(json, "\"rule_priority\":7"));
    CHECK(std::strstr(json, "\"location_role\":\"receiver\""));
    const uint8_t binary_ad[] = {3, 9, 0xff, 1};
    CHECK(parse_ble(binary_ad, sizeof(binary_ad), o));
    rule.predicates[0] = {Field::name_prefix, "??"};
    CHECK(!matches(rule, o)); // lossy ASCII sanitization formerly caused this false match
    const Fix invalid{true, std::numeric_limits<double>::quiet_NaN(), 0, 1000};
    auto &invalid_track = tracker.observe(o, invalid);
    invalid_track.peak_fix = invalid;
    CHECK(observation_json(o, invalid_track, {}, invalid, gps, json, sizeof(json)));
    CHECK(std::strstr(json, "\"name\":\"\\u00ff\\u0001\""));
    CHECK(std::strstr(json, "\"receiver_fix\":null"));
    CHECK(std::strstr(json, "\"peak_receiver_fix\":null"));
    // Truncated, overlong, surrogate and out-of-range UTF-8 must remain valid JSON.
    for (const auto &invalid_utf8 :
         {std::string("\xc3"), std::string("\xc0\x80"), std::string("\xed\xa0\x80"),
          std::string("\xf4\x90\x80\x80")}) {
        o.name.fill(0);
        std::memcpy(o.name.data(), invalid_utf8.data(), invalid_utf8.size());
        CHECK(observation_json(o, track, {}, {}, gps, json, sizeof(json)));
        CHECK(std::strstr(json, "\"name\":\"\\u00"));
        std::fputs(json, stdout);
    }
    // Invalid Wi-Fi bytes and valid UTF-8 remain distinguishable in shared logs.
    o.ssid.fill(0);
    o.ssid[0] = char(0xff);
    CHECK(observation_json(o, track, {}, {}, gps, json, sizeof(json)));
    CHECK(std::strstr(json, "\"ssid_hex\":\"ff\""));
    o.ssid[0] = char(0xc3);
    o.ssid[1] = char(0xbf);
    CHECK(observation_json(o, track, {}, {}, gps, json, sizeof(json)));
    CHECK(std::strstr(json, "\"ssid_hex\":\"c3bf\""));
    // Maximum bounded metadata, payload, counter values and escaping must fit.
    o.name.fill(1);
    o.name.back() = 0;
    o.ssid = o.name;
    o.payload.fill(0xff);
    o.payload_size = max_payload;
    o.at_ms = UINT64_MAX;
    o.timing_uncertainty_ms = UINT32_MAX;
    o.receiver_boot = o.boot = o.sequence = UINT32_MAX;
    o.node = UINT16_MAX;
    const std::string id(64, 'i');
    rule.id = id;
    const std::string category(40, 'c');
    rule.category = category;
    rule.confidence = "broadcast";
    rule.priority = UINT8_MAX;
    o.channel = UINT8_MAX;
    o.rssi = -127;
    const Fix extreme{true, -90, -180, UINT64_MAX};
    Tracker maximum_tracker;
    auto &max_track = maximum_tracker.observe(o, extreme);
    max_track.count = UINT64_MAX;
    CHECK(feed(gps, nmea("GNRMC,120000.000,A,9000.000,S,18000.000,W,0,0,271026,,,A"),
               UINT64_MAX - 1000000));
    CHECK(observation_json(o, max_track, {&rule}, extreme, gps, json, sizeof(json)));
    std::fputs(json, stdout);
}
static void following_test() {
    DeviceIdentity ignored{Radio::ble, 1, {{1, 2, 3, 4, 5, 6}}};
    FollowingDetector detector({true, 100, 2000, 1500}, &ignored, 1);
    auto o = obs(-50, 1000);
    double distance = 0;
    auto sight = [&](uint64_t time, double latitude, bool valid = true) {
        o.at_ms = time;
        return detector.observe(o, {valid, latitude, 0, time}, distance);
    };
    CHECK(!sight(1000, 0));
    CHECK(!sight(2000, .002)); // crossed 100m: start delay
    CHECK(!sight(3000, .002));
    o.node = 2;
    o.channel = 6; // combine receivers/channels
    CHECK(sight(4000, .002));
    CHECK(distance > 200 && distance < 230);
    CHECK(!sight(5000, .003)); // once per encounter
    CHECK(!sight(7000, .003)); // gap creates a new origin
    CHECK(!sight(8000, .005));
    CHECK(!sight(9000, .003)); // returning inside radius resets timer
    CHECK(!sight(10000, .005));
    CHECK(!sight(11000, .005, false)); // loss of GPS resets encounter
    CHECK(!sight(12000, .005));
    CHECK(!sight(13000, .007));
    CHECK(!sight(12500, .007)); // out-of-order reports ignored
    CHECK(!sight(14000, .007));
    CHECK(sight(15000, .007));
    FollowingDetector disabled({}, nullptr, 0);
    CHECK(!disabled.observe(o, {true, 0, 0, 0}, distance));
    o.radio = ignored.radio;
    o.address_type = ignored.address_type;
    o.address = ignored.address;
    CHECK(!sight(16000, 0));
    CHECK(!sight(17000, .002));
    CHECK(!sight(18000, .002));
    CHECK(!sight(19000, .002));
    FollowingDetector immediate({true, 100, 0, 1500}, nullptr, 0);
    o.at_ms = 1000;
    CHECK(!immediate.observe(o, {true, 0, 0, 1000}, distance));
    o.at_ms = 2000;
    CHECK(immediate.observe(o, {true, .002, 0, 2000}, distance));
}
int main() {
    CHECK(config::channels.size() > 0);
    following_test();
    membership_test();
    parser_match_test();
    mac_test();
    gps_test();
    gps_tagging_test();
    tracking_test();
    trend_confirmation_test();
    busy_trend_test();
    retained_delivery_test();
    protocol_test();
    json_test();
    text_evidence_test();
    std::puts("All core tests passed");
}
