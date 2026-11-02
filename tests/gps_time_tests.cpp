#include "wiknow/core.hpp"
#include "wiknow/gps_time_request.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace wiknow;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)

static std::string nmea(const std::string &body) {
    unsigned sum = 0;
    for (unsigned char c : body)
        sum ^= c;
    char tail[8];
    std::snprintf(tail, sizeof(tail), "*%02X\r\n", sum);
    return "$" + body + tail;
}
static bool feed(Gps &gps, const std::string &body, uint64_t at) {
    bool accepted = false;
    for (char c : nmea(body))
        accepted |= gps.feed(c, at);
    return accepted;
}
static const char *rtc = "PCAS60,120000.000,27102026,2442,216018,1,18,1";
static const char *rmc = "GNRMC,120002.000,A,4200.000,N,08300.000,W,0,0,271026,,,A";
static const char *lost = "GNRMC,120003.000,V,,,,,,,271026,,,N";
int main() {
    Gps approximate(3000, true);
    const char *pre_fix = "GNRMC,221032.000,V,,,,,,,021126,,,M,V";
    CHECK(feed(approximate, pre_fix, 1000));
    CHECK(approximate.source(1000) == TimeSource::rtc);
    CHECK(!approximate.has_gps_sync());
    CHECK(!approximate.nearest(1000, 3000).valid);
    int64_t estimated{};
    CHECK(approximate.utc(1000, estimated) && estimated == 1793657432000LL);
    CHECK(!feed(approximate, "PCAS60,221032.000,02112026,,,0,,0", 1001));
    CHECK(approximate.source(1001) == TimeSource::rtc); // invalid TIM cannot erase RMC fallback
    auto bad_rmc = nmea(pre_fix);
    bad_rmc[8] = '9';
    for (char c : bad_rmc)
        approximate.feed(c, 4001);
    CHECK(!approximate.utc(4001, estimated)); // corrupt RMC cannot refresh it
    CHECK(feed(approximate, pre_fix, 5000));
    CHECK(!feed(approximate, "GNRMC,,V,,,,,,,,,,N", 5001));
    CHECK(!approximate.utc(5001, estimated)); // missing date/time revokes estimate
    CHECK(feed(approximate, pre_fix, 6000));
    CHECK(feed(approximate, rmc, 6001));
    CHECK(approximate.source(6001) == TimeSource::gps);
    CHECK(approximate.last_sync() == 6001);
    CHECK(feed(approximate, pre_fix, 6002));
    CHECK(approximate.source(6002) == TimeSource::rtc);
    CHECK(approximate.last_sync() == 6001); // fallback never counts as GPS synchronization
    Gps old_air530;
    CHECK(!feed(old_air530, pre_fix, 0));
    CHECK(old_air530.source(0) == TimeSource::unknown);

    Gps gps(3000, true);
    int64_t utc{};
    char clock[9], json[log_line_capacity];
    CHECK(!gps.utc(0, utc));
    CHECK(feed(gps, rtc, 1000));
    CHECK(gps.source(1000) == TimeSource::rtc);
    CHECK(gps.utc(1500, utc) && utc == 1793102400500LL);
    CHECK(!gps.has_gps_sync());
    CHECK(!gps.nearest(1000, 3000).valid);
    CHECK(gps.utc(2000, utc));
    CHECK(feed(gps, rmc, 2000));
    CHECK(gps.source(2000) == TimeSource::gps);
    feed(gps, "PCAS60,120003.000,27102026,2442,216021,1,18,1", 3000);
    CHECK(gps.source(3000) == TimeSource::gps); // independent lower-priority anchor
    CHECK(gps.last_sync() == 2000);
    CHECK(!feed(gps, lost, 3100));
    CHECK(gps.source(3100) == TimeSource::rtc); // immediate loss, no timeout wait
    CHECK(gps.utc(3100, utc) && utc == 1793102403100LL);
    gps.expire(6000, 3000);
    CHECK(gps.source(6000) == TimeSource::rtc); // inclusive boundary
    gps.expire(6101, 3000);
    CHECK(gps.source(6101) == TimeSource::unknown);
    CHECK(!gps.utc(6101, utc));
    CHECK(!gps.utc(3000, utc)); // old capture cannot revive an unavailable clock
    gps.utc_text(6101, clock);
    CHECK(!std::strcmp(clock, "--:--:--"));
    Observation observation{};
    observation.at_ms = 6101;
    Tracker tracker;
    auto &track = tracker.observe(observation, {});
    CHECK(observation_json(observation, track, {}, {}, gps, json, sizeof(json)));
    CHECK(std::strstr(json, "\"utc_ms\":null"));
    CHECK(std::strstr(json, "\"time_source\":\"unknown\""));
    CHECK(feed(gps, rmc, 7000)); // GPS recovery without RTC
    gps.expire(10001, 3000);
    CHECK(!gps.utc(10001, utc)); // no indefinite software holdover
    CHECK(feed(gps, rtc, 11000));
    CHECK(gps.source(11000) == TimeSource::rtc);
    CHECK(!feed(gps, "PCAS60,120001.000,27102026,,,0,,0", 11001));
    CHECK(!gps.utc(11001, utc)); // explicit invalidity immediately revokes RTC

    for (const char *bad :
         {"PCAS60,120000.000,27102026,,,1,,1", "PCAS60,120000.000,27102026,x,216018,1,18,1",
          "PCAS60,120000.000,27102026,65536,216018,1,18,1",
          "PCAS60,120000.000,27102026,2442,604800,1,18,1",
          "PCAS60,120000.000,27102026,2442,216018,1,999,1",
          "PCAS60,120000.000,27102026,2442,216018,0,18,1",
          "PCAS60,120000.000,27102026,2442,216018,1,18,0",
          "PCAS60,120000.000,27102026,2442,216018,1,18,",
          "PCAS60,120000.000,27102026,2442,216018,1,18,1,extra",
          "PCAS60,120000.000,30022026,2442,216018,1,18,1",
          "PCAS60,120000.000,27101999,2442,216018,1,18,1",
          "PCAS60,120000.000,2710202x,2442,216018,1,18,1",
          "PCAS60,240000.000,27102026,2442,216018,1,18,1",
          "PCAS60,120000e0,27102026,2442,216018,1,18,1", "GNRMC,120000.000,V,,,,,,,311126,,,N",
          "PGKC280,2026,10,27,12,0,0"}) {
        Gps empty(3000, true);
        CHECK(!feed(empty, bad, 0));
        CHECK(!empty.utc(0, utc));
    }
    Gps legacy;
    CHECK(!feed(legacy, rtc, 0)); // never enable Air530Z protocol for Air530
    Gps corrupt(3000, true);
    CHECK(feed(corrupt, rtc, 0));
    auto broken = nmea(rtc);
    broken[10] = '9';
    for (char c : broken)
        corrupt.feed(c, 3001);
    CHECK(!corrupt.utc(3001, utc)); // bad checksums don't extend lifetime
    CHECK(feed(corrupt, "PCAS60,235959.500,28022028,2512,259217,1,18,1", 4000));
    corrupt.utc_text(4500, clock);
    CHECK(!std::strcmp(clock, "00:00:00"));
    CHECK(feed(corrupt, "PCAS60,000000.000,29022028,2512,259218,1,18,1", 4500));
    Gps short_age(100, true);
    CHECK(feed(short_age, rtc, 0));
    CHECK(short_age.utc(100, utc));
    CHECK(!short_age.utc(101, utc));
    CHECK(!short_age.utc(UINT64_MAX, utc));

    // Exact command framing, checksum, RMC/TIM fields, partial FIFO writes and retry budget.
    GpsTimeRequest request;
    size_t size{};
    std::string command = request.pending(0, size);
    CHECK(command == nmea("PCAS03,0,0,0,0,1,0,0,0,0,0,,,0,0,,,,1"));
    CHECK(size == command.size());
    request.sent(0, 7);
    CHECK(std::string(request.pending(0, size)) == command.substr(7));
    request.sent(0, 0);
    CHECK(size == command.size() - 7);
    request.sent(0, size);
    request.pending(999, size);
    CHECK(size == 0);
    for (uint64_t now = 1000; now <= 5000; now += 1000) {
        request.pending(now, size);
        CHECK(size == command.size());
        request.sent(now, size);
    }
    request.pending(34999, size);
    CHECK(size == 0);
    request.pending(35000, size);
    CHECK(size == command.size()); // recovers a later module reset
    GpsTimeRequest fragmented;
    std::string transmitted;
    for (size_t i = 0; i < command.size(); ++i) {
        const char *part = fragmented.pending(0, size);
        CHECK(size == command.size() - i);
        transmitted += *part;
        fragmented.sent(0, 1);
    }
    CHECK(transmitted == command); // split anywhere in the command
    fragmented.pending(999, size);
    CHECK(size == 0);
}
