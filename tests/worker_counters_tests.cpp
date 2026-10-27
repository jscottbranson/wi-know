#include "wiknow/worker_counters.hpp"
#include <cstdio>
#include <cstdlib>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d failed: %s\n", __FILE__, __LINE__, #condition);            \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

int main() {
    wiknow::WorkerLossCounters counters;
    CHECK(counters.total() == 0 && !counters.get(1));
    CHECK(!counters.update(0, 1, 100));
    CHECK(!counters.update(wiknow::max_nodes + 1, 1, 100));
    CHECK(!counters.offline(0));
    CHECK(!counters.offline(wiknow::max_nodes + 1));
    CHECK(counters.update(1, 0, 0)); // Boot and counter zero are valid first values.
    CHECK(!counters.update(1, 0, 0));
    CHECK(counters.update(1, 0, 7));
    CHECK(counters.update(2, 42, 11));
    CHECK(counters.total() == 18);
    CHECK(!counters.update(2, 42, 11)); // Repeated reports do not add losses again.
    CHECK(counters.offline(2));
    CHECK(!counters.offline(2));
    CHECK(!counters.get(2)->online && counters.total() == 18);
    CHECK(counters.update(2, 42, 15)); // Same boot rejoins after its membership expired.
    CHECK(counters.get(2)->online && counters.total() == 22);
    CHECK(counters.update(2, 43, 3)); // New boot adds its initial losses, retaining history.
    CHECK(counters.get(2)->reported == 3 && counters.get(2)->total == 18);
    CHECK(counters.total() == 25);
    CHECK(!counters.update(2, 43, 3));
    CHECK(counters.update(3, 99, UINT32_MAX - 2));
    const uint64_t before_wrap = counters.total();
    CHECK(counters.update(3, 99, 2));
    CHECK(counters.total() == before_wrap + 5);
    CHECK(counters.get(3)->total == uint64_t(UINT32_MAX) + 3);
    CHECK(counters.offline(1) && counters.offline(2) && counters.offline(3));
    CHECK(counters.total() == before_wrap + 5); // Offline workers remain in parent totals.
    std::puts("Worker counter tests passed");
}
