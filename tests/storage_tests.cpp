#include "log_helpers.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #condition);                 \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

int main() {
    uint32_t id = 99;
    CHECK(platform::log_file_id("WK00000001.LOG", id) && id == 1);
    CHECK(platform::log_file_id("WK99999999.LOG", id) && id == 99999999);
    // Stray files must not join retention or prevent logging from opening a new file.
    for (const char *name : {"WK1.LOG", "WK0000001.LOG", "WK000000001.LOG", "WK+000001.LOG",
                             "WK 0000001.LOG", "WK-0000001.LOG", "WK00000001.LOG.bak",
                             "WK00000001.log", "WK0000000a.LOG", "WK00000001.TXT", ""})
        CHECK(!platform::log_file_id(name, id));

    wiknow::Observation observation{};
    observation.node = 3;
    observation.boot = 123;
    observation.receiver_boot = 456;
    observation.sequence = 7;
    observation.radio = wiknow::Radio::ble;
    observation.address = {1, 2, 3, 4, 5, 6};
    observation.at_ms = 2000;
    observation.rssi = -42;
    observation.timing_uncertainty_ms = 12;
    wiknow::Gps gps;
    wiknow::Fix fix{true, 42.1, -83.2, 1990};
    char line[1024];
    CHECK(platform::following_alert_json(observation, fix, gps, 200.5, 30000, line, sizeof(line)));
    CHECK(std::strstr(line, "\"event\":\"following_alert\""));
    CHECK(std::strstr(line, "\"node\":3,\"boot\":123,\"receiver_boot\":456,\"sequence\":7"));
    CHECK(std::strstr(line, "\"capture_ms\":2000,\"utc_ms\":null"));
    CHECK(std::strstr(line, "\"address\":\"01:02:03:04:05:06\""));
    CHECK(std::strstr(
        line,
        "\"receiver_fix\":{\"latitude\":42.1000000,\"longitude\":-83.2000000,\"fix_at_ms\":1990}"));
    CHECK(std::strstr(line, "\"timing_uncertainty_ms\":12"));
    CHECK(std::strstr(line, "\"location_role\":\"receiver\""));
    CHECK(std::strstr(line, "\"displacement_m\":200.5,\"delay_ms\":30000"));
    CHECK(line[std::strlen(line) - 1] == '\n');
    CHECK(!platform::following_alert_json(observation, fix, gps, 200, 1, line, 10));
    CHECK(!platform::following_alert_json(observation, fix, gps, 200, 1, nullptr, 0));
    CHECK(!platform::following_alert_json(
        observation, fix, gps, std::numeric_limits<double>::infinity(), 1, line, sizeof(line)));
    fix.valid = false;
    CHECK(platform::following_alert_json(observation, fix, gps, 200, 1, line, sizeof(line)));
    CHECK(std::strstr(line, "\"receiver_fix\":null"));
    fix.valid = true;
    fix.latitude = std::numeric_limits<double>::quiet_NaN();
    CHECK(platform::following_alert_json(observation, fix, gps, 200, 1, line, sizeof(line)));
    CHECK(std::strstr(line, "\"receiver_fix\":null"));
    const char *rmc = "GNRMC,120000.000,A,4200.000,N,08300.000,W,0.0,0.0,271026,,,A";
    unsigned checksum = 0;
    for (const char *p = rmc; *p; ++p)
        checksum ^= static_cast<unsigned char>(*p);
    char sentence[128];
    std::snprintf(sentence, sizeof(sentence), "$%s*%02X\r\n", rmc, checksum);
    for (const char *p = sentence; *p; ++p)
        gps.feed(*p, 1000);
    CHECK(platform::following_alert_json(observation, {}, gps, 200, 1, line, sizeof(line)));
    CHECK(std::strstr(line, "\"utc_ms\":1793102401000"));
    CHECK(std::strstr(line, "\"time_source\":\"gps_holdover\""));
    std::puts("All firmware log tests passed");
}
