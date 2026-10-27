#include "wiknow/bus_timing.hpp"
#include "wiknow/core.hpp"
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
    // A 114-byte report fits the C5 FIFO but still needs 118.75ms on the wire.
    // The previous fixed 100ms wait falsely counted this successful send as lost.
    CHECK(bus_tx_wait_ms(114, 9600) == 139);
    CHECK(bus_tx_wait_ms(max_frame, 9600) == 575);
    CHECK(bus_tx_wait_ms(max_frame, 115200) == 67);
    CHECK(bus_tx_wait_ms(114, 115200) == 30);
    CHECK(bus_tx_wait_ms(1, 9600) == 22);
    CHECK(bus_tx_wait_ms(1, 10000) == 21);
    CHECK(bus_tx_wait_ms(0, 9600) == 0);
    CHECK(bus_tx_wait_ms(114, 0) == 0);
    // Every allowed frame size/representative supported baud has at least the
    // full wire duration plus 20ms, without downward rounding.
    for (uint32_t baud : {9600u, 115200u, 921600u})
        for (uint16_t bytes = 1; bytes <= max_frame; ++bytes) {
            uint64_t wait = bus_tx_wait_ms(bytes, baud) - 20;
            CHECK(wait * baud >= uint64_t(bytes) * 10000);
            CHECK((wait - 1) * baud < uint64_t(bytes) * 10000);
        }
    std::puts("Bus timing tests passed");
}
