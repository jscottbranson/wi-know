#pragma once

#include <cstdint>

namespace wiknow {
// UART uses 8N1: ten wire bits per byte. Include time for the entire encoded
// frame even though uart_write_bytes may already have transmitted part of it,
// plus margin for TX_DONE handling and conversion to FreeRTOS ticks.
constexpr uint32_t bus_tx_wait_ms(uint16_t frame_bytes, uint32_t baud) {
    if (!frame_bytes || !baud)
        return 0;
    return uint32_t((uint64_t(frame_bytes) * 10000 + baud - 1) / baud) + 20;
}
} // namespace wiknow
