#include "wiknow/core.hpp"
#include <cassert>
#include <cstring>
#include <limits>
using namespace wiknow;

static FollowingConfig config() {
    FollowingConfig c{true, 100, 1000, 4000};
    c.scoring_enabled = true;
    c.min_duration_ms = 2000;
    c.min_hits = c.score_threshold = 3;
    c.hit_interval_ms = 1000;
    c.score_decay_ms = 10000;
    c.gps_jitter_m = 10;
    c.max_speed_mps = 150;
    return c;
}
struct Fixture {
    FollowingDetector d;
    Observation o{};
    double distance{};
    Fixture(FollowingConfig c = config()) : d(c, nullptr, 0) {
        o.radio = Radio::ble;
        o.address = {1, 2, 3, 4, 5, 6};
    }
    bool see(uint64_t t, double lat = 0, bool valid = true, uint64_t received = 0) {
        o.at_ms = t;
        return d.observe(o, {valid, lat, 0, t}, distance, received ? received : t);
    }
    bool event(const char *s) {
        return d.evidence().event && std::strcmp(d.evidence().event, s) == 0;
    }
    bool reason(const char *s) {
        return d.evidence().reason && std::strcmp(d.evidence().reason, s) == 0;
    }
};
int main() {
    Fixture stationary;
    assert(!stationary.see(1000));
    assert(!stationary.see(2000));
    assert(!stationary.see(3000));
    assert(stationary.event("device_persistence"));
    assert(stationary.d.evidence().hits == 3);
    for (uint64_t t = 4000; t < 20000; t += 1000)
        assert(!stationary.see(t));

    Fixture moving;
    assert(!moving.see(1000));
    assert(!moving.see(2000, .0012));
    moving.o.node = 9;
    moving.o.channel = 36;
    assert(moving.see(3000, .0013));
    assert(moving.event("following_alert"));
    assert(moving.distance > 140 && moving.distance < 150);
    assert(!moving.see(4000, .0013));
    assert(!moving.see(10000, .0013));
    assert(moving.reason("continuity_gap"));
    assert(moving.d.evidence().hits == 1);

    Fixture crossing;
    crossing.see(1000);
    crossing.see(2000);
    assert(!crossing.see(3000, .0012));
    assert(crossing.event("device_persistence")); // crossing does not hide persistence
    assert(crossing.see(4000, .0012));

    Fixture jitter;
    assert(!jitter.see(1000));
    assert(!jitter.see(2000, .0009));
    assert(!jitter.see(3000, .0009)); // about 100m, below radius plus uncertainty
    assert(!jitter.see(4000, .0012));
    assert(!jitter.see(5000, 0));
    assert(jitter.reason("inside_radius"));
    assert(!jitter.see(6000, .0012));
    assert(jitter.see(7000, .0012));

    Fixture gaps;
    gaps.see(1000);
    gaps.see(2000, .0012);
    assert(!gaps.see(3000, .0013, false));
    assert(gaps.reason("invalid_or_stale_gps"));
    assert(!gaps.see(4000, .0013));
    assert(!gaps.see(5000, .0013));
    assert(!gaps.see(6000, .0013)); // reacquisition establishes new origin
    assert(!gaps.see(7000, 20));
    assert(gaps.reason("gps_jump"));

    Fixture rate;
    rate.see(1000);
    rate.see(1500);
    assert(!rate.d.evidence().event);
    rate.see(2000);
    assert(rate.d.evidence().hits == 2);
    rate.o.at_ms = 2000;
    assert(!rate.d.observe(rate.o, {true, .0012, 0, 2000}, rate.distance, 2100));
    assert(!rate.d.evidence().event);
    rate.o.at_ms = 1900;
    assert(!rate.d.observe(rate.o, {true, .0012, 0, 1900}, rate.distance, 2200));
    assert(!rate.d.evidence().event);
    rate.see(3000);
    assert(rate.d.evidence().hits == 3);
    assert(!rate.see(4000, .001, true, 9000));
    assert(rate.reason("late_capture"));

    Fixture stale;
    stale.see(1000);
    stale.o.at_ms = 5000;
    assert(!stale.d.observe(stale.o, {true, .001, 0, 1000}, stale.distance, 5000));
    assert(stale.reason("invalid_or_stale_gps"));
    stale.see(6000);
    stale.o.at_ms = 7000;
    assert(!stale.d.observe(stale.o, {true, std::numeric_limits<double>::quiet_NaN(), 0, 7000},
                            stale.distance, 7000));
    assert(stale.reason("invalid_or_stale_gps"));

    Fixture future;
    future.see(1000);
    assert(!future.see(3000, .0012, true, 2000));
    assert(future.reason("late_capture"));
    Fixture uncertainty;
    uncertainty.see(1000);
    uncertainty.o.timing_uncertainty_ms = 2001;
    assert(!uncertainty.see(2000));
    assert(uncertainty.reason("invalid_or_stale_gps"));
    Fixture nearest;
    nearest.see(1000);
    nearest.o.at_ms = 2000;
    assert(!nearest.d.observe(nearest.o, {true, 0, 0, 2100}, nearest.distance, 2200));
    assert(nearest.d.evidence().hits == 2); // nearest fix can be after capture

    Fixture rotation;
    std::strcpy(rotation.o.name.data(), "same tracker class");
    rotation.o.payload_size = 4;
    rotation.o.payload[0] = 3;
    rotation.o.payload[1] = 3;
    rotation.o.payload[2] = 0x0f;
    rotation.o.payload[3] = 0x18;
    rotation.see(1000);
    rotation.see(2000, .0012);
    rotation.o.address[0]++;
    assert(!rotation.see(3000, .0013));
    assert(rotation.d.evidence().hits == 1);
    rotation.o.address_type = 1;
    assert(!rotation.see(4000, .0013));
    assert(rotation.d.evidence().hits == 1);
    rotation.o.radio = Radio::wifi;
    assert(!rotation.see(5000, .0013));
    assert(rotation.d.evidence().hits == 1);

    auto decay_config = config();
    decay_config.score_decay_ms = 1000;
    Fixture decay(decay_config);
    decay.see(1000);
    decay.see(2000, .0012);
    assert(!decay.see(3000, .0013));
    assert(decay.d.evidence().hits == 3 && decay.d.evidence().score == 1);
    assert(!decay.see(6000, .0013));
    assert(decay.d.evidence().score == 1);

    Fixture clock;
    clock.see(10000);
    clock.see(11000, .0012);
    assert(!clock.see(1000, .0013));
    assert(clock.reason("clock_discontinuity"));
    assert(clock.d.evidence().hits == 1);

    DeviceIdentity ignore{Radio::ble, 0, {{1, 2, 3, 4, 5, 6}}};
    FollowingDetector ignored(config(), &ignore, 1);
    Observation o{};
    o.radio = Radio::ble;
    o.address = ignore.address;
    double distance{};
    assert(!ignored.observe(o, {true, 0, 0, 0}, distance));
    assert(!ignored.evidence().event);
    auto disabled_config = config();
    disabled_config.enabled = false;
    Fixture disabled(disabled_config);
    assert(!disabled.see(1000));
    assert(!disabled.d.evidence().event);
    Fixture capacity;
    for (unsigned i = 0; i < max_tracks + 1; ++i) {
        capacity.o.address[0] = uint8_t(i);
        assert(!capacity.see(1000 + i));
    }
    assert(capacity.reason("capacity_replacement"));
    capacity.o.address[0] = 0;
    assert(!capacity.see(2000));
    assert(capacity.d.evidence().hits == 1);
}
