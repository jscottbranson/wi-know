#pragma once
#include "esp_mac.h"
#include "wiknow/discovery.hpp"

namespace platform {
inline esp_err_t read_device_identity(wiknow::DeviceId &id) {
    // On IEEE 802.15.4 targets (including C5), esp_efuse_mac_get_default writes
    // an eight-byte extended MAC. Discovery uses the complete six-byte factory
    // MAC. The extended read would overrun DeviceId and drop distinguishing bytes.
    return esp_read_mac(id.data(), ESP_MAC_EFUSE_FACTORY);
}
} // namespace platform
