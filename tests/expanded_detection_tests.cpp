#include "settings.hpp"
#include "wiknow/core.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>
using namespace wiknow;
using Bytes = std::vector<uint8_t>;
static Rule rule(std::string_view id) {
    for (auto r : config::rules)
        if (r.id == id)
            return r;
    assert(false);
    return {};
}
static Observation ble(Bytes bytes) {
    Observation o{};
    assert(parse_ble(bytes.data(), bytes.size(), o));
    o.address_type = 1;
    return o;
}
static Bytes service(unsigned id, bool wide = false) {
    if (!wide)
        return {3, 3, uint8_t(id), uint8_t(id >> 8)};
    return {17, 7,    0xfb, 0x34, 0x9b, 0x5f, 0x80,        0,
            0,  0x80, 0,    0x10, 0,    0,    uint8_t(id), uint8_t(id >> 8),
            0,  0};
}
int main() {
    auto tile = rule("tile-services");
    assert(tile.enabled);
    auto unifi = rule("unifi-vendor-wifi-1");
    assert(unifi.enabled);
    for (bool wide : {false, true}) {
        assert(matches(tile, ble(service(0xfeed, wide))));
        assert(matches(tile, ble(service(0xfeec, wide))));
        assert(!matches(tile, ble(service(0xfeee, wide))));
        auto raven = rule("raven-advertised-service-range");
        for (unsigned id : {0x3100, 0x3101, 0x34ff, 0x3500})
            assert(matches(raven, ble(service(id, wide))));
        for (unsigned id : {0x180a, 0x1809, 0x1819, 0x30ff, 0x3501})
            assert(!matches(raven, ble(service(id, wide))));
        auto malformed = service(0x3100, wide);
        malformed[0]++;
        malformed.push_back(0);
        assert(!matches(raven, ble(malformed))); // complete AD, partial UUID
    }
    auto range = rule("raven-advertised-service-range");
    auto vendor = service(0x3100);
    vendor[1] = 0xff;
    assert(!matches(range, ble(vendor)));
    auto fakebase = service(0x3100, true);
    fakebase[2] ^= 1;
    assert(!matches(range, ble(fakebase)));
    auto sd = service(0x3101);
    sd[1] = 0x16;
    sd[0] += 2;
    sd.insert(sd.end(), {0x00, 0x35});
    assert(matches(range, ble(sd)));
    sd[2] = 0x0a;
    sd[3] = 0x18;
    assert(!matches(range, ble(sd))); // UUID bytes in application data do not count
    auto meta = rule("meta-glasses-composite");
    auto bytes = service(0xfd5f);
    assert(!matches(meta, ble(bytes)));
    bytes.insert(bytes.end(), {3, 0xff, 0x53, 0x0d});
    assert(matches(meta, ble(bytes)));
    bytes.back() = 0x0e;
    assert(!matches(meta, ble(bytes)));
    auto findmy = rule("apple-findmy-advertisement");
    assert(findmy.enabled);
    assert(matches(findmy, ble({4, 0xff, 0x4c, 0, 0x12})));
    assert(!matches(findmy, ble({4, 0x16, 0x4c, 0, 0x12})));
    assert(!matches(findmy, ble({4, 0xff, 0x4c, 0, 0x10})));
    Observation named{};
    named.radio = Radio::ble;
    std::strcpy(named.name.data(), "My-RaY-bAn");
    assert(matches(rule("meta-glasses-name-1"), named));
    std::strcpy(named.name.data(), "HC-05-extra");
    assert(!matches(rule("hc-serial-module-name-1"), named));
    std::strcpy(named.name.data(), "HC-05");
    assert(matches(rule("hc-serial-module-name-1"), named));
    Observation wifi{};
    wifi.radio = Radio::wifi;
    wifi.wifi_subtype = WifiSubtype::beacon;
    std::strcpy(wifi.ssid.data(), "TEST_FlCk");
    assert(matches(rule("flock-network-name-2"), wifi));
    wifi.wifi_subtype = WifiSubtype::probe_request;
    assert(!matches(rule("flock-network-name-2"), wifi));
    // Synthetic probe with the upstream ordered tags and exact vendor bytes.
    Bytes ies{0, 0, 2, 1,  0x82, 12,  0, 127, 1, 0, 221,  7,    0x50, 0x6f, 0x9a, 0x16,
              3, 1, 3, 45, 0,    191, 0, 221, 7, 0, 0x50, 0xf2, 8,    0,    0,    0};
    auto probe = rule("flock-community-probe-pattern-1");
    wifi.address = {0xf4, 0x6a, 0xdd, 1, 2, 3};
    auto set_ies = [&] {
        std::copy(ies.begin(), ies.end(), wifi.payload.begin());
        wifi.payload_size = ies.size();
    };
    set_ies();
    assert(matches(probe, wifi));
    Report report{};
    report.has_observation = true;
    report.observation = wifi;
    Packet packet{};
    assert(pack_report(report, packet));
    Report decoded{};
    assert(unpack_report(packet, decoded));
    assert(matches(probe, decoded.observation));
    wifi.address[0] ^= 2;
    assert(!matches(probe, wifi));
    wifi.address[0] ^= 2;
    ies.back() ^= 1;
    set_ies();
    assert(!matches(probe, wifi));
    ies.back() ^= 1;
    ies.insert(ies.end(), {1, 0});
    set_ies();
    assert(!matches(probe, wifi)); // extra ordered IE
    ies.pop_back();
    set_ies();
    assert(!matches(probe, wifi)); // truncated IE
}
