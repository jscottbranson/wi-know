#include "log_helpers.hpp"
#include "wiknow/capture_switch.hpp"
#include <cassert>
#include <cstring>
int main() {
    for (bool base : {false, true})
        for (bool enabled : {false, true})
            for (bool boot_on : {false, true}) {
                wiknow::CaptureSwitch s(enabled, base, 50);
                s.start(boot_on, 0);
                assert(s.on() == (enabled && boot_on));
                assert(s.all() == (base || (enabled && boot_on)));
                // Logging selection is radio-independent; matched records always survive.
                assert(s.retain(true));
                assert(s.retain(false) == s.all());
                assert(!s.update(!boot_on, 10));
                assert(!s.update(boot_on, 40));
                assert(!s.update(!boot_on, 50));
                assert(!s.update(!boot_on, 99));
                assert(s.update(!boot_on, 100) == enabled);
                assert(!s.update(!boot_on, 150));
                assert(s.on() == (enabled && !boot_on));
                assert(s.retain(true));
                assert(s.retain(false) == (base || s.on()));
                assert(!s.update(boot_on, 200));
                assert(s.update(boot_on, 250) == enabled);
                assert(s.on() == (enabled && boot_on));
            }
    wiknow::Gps gps;
    char json[640];
    assert(platform::capture_transition_json(true, true, true, 42, 7, gps, json, sizeof(json)));
    assert(std::strstr(json, "\"at_ms\":42"));
    assert(std::strstr(json, "\"utc_ms\":null"));
    assert(std::strstr(json, "\"receiver_fix\":null"));
    assert(std::strstr(json, "\"reason\":\"startup\""));
    assert(platform::capture_transition_json(false, false, false, 100, 7, gps, json, sizeof(json)));
    assert(std::strstr(json, "\"effective_mode\":\"matches\""));
    assert(!platform::capture_transition_json(false, true, false, 100, 7, gps, json, 2));
    const char *rmc = "GNRMC,120000.000,A,4200.000,N,08300.000,W,0.0,0.0,271026,,,A";
    unsigned checksum = 0;
    for (const char *p = rmc; *p; ++p)
        checksum ^= static_cast<unsigned char>(*p);
    char sentence[128];
    std::snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", rmc, checksum);
    for (const char *p = sentence; *p; ++p)
        gps.feed(*p, 1000);
    assert(platform::capture_transition_json(true, true, false, 2000, 7, gps, json, sizeof(json)));
    assert(std::strstr(json, "\"utc_ms\":1793102401000"));
    assert(std::strstr(json, "\"time_source\":\"gps_holdover\""));
    assert(std::strstr(json, "\"latitude\":42.0000000"));
    gps.expire(4001, 3000);
    assert(
        platform::capture_transition_json(false, false, false, 4001, 7, gps, json, sizeof(json)));
    assert(std::strstr(json, "\"utc_ms\":1793102403001"));
    assert(std::strstr(json, "\"receiver_fix\":null"));
    assert(std::strstr(json, "\"time_source\":\"gps_holdover\""));
    assert(platform::time_sync_json(4001, 7, false, 0, gps, json, sizeof(json)));
    assert(std::strstr(json, "\"correction_ms\":null"));
    assert(std::strstr(json, "\"previous_source\":\"unknown\""));
    assert(platform::time_sync_json(4001, 7, true, 1793102404001LL, gps, json, sizeof(json)));
    assert(std::strstr(json, "\"correction_ms\":-1000"));
    assert(std::strstr(json, "\"at_ms\":4001"));
    assert(platform::time_sync_json(4001, 7, true, 1793102402001LL, gps, json, sizeof(json)));
    assert(std::strstr(json, "\"correction_ms\":1000"));
    assert(!platform::time_sync_json(4001, 7, false, 0, gps, json, 2));
    assert(!platform::time_sync_json(0, 7, false, 0, wiknow::Gps{}, json, sizeof(json)));
}
