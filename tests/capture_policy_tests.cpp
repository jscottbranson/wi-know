#include "wiknow/capture_policy.hpp"
#include "wiknow/observation_queue.hpp"
#include <cstdio>
#include <cstdlib>

using namespace wiknow;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d failed: %s\n", __FILE__, __LINE__, #condition);            \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

static Observation observation(uint32_t sequence, uint64_t at) {
    Observation result{};
    result.boot = 7;
    result.sequence = sequence;
    result.at_ms = at;
    result.radio = Radio::wifi;
    result.address = {0, 1, 2, 3, 4, 5};
    return result;
}

int main() {
    CapturePolicy child{Role::child};
    auto captured = observation(1, 100);
    CHECK(!child.accepts(captured)); // no assignment yet
    child.assignment.count = 1;
    child.assignment.channels[0] = {1};
    child.expires = 200;
    const auto original_child = child;
    ObservationQueue queue;
    CHECK(original_child.accepts(captured));
    CHECK(queue.push(captured, false));
    child.assignment = {}; // stop/reassignment affects future admission only
    child.expires = 0;
    CHECK(!child.accepts(observation(2, 201)));
    Observation queued{};
    CHECK(queue.pop(queued));
    RetainedObservation pending;
    CHECK(pending.retain(queued));
    Report report{};
    pending.report(report, 250);
    CHECK(report.has_observation && report.observation.sequence == 1);
    CHECK(report.capture_age_ms == 150);
    CHECK(pending.pending()); // expired assignment does not erase in-flight report
    CHECK(pending.acknowledge({true, captured.boot, captured.sequence}));

    CHECK(original_child.accepts(observation(2, 199)));
    CHECK(!original_child.accepts(observation(2, 200))); // exact lease boundary
    auto ble = captured;
    ble.radio = Radio::ble;
    CHECK(!original_child.accepts(ble));

    CapturePolicy follower{Role::follower};
    follower.assignment.follow = true;
    follower.assignment.target_radio = Radio::wifi;
    follower.assignment.target = captured.address;
    follower.expires = 200;
    const auto capture_snapshot = follower;
    follower.assignment.target[5] = 9; // change before deferred parsing
    CHECK(capture_snapshot.accepts(captured));
    CHECK(!follower.accepts(captured));
    CHECK(queue.push(captured, true));
    auto other = captured;
    other.address[5] = 9;
    other.sequence = 3;
    CHECK(follower.accepts(other));
    CHECK(queue.push(other, true));
    follower.assignment.follow = false;
    CHECK(!follower.accepts(other));
    CHECK(queue.pop(queued) && queued.sequence == 1);
    CHECK(pending.retain(queued));
    pending.report(report, 300);
    CHECK(report.has_observation && report.observation.sequence == 1);
    CHECK(pending.acknowledge({true, 7, 1}));
    CHECK(queue.pop(queued) && queued.sequence == 3);
    CHECK(pending.retain(queued));
    pending.report(report, 301);
    CHECK(report.has_observation && report.observation.sequence == 3);
    CHECK(pending.acknowledge({true, 7, 3}));
    CHECK(!queue.pop(queued));

    auto random = captured;
    random.address_type = 1;
    CHECK(!capture_snapshot.accepts(random));
    CHECK(!capture_snapshot.accepts(ble));
    CapturePolicy parent{Role::parent};
    CHECK(parent.accepts(captured) && parent.accepts(ble));
    std::puts("Capture admission and preservation tests passed");
}
