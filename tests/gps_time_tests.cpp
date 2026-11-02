#include "gps_diagnostics.hpp"
#include "log_helpers.hpp"
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
    platform::GpsDiagnostics diagnostics;
    const auto tim = nmea(rtc);
    for (char c : tim.substr(0, 12))
        diagnostics.feed(c, 100);
    CHECK(diagnostics.tim_sentences == 0);
    for (char c : tim.substr(12))
        diagnostics.feed(c, 200);
    CHECK(diagnostics.tim_sentences == 1 && diagnostics.last_tim_at == 200);
    CHECK(diagnostics.received_bytes == tim.size());
    CHECK(diagnostics.last_tim == tim.substr(0, tim.size() - 2));
    for (char c : nmea(rmc) + "$PCAS60," + std::string(200, 'x') + "\r\n")
        diagnostics.feed(c, 300);
    CHECK(diagnostics.tim_sentences == 1); // ignore other sentences and overflow
    const auto invalid_tim = nmea("PCAS60,120001.000,27102026,,,0,,0");
    for (char c : "$PCAS60,partial" + invalid_tim)
        diagnostics.feed(c, 400);
    CHECK(diagnostics.tim_sentences == 2 && diagnostics.last_tim_at == 400);
    CHECK(diagnostics.last_tim == invalid_tim.substr(0, invalid_tim.size() - 2));

    Gps gps(3000, true);
    int64_t utc{};
    char clock[9], json[log_line_capacity];
    CHECK(!gps.utc(0, utc));
    CHECK(feed(gps, rtc, 1000));
    CHECK(gps.source(1000) == TimeSource::rtc);
    CHECK(gps.utc(1500, utc) && utc == 1793102400500LL);
    CHECK(!gps.has_gps_sync());
    CHECK(!gps.nearest(1000, 3000).valid);
    CHECK(platform::time_sync_json(1000, 1, TimeSource::unknown, 0, gps, json, sizeof(json)));
    CHECK(std::strstr(json, "\"time_source\":\"rtc\""));
    CHECK(std::strstr(json, "\"gps_sync_age_ms\":null"));
    const auto before = gps.source(2000);
    CHECK(gps.utc(2000, utc));
    CHECK(feed(gps, rmc, 2000));
    CHECK(gps.source(2000) == TimeSource::gps);
    CHECK(platform::time_sync_json(2000, 1, before, utc, gps, json, sizeof(json)));
    CHECK(std::strstr(json, "\"previous_source\":\"rtc\""));
    CHECK(std::strstr(json, "\"time_source\":\"gps\""));
    CHECK(std::strstr(json, "\"correction_ms\":1000"));
    CHECK(std::strstr(json, "\"gps_sync_age_ms\":0"));
    feed(gps, "PCAS60,120003.000,27102026,2442,216021,1,18,1", 3000);
    CHECK(gps.source(3000) == TimeSource::gps); // independent lower-priority anchor
    CHECK(gps.last_sync() == 2000);
    CHECK(!feed(gps, lost, 3100));
    CHECK(gps.source(3100) == TimeSource::rtc); // immediate loss, no timeout wait
    CHECK(gps.utc(3100, utc) && utc == 1793102403100LL);
    gps.expire(6000, 3000);
    CHECK(gps.source(6000) == TimeSource::rtc); // inclusive boundary
    gps.expire(6001, 3000);
    CHECK(gps.source(6001) == TimeSource::unknown);
    CHECK(!gps.utc(6001, utc));
    CHECK(!gps.utc(3000, utc)); // old capture cannot revive an unavailable clock
    gps.utc_text(6001, clock);
    CHECK(!std::strcmp(clock, "--:--:--"));
    Observation observation{};
    observation.at_ms = 6001;
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
          "PCAS60,120000e0,27102026,2442,216018,1,18,1", "GNRMC,120000.000,V,,,,,,,271026,,,N",
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

    // Integration event decisions: only selected samples synchronize; invalidity
    // and silence produce explicit source transitions, including null UTC.
    platform::GpsTimeEvents events;
    Gps logged(3000, true);
    auto input = [&](const char *body, uint64_t now) {
        int emitted = 0;
        if (events.update(now, 1, logged, false, 0, json, sizeof(json)))
            ++emitted;
        for (char c : nmea(body)) {
            int64_t previous{};
            logged.utc(now, previous);
            bool sample = logged.feed(c, now);
            if (events.update(now, 1, logged, sample, previous, json, sizeof(json)))
                ++emitted;
        }
        return emitted;
    };
    CHECK(input(rtc, 1000) == 1);
    CHECK(std::strstr(json, "\"time_source\":\"rtc\""));
    CHECK(input(rmc, 2000) == 1);
    CHECK(std::strstr(json, "\"previous_source\":\"rtc\""));
    CHECK(input(rtc, 3000) == 0); // must not manufacture a GPS sync
    CHECK(input(lost, 3100) == 1);
    CHECK(std::strstr(json, "\"event\":\"time_source\""));
    CHECK(std::strstr(json, "\"previous_source\":\"gps\""));
    CHECK(std::strstr(json, "\"time_source\":\"rtc\""));
    logged.expire(6001, 3000);
    CHECK(events.update(6001, 1, logged, false, 0, json, sizeof(json)));
    CHECK(std::strstr(json, "\"previous_source\":\"rtc\""));
    CHECK(std::strstr(json, "\"time_source\":\"unknown\""));
    CHECK(std::strstr(json, "\"utc_ms\":null"));
    CHECK(!events.update(6002, 1, logged, false, 0, json, sizeof(json)));
    CHECK(input(rtc, 7000) == 1);
    CHECK(std::strstr(json, "\"correction_ms\":null"));
    CHECK(input("PCAS60,120001.000,27102026,,,0,,0", 7001) == 1);
    CHECK(std::strstr(json, "\"time_source\":\"unknown\""));

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
}
