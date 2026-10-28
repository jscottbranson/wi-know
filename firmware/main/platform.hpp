#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "settings.hpp"
#include "wiknow/core.hpp"
#include "wiknow/discovery.hpp"
#include <atomic>
namespace platform {
inline TickType_t ticks(uint32_t milliseconds) {
    const TickType_t value = pdMS_TO_TICKS(milliseconds);
    return value ? value : 1;
}
extern std::atomic<uint32_t> dropped;
extern uint32_t boot_id;
extern std::atomic<wiknow::Role> role;
extern std::atomic<uint16_t> node_id;
extern wiknow::DeviceId device_id;
void detect_identity();
void set_runtime_role(wiknow::Role value, uint16_t node);
void local_scanning(bool enabled);
bool local_scanning();
bool standalone_scheduler();
uint64_t now_ms();
void init_observation_queues();
bool take_observation(wiknow::Observation &observation);
void set_capture_assignment(const wiknow::Command &assignment, uint64_t expires);
void start_radios();
int standalone_scan_channel(); // -2 starting, -1 error, 0 BLE, positive Wi-Fi channel
bool tune(uint8_t channel);
void pause_wifi();
void submit(wiknow::Observation &observation);
void start_peripherals();
void gps_update(wiknow::Gps &gps);
void indicators(wiknow::Trend trend, bool beep);
void screen(const char *line1, const char *line2);
void write_log(const char *json);
void storage_tick();
bool storage_ready();
void bus_start();
void bus_send(const wiknow::Packet &packet);
bool bus_receive(wiknow::Packet &packet, uint32_t timeout_ms);
} // namespace platform
