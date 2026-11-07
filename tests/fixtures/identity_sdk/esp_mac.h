#pragma once
#include <cstdint>
using esp_err_t = int;
enum esp_mac_type_t { ESP_MAC_EFUSE_FACTORY };
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);
