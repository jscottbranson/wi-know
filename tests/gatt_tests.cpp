#include "wiknow/gatt_discovery.hpp"
#include "wiknow/observation_queue.hpp"
#include "wiknow/remote_id.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
using namespace wiknow;
static Rule detector() {
    Rule r{};
    r.radio = Radio::ble;
    r.enabled = true;
    r.require_all = true;
    r.predicates[0] = {Field::mac, "12:34:56:78:9a:bc"};
    r.predicates[1] = {Field::gatt_service_uuid128, "00112233-4455-6677-8899-aabbccddeeff"};
    r.predicate_count = 2;
    return r;
}
int main() {
    GattLifecycle life;
    assert(!life.busy());
    assert(life.begin(100, 1000));
    auto token = life.token();
    assert(!life.begin(100, 1000));  // one concurrent attempt
    assert(!life.service(token, 1)); // no service evidence before connection
    assert(!life.connected(token - 1, 1, 100, true));
    assert(life.connected(token, 1, 101, true));
    assert(life.service(token, 1) && !life.service(token, 2));
    assert(!life.expired(1099) && life.expired(1100));
    life.stop(1100); // timeout or scheduler boundary
    assert(life.busy() && !life.service(token, 1));
    assert(!life.begin(1100, 1000)); // terminate ACK cannot release session
    life.stop(1200);                 // retries must not extend the watchdog
    assert(!life.request_reset(3099));
    assert(life.request_reset(3100));
    assert(!life.request_reset(3101)); // reset requested only once
    life.finish();                     // host reset, remote disconnect, or connection failure
    assert(!life.current(token) && !life.busy());
    assert(life.begin(3200, 1000));
    auto second = life.token();
    assert(!life.connected(token, 1, 3201, true)); // late old success, reused handle
    assert(life.connected(second, 1, 3201, true));
    assert(!life.service(token, 1) && life.service(second, 1));
    life.finish(); // disconnect restores eligibility for passive scanning
    assert(!life.busy());
    assert(life.begin(4000, 1000));
    token = life.token();
    life.stop(4001); // connect cancellation races a success callback
    assert(!life.connected(token, 2, 4002, true));
    assert(life.handle() == 2 && life.busy()); // caller must terminate the live handle
    assert(!life.service(token, 2));
    life.finish();
    assert(life.begin(5000, 1000));
    assert(!life.connected(life.token(), 3, 6000, true)); // expired success
    life.stop(6000);
    life.finish();
    assert(life.begin(7000, 1000));
    assert(!life.connected(life.token(), 4, 7001, false)); // Wi-Fi phase now desired
    life.stop(7001);
    life.finish();

    auto r = detector();
    Observation ad{};
    ad.radio = Radio::ble;
    ad.address = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
    ad.rssi = -55;
    ad.at_ms = 100;
    const DeviceIdentity id{Radio::ble, 0, ad.address};
    GattSchedule s(&id, 1, 60000);
    assert(!s.offer(ad, false, 100, &r, 1));
    r.enabled = false;
    assert(!s.offer(ad, true, 100, &r, 1));
    r.enabled = true;
    auto wrong = ad;
    wrong.address_type = 1;
    assert(!s.offer(wrong, true, 100, &r, 1));
    wrong.address[5]++;
    assert(!s.offer(wrong, true, 100, &r, 1));
    assert(s.offer(ad, true, 100, &r, 1));
    assert(!s.offer(ad, true, 100, &r, 1));
    Observation out;
    assert(s.take(1100, out) && out.address == ad.address);
    assert(!s.take(1100, out));
    assert(!s.offer(ad, true, 100, &r, 1));
    assert(!s.offer(ad, true, 99, &r, 1));
    assert(!s.offer(ad, true, 60099, &r, 1));
    assert(s.offer(ad, true, 60100, &r, 1));
    assert(!s.take(61101, out)); // stale candidate must never connect
    assert(s.offer(ad, true, 120100, &r, 1));
    s.cancel();
    assert(!s.take(120101, out));
    assert(!s.offer(ad, true, 120101, &r, 1)); // cancellation keeps cooldown
    GattSchedule empty(nullptr, 0, 60000);
    assert(!empty.offer(ad, true, 0, &r, 1));
    r.require_all = false;
    assert(!gatt_candidate(&r, 1, ad));
    r = detector();
    r.predicates[1].field = Field::service_uuid128;
    assert(!gatt_candidate(&r, 1, ad));
    r = detector();
    // Rotate a random address: no identity association with a configured static target.
    auto random = ad;
    random.address[0] = 0xd2;
    random.address_type = 1;
    const DeviceIdentity static_id{Radio::ble, 1, random.address};
    GattSchedule randoms(&static_id, 1, 60000);
    r.predicates[0].value = "d2:34:56:78:9a:bc";
    assert(randoms.offer(random, true, 0, &r, 1));
    randoms.cancel();
    random.address[0] = 0x52;
    assert(!randoms.offer(random, true, 60000, &r, 1));
    r = detector();
    uint8_t advertised[]{17,   7,    0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99,
                         0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0};
    assert(parse_ble(advertised, sizeof(advertised), ad));
    assert(!matches(r, ad));
    auto discovered = ad;
    discovered.ble_evidence = BleEvidence::gatt_primary_services;
    discovered.payload_size = 16;
    for (unsigned i = 0; i < 16; ++i)
        discovered.payload[i] = uint8_t(i * 17);
    assert(matches(r, discovered));
    r.predicates[1].field = Field::service_uuid128;
    assert(!matches(r, discovered) && matches(r, ad));
    r = detector();
    assert(remote_id_messages(discovered).count == 0);
    auto malformed = discovered;
    malformed.payload_size = 17;
    assert(!matches(r, malformed));
    malformed = discovered;
    std::reverse(malformed.payload.begin(), malformed.payload.begin() + 16);
    assert(!matches(r, malformed));
    malformed = discovered;
    malformed.payload_size = 256;
    assert(!matches(r, malformed));
    auto generic = discovered;
    generic.payload.fill(0);
    assert(!matches(r, generic));
    Report report{};
    report.role = Role::follower;
    report.has_observation = true;
    report.observation = discovered;
    report.capture_age_ms = 123;
    Packet p{};
    assert(pack_report(report, p));
    Report forwarded;
    assert(unpack_report(p, forwarded));
    assert(matches(r, forwarded.observation));
    assert(forwarded.capture_age_ms == 123);
    assert(forwarded.observation.ble_evidence == BleEvidence::gatt_primary_services);
    p.payload[132] = 2;
    assert(!unpack_report(p, forwarded));
    report.observation.radio = Radio::wifi;
    assert(!pack_report(report, p));
    report.observation = malformed;
    assert(!pack_report(report, p));
    RetainedObservation retained;
    assert(retained.retain(discovered));
    retained.report(report, 500);
    assert(report.observation.ble_evidence == discovered.ble_evidence);
    ObservationQueue queue;
    assert(queue.push(discovered, true) && queue.pop(out));
    assert(matches(r, out));
    Tracker tracker;
    const auto &track = tracker.observe(discovered, {});
    Gps gps;
    char json[log_line_capacity];
    assert(observation_json(discovered, track, {&r}, {}, gps, json, sizeof(json)));
    assert(std::strstr(json, "\"ble_ad_hex\":\"\""));
    assert(std::strstr(json, "connected_primary_discovery"));
    assert(std::strstr(json, "\"gatt_services_hex\":\"00112233445566778899aabbccddeeff\""));
    FollowingDetector following({true, 1, 0, 10000}, nullptr, 0);
    double distance{};
    assert(!following.observe(discovered, {true, 42, -83, 100}, distance));
    discovered.at_ms += 5000;
    assert(!following.observe(discovered, {true, 43, -83, 5100}, distance));
    // Connected RSSI must never change the passive trend/peak of the same address.
    Tracker isolated;
    ad.at_ms = 100;
    ad.rssi = -70;
    isolated.observe(ad, {});
    auto strong = discovered;
    strong.at_ms = 200;
    strong.rssi = -10;
    isolated.observe(strong, {});
    ad.at_ms = 300;
    ad.rssi = -60;
    const auto &passive = isolated.observe(ad, {});
    assert(passive.count == 2 && passive.peak.rssi == -60);
    assert(!same_signal(ad, strong));
    // Even failed parses clear stale acquisition state before any reuse.
    auto wifi = discovered;
    assert(!parse_wifi(nullptr, 0, wifi));
    assert(wifi.ble_evidence == BleEvidence::advertisement);
    // Reusing an output object for advertising clears acquisition provenance.
    assert(parse_ble(advertised, sizeof(advertised), discovered));
    assert(discovered.ble_evidence == BleEvidence::advertisement);
}
