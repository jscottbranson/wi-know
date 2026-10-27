#include "wiknow/observation_queue.hpp"
#include "wiknow/report_delivery.hpp"
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

int main() {
    ObservationQueue queue;
    Observation observation{}, received{};
    CHECK(!queue.pop(received));
    // Saturated ordinary traffic cannot consume matching capacity. Repeating
    // across ring wraps also verifies that FIFO order survives slot reuse.
    for (unsigned round = 0; round < 4; ++round) {
        for (size_t i = 0; i < ObservationQueue::ordinary_capacity; ++i) {
            observation.sequence = unsigned(i + round * 100);
            CHECK(queue.push(observation, false));
        }
        CHECK(!queue.push(observation, false));
        for (size_t i = 0; i < ObservationQueue::matching_capacity; ++i) {
            observation.sequence = unsigned(i + round * 100 + 50);
            CHECK(queue.push(observation, true));
        }
        CHECK(!queue.push(observation, true));
        for (size_t i = 0; i < ObservationQueue::matching_capacity; ++i) {
            CHECK(queue.pop(received));
            CHECK(received.sequence == i + round * 100 + 50);
        }
        for (size_t i = 0; i < ObservationQueue::ordinary_capacity; ++i) {
            CHECK(queue.pop(received));
            CHECK(received.sequence == i + round * 100);
        }
        CHECK(!queue.pop(received));
    }
    // Priority selection is reconsidered on every dequeue, even while ordinary
    // observations remain queued.
    observation.sequence = 1;
    CHECK(queue.push(observation, false));
    observation.sequence = 2;
    CHECK(queue.push(observation, false));
    CHECK(queue.pop(received) && received.sequence == 1);
    observation.sequence = 3;
    CHECK(queue.push(observation, true));
    CHECK(queue.pop(received) && received.sequence == 3);
    CHECK(queue.pop(received) && received.sequence == 2);

    // A producer can preempt the polling worker after its clock snapshot, then
    // enqueue a newer capture before the worker dequeues it. The transfer used
    // by the firmware must preserve it rather than compare it to that old clock.
    const uint64_t poll_started = 100;
    observation.boot = 7;
    observation.sequence = 4;
    observation.at_ms = 101;
    CHECK(observation.at_ms > poll_started);
    CHECK(queue.push(observation, true));
    RetainedObservation pending;
    auto take = [&](Observation &out) { return queue.pop(out); };
    CHECK(retain_next_observation(pending, received, take));
    Report report;
    pending.report(report, 102); // report time is sampled after dequeue
    CHECK(report.has_observation && report.observation.sequence == 4);
    CHECK(report.observation.at_ms == 101 && report.capture_age_ms == 1);
    observation.sequence = 5;
    CHECK(queue.push(observation, false));
    CHECK(!retain_next_observation(pending, received, take));
    pending.report(report, 105); // unacknowledged retry retains the same capture
    CHECK(report.observation.sequence == 4 && report.capture_age_ms == 4);
    CHECK(pending.acknowledge({true, 7, 4}));
    CHECK(retain_next_observation(pending, received, take));
    CHECK(received.sequence == 5);
    CHECK(pending.acknowledge({true, 7, 5}));
    CHECK(!retain_next_observation(pending, received, take));
    std::puts("Observation queue tests passed");
}
