#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "platform.hpp"
#include "wiknow/ble_reassembly.hpp"
#include "wiknow/ble_scan_lifecycle.hpp"
#include "wiknow/capture_policy.hpp"
#include "wiknow/gatt_discovery.hpp"
#include "wiknow/observation_queue.hpp"
#include "wiknow/remote_id.hpp"
#include "wiknow/standalone_scan.hpp"
#include "wiknow/wifi_capture.hpp"
#include <algorithm>
#include <cstring>

namespace platform {
std::atomic<uint32_t> dropped{};
uint32_t boot_id{};
static std::atomic<uint32_t> sequence{};
static bool wifi_started{};
static bool wifi_receiving{};
static wiknow::ObservationQueue observation_queue;
static portMUX_TYPE observation_lock = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<uint32_t> ble_generation{1}, ble_stale_reports{};
static wiknow::CapturePolicy capture_policy{wiknow::config::role};
static std::atomic<bool> local_scan_requested{};
void local_scanning(bool enabled) {
    local_scan_requested = enabled;
}
bool local_scanning() {
    return local_scan_requested.load();
}
bool standalone_scheduler() {
    return wiknow::config::standalone_enabled ||
           (wiknow::config::universal_enabled && role == wiknow::Role::parent);
}
static portMUX_TYPE capture_lock = portMUX_INITIALIZER_UNLOCKED;
void set_runtime_role(wiknow::Role value, uint16_t node) {
    portENTER_CRITICAL(&capture_lock);
    role = value;
    node_id = node;
    capture_policy.role = value;
    capture_policy.node = node;
    capture_policy.expires = 0;
    portEXIT_CRITICAL(&capture_lock);
}
void set_capture_assignment(const wiknow::Command &assignment, uint64_t expires) {
    portENTER_CRITICAL(&capture_lock);
    capture_policy.assignment = assignment;
    capture_policy.expires = expires;
    portEXIT_CRITICAL(&capture_lock);
}
static wiknow::CapturePolicy capture_snapshot() {
    portENTER_CRITICAL(&capture_lock);
    const auto snapshot = capture_policy;
    portEXIT_CRITICAL(&capture_lock);
    return snapshot;
}
void init_observation_queues() {
    // Storage is statically initialized before app_main starts the radios.
}
bool take_observation(wiknow::Observation &o) {
    portENTER_CRITICAL(&observation_lock);
    const bool available = observation_queue.pop(o);
    portEXIT_CRITICAL(&observation_lock);
    return available;
}
static void enqueue_observation(const wiknow::Observation &o, uint32_t ble_epoch = 0) {
    // Match outside the radio driver's raw callback and outside the queue lock.
    const bool matching =
        wiknow::best_match(wiknow::config::rules.data(), wiknow::config::rules.size(), o).rule ||
        wiknow::remote_id_messages(o).count != 0;
    portENTER_CRITICAL(&observation_lock);
    const bool current = !ble_epoch || ble_epoch == ble_generation.load();
    const bool queued = current && observation_queue.push(o, matching);
    portEXIT_CRITICAL(&observation_lock);
    if (!current)
        ++ble_stale_reports;
    if (!queued)
        ++dropped;
}
uint64_t now_ms() {
    return uint64_t(esp_timer_get_time() / 1000);
}
void submit(wiknow::Observation &o) {
    if (o.rssi > 0 || o.rssi == -128)
        return;
    const auto policy = capture_snapshot();
    o.node = policy.node;
    o.boot = boot_id;
    o.sequence = ++sequence;
    o.at_ms = now_ms();
    if (policy.accepts(o))
        enqueue_observation(o);
}
// Driver callback copies bounded packets only; parsing happens in a worker task.
struct RawWifi {
    wiknow::CapturePolicy policy;
    uint64_t at;
    uint16_t node;
    int8_t rssi;
    uint8_t channel;
    uint16_t size;
    uint8_t frame[512];
};
static QueueHandle_t raw_wifi{};
static wiknow::WifiDataAdmission data_admission{};
static void wifi_rx(void *buffer, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)
        return;
    const auto *packet = static_cast<const wifi_promiscuous_pkt_t *>(buffer);
    const size_t size = wiknow::wifi_capture_size(packet->payload, packet->rx_ctrl.sig_len,
                                                  packet->rx_ctrl.rx_state == 0);
    if (!size)
        return;
    const uint64_t captured = now_ms();
    if (type == WIFI_PKT_DATA &&
        !data_admission.accept(captured, uxQueueSpacesAvailable(raw_wifi))) {
        ++dropped;
        return;
    }
    RawWifi raw{};
    raw.at = captured;
    raw.policy = capture_snapshot();
    raw.node = raw.policy.node;
    raw.rssi = packet->rx_ctrl.rssi;
    raw.channel = packet->rx_ctrl.channel;
    raw.size = uint16_t(size);
    std::memcpy(raw.frame, packet->payload, raw.size);
    if (xQueueSend(raw_wifi, &raw, 0) != pdTRUE)
        ++dropped;
}
static void wifi_worker(void *) {
    RawWifi raw{};
    for (;;)
        if (xQueueReceive(raw_wifi, &raw, portMAX_DELAY) == pdTRUE) {
            wiknow::Observation o{};
            o.channel = raw.channel;
            o.rssi = raw.rssi;
            if (wiknow::parse_wifi(raw.frame, raw.size, o)) {
                o.node = raw.node;
                o.boot = boot_id;
                o.sequence = ++sequence;
                o.at_ms = raw.at;
                if (o.rssi <= 0 && o.rssi != -128 && raw.policy.accepts(o))
                    enqueue_observation(o);
            }
        }
}
bool tune(uint8_t channel) {
    if (!wifi_started)
        return false;
    esp_err_t error = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (error != ESP_OK)
        ESP_LOGW("radio", "Cannot tune channel %u: %s", channel, esp_err_to_name(error));
    if (error == ESP_OK && !wifi_receiving) {
        error = esp_wifi_set_promiscuous(true);
        wifi_receiving = error == ESP_OK;
    }
    return error == ESP_OK;
}
void pause_wifi() {
    if (wifi_receiving) {
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
        wifi_receiving = false;
    }
}
// NimBLE calls and scan transitions are serialized on its host event queue.
// Reassembly only copies bounded bytes there; AD/RID parsing runs on ble_parse.
static wiknow::BleAssembler ble_assembler;
static portMUX_TYPE ble_assembly_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t raw_ble{};
static constexpr unsigned ble_queue_depth = 2;
struct RawBle {
    wiknow::BleAdvertisement advertisement;
    uint32_t generation;
};
static std::atomic<uint32_t> ble_queue_drops{};
static bool ble_ready{};
static void reset_ble_assembly() {
    // Synchronize with the final queue insertion, not just the worker's first
    // generation check: a reset during parsing must also reject the old report.
    portENTER_CRITICAL(&observation_lock);
    if (++ble_generation == 0)
        ++ble_generation;
    portEXIT_CRITICAL(&observation_lock);
    portENTER_CRITICAL(&ble_assembly_lock);
    ble_assembler.reset();
    portEXIT_CRITICAL(&ble_assembly_lock);
}
static void gatt_offer(const wiknow::BleAdvertisement &);
static void receive_ble_fragment(wiknow::BleFragment &fragment) {
    if (!ble_ready || !ble_gap_disc_active())
        return;
    fragment.capture.at_ms = now_ms();
    fragment.capture.policy = capture_snapshot();
    RawBle raw{};
    raw.generation = ble_generation.load();
    portENTER_CRITICAL(&ble_assembly_lock);
    const auto result = ble_assembler.accept(fragment, raw.advertisement);
    portEXIT_CRITICAL(&ble_assembly_lock);
    if (result == wiknow::BleAssemblyResult::rejected)
        ++dropped;
    if (result == wiknow::BleAssemblyResult::complete)
        gatt_offer(raw.advertisement);
    if (result == wiknow::BleAssemblyResult::complete && xQueueSend(raw_ble, &raw, 0) != pdTRUE) {
        ++ble_queue_drops;
        ++dropped;
    }
}
static void ble_worker(void *) {
    RawBle raw{};
    uint64_t last_report = now_ms();
    for (;;) {
        if (xQueueReceive(raw_ble, &raw, ticks(100)) == pdTRUE) {
            if (raw.generation != ble_generation.load()) {
                ++ble_stale_reports;
                ++dropped;
            } else {
                const auto &advertisement = raw.advertisement;
                wiknow::Observation o{};
                o.address = advertisement.key.address;
                o.address_type = advertisement.key.address_type == BLE_ADDR_PUBLIC ||
                                         advertisement.key.address_type == BLE_ADDR_PUBLIC_ID
                                     ? 0
                                     : 1;
                o.rssi = advertisement.capture.rssi;
                o.at_ms = advertisement.capture.at_ms;
                o.timing_uncertainty_ms = advertisement.duration_ms;
                o.node = advertisement.capture.policy.node;
                o.boot = boot_id;
                o.sequence = ++sequence;
                if (o.rssi <= 0 && o.rssi != -128 &&
                    wiknow::parse_ble(advertisement.data.data(), advertisement.size, o) &&
                    advertisement.capture.policy.accepts(o))
                    enqueue_observation(o, raw.generation);
            }
        }
        if (now_ms() - last_report >= 30000) {
            portENTER_CRITICAL(&ble_assembly_lock);
            const auto stats = ble_assembler.stats();
            portEXIT_CRITICAL(&ble_assembly_lock);
            ESP_LOGI(
                "ble",
                "assembly=%u complete=%u timeout=%u truncated=%u overflow=%u "
                "ambiguous=%u capacity_reset=%u cancelled=%u queue_drop=%u stale=%u "
                "internal_free=%u internal_min=%u",
                unsigned(stats.assemblies), unsigned(stats.complete_reports),
                unsigned(stats.timeouts), unsigned(stats.truncations), unsigned(stats.overflows),
                unsigned(stats.ambiguous), unsigned(stats.capacity_resets),
                unsigned(stats.cancelled), unsigned(ble_queue_drops.load()),
                unsigned(ble_stale_reports.load()),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
            last_report = now_ms();
        }
    }
}
// Standalone transitions run on the NimBLE host queue, serializing them with
// sync/reset/discovery callbacks. With BLE disabled a dedicated task drives them.
static std::atomic<int> standalone_channel{-2}; // -2 starting, -1 error, 0 BLE
int standalone_scan_channel() {
    return standalone_channel.load();
}
static wiknow::StandaloneScan standalone_scan;
static ble_npl_callout standalone_timer;
static uint8_t own_type{};
static void scan_ble();
static wiknow::BleScanLifecycle ble_scan_lifecycle;
static ble_npl_event ble_scan_barrier;
static void post_ble_barrier() {
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &ble_scan_barrier);
}
static void ble_boundary() {
    ble_scan_lifecycle.boundary(reset_ble_assembly, post_ble_barrier);
}
static bool ble_scan_desired() {
    return wiknow::ble_phase_wants_scan(standalone_scheduler(), standalone_scan.ble(),
                                        wiknow::config::universal_enabled, local_scanning(),
                                        wifi_receiving);
}
#if CONFIG_BT_NIMBLE_ROLE_CENTRAL
static wiknow::GattSchedule gatt_schedule(wiknow::config::gatt_targets.data(),
                                          wiknow::config::gatt_targets.size(),
                                          wiknow::config::gatt_cooldown_ms);
using GattPhase = wiknow::GattLifecycle::Phase;
static wiknow::GattLifecycle gatt_lifecycle;
static wiknow::Observation gatt_result;
static uint32_t gatt_attempts{}, gatt_completed{}, gatt_failed{};
static bool gatt_busy() {
    return gatt_lifecycle.busy() || gatt_schedule.pending();
}
static void gatt_finish() {
    gatt_lifecycle.finish();
    gatt_result = {};
}
static void gatt_cancel() {
    gatt_schedule.cancel();
    if (!gatt_lifecycle.busy())
        return;
    gatt_lifecycle.stop(now_ms());
    if (gatt_lifecycle.handle() != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(gatt_lifecycle.handle(), BLE_ERR_REM_USER_CONN_TERM);
    else if (ble_gap_conn_active())
        ble_gap_conn_cancel(); // Wait for CONNECT cancellation callback, never reuse state early.
}
static bool gatt_assignment_current() {
    // Discovery ends later than the advertisement that selected this device.
    // Recheck the live lease/target, rather than publishing on an expired grant.
    return wiknow::gatt_capture_allowed(gatt_result, capture_snapshot(), now_ms());
}
static int gatt_service(uint16_t handle, const ble_gatt_error *error, const ble_gatt_svc *service,
                        void *arg) {
    if (!gatt_lifecycle.service(uint32_t(uintptr_t(arg)), handle))
        return 0;
    if (gatt_lifecycle.expired(now_ms()) || !ble_scan_desired() || !gatt_assignment_current()) {
        gatt_cancel();
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        int8_t rssi{};
        if (ble_gap_conn_rssi(handle, &rssi) == 0 && rssi <= 0 && rssi != -128) {
            gatt_result.rssi = rssi;
            gatt_result.at_ms = now_ms();
            gatt_result.sequence = ++sequence;
            // Only completed discovery is evidence. No advertising bytes are
            // fabricated, and no intermediate or failed database is published.
            enqueue_observation(gatt_result);
            ++gatt_completed;
        } else
            ++gatt_failed;
        gatt_cancel();
        return 0;
    }
    if (error->status || !service || gatt_result.payload_size + 16 > 240) {
        ++gatt_failed;
        gatt_cancel();
        return 0;
    }
    // UUID128 canonical big-endian bytes; standard 16/32-bit UUIDs expand into
    // the Bluetooth base UUID, so every complete entry has exactly 16 octets.
    uint8_t uuid[16]{0, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0x80, 0x5f, 0x9b, 0x34, 0xfb};
    const auto *u = &service->uuid.u;
    if (u->type == BLE_UUID_TYPE_128)
        std::reverse_copy(BLE_UUID128(u)->value, BLE_UUID128(u)->value + 16, uuid);
    else if (u->type == BLE_UUID_TYPE_16) {
        uuid[2] = uint8_t(BLE_UUID16(u)->value >> 8);
        uuid[3] = uint8_t(BLE_UUID16(u)->value);
    } else if (u->type == BLE_UUID_TYPE_32) {
        const auto value = BLE_UUID32(u)->value;
        for (unsigned i = 0; i < 4; ++i)
            uuid[i] = uint8_t(value >> (24 - 8 * i));
    } else {
        ++gatt_failed;
        gatt_cancel();
        return 0;
    }
    std::copy(uuid, uuid + 16, gatt_result.payload.begin() + gatt_result.payload_size);
    gatt_result.payload_size += 16;
    return 0;
}
static int gatt_gap(struct ble_gap_event *event, void *arg) {
    if (!gatt_lifecycle.current(uint32_t(uintptr_t(arg))))
        return 0;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (event->connect.status) {
            ++gatt_failed;
            gatt_finish();
            return 0;
        }
        if (!gatt_lifecycle.connected(uint32_t(uintptr_t(arg)), event->connect.conn_handle,
                                      now_ms(), ble_scan_desired() && gatt_assignment_current())) {
            gatt_cancel();
            return 0;
        }
        if (ble_gattc_disc_all_svcs(gatt_lifecycle.handle(), gatt_service, arg)) {
            ++gatt_failed;
            gatt_cancel();
        }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        if (event->disconnect.conn.conn_handle == gatt_lifecycle.handle())
            gatt_finish();
    } else if (event->type == BLE_GAP_EVENT_PASSKEY_ACTION) {
        // Never pair, provide credentials, read characteristics, or write them.
        gatt_cancel();
    }
    return 0;
}
static void gatt_offer(const wiknow::BleAdvertisement &a) {
    if (!wiknow::config::gatt_enabled || gatt_busy() || !ble_scan_desired() ||
        a.key.address_type > 1 || a.key.primary_phy > 1 || !(a.key.properties & 1) ||
        (a.key.properties & 8))
        return;
    wiknow::Observation o{};
    o.radio = wiknow::Radio::ble;
    o.address = a.key.address;
    o.address_type = a.key.address_type;
    o.node = a.capture.policy.node;
    o.boot = boot_id;
    o.at_ms = a.capture.at_ms;
    if (!a.capture.policy.accepts(o))
        return;
    gatt_schedule.offer(o, true, now_ms(), wiknow::config::rules.data(),
                        wiknow::config::rules.size());
}
static void gatt_tick() {
    if (!wiknow::config::gatt_enabled)
        return;
    const auto now = now_ms();
    static uint64_t last_diagnostics{};
    if (now - last_diagnostics >= 30000) {
        ESP_LOGI("gatt",
                 "attempts=%u completed=%u failed=%u state=%u internal_free=%u internal_min=%u",
                 unsigned(gatt_attempts), unsigned(gatt_completed), unsigned(gatt_failed),
                 unsigned(gatt_lifecycle.phase()),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        last_diagnostics = now;
    }
    if (gatt_lifecycle.busy()) {
        if (!ble_scan_desired() || gatt_lifecycle.expired(now) || !gatt_assignment_current())
            gatt_cancel();
        if (gatt_lifecycle.request_reset(now)) {
            // Termination failure cannot keep a connection alive indefinitely.
            // Host reset destroys handles; reset callback invalidates all state.
            ble_hs_sched_reset(BLE_HS_ETIMEOUT);
        }
        return;
    }
    if (!gatt_schedule.pending())
        return;
    if (!ble_scan_desired() || !ble_ready) {
        gatt_schedule.cancel();
        return;
    }
    if (ble_gap_disc_active()) {
        if (ble_gap_disc_cancel() == 0)
            ble_boundary();
        return;
    }
    if (ble_scan_lifecycle.pending())
        return;
    if (!gatt_schedule.take(now, gatt_result))
        return;
    if (!gatt_assignment_current())
        return;
    gatt_result.ble_evidence = wiknow::BleEvidence::gatt_primary_services;
    ble_addr_t peer{};
    peer.type = gatt_result.address_type;
    std::reverse_copy(gatt_result.address.begin(), gatt_result.address.end(), peer.val);
    gatt_lifecycle.begin(now, wiknow::config::gatt_timeout_ms);
    ++gatt_attempts;
    if (ble_gap_connect(own_type, &peer, wiknow::config::gatt_timeout_ms, nullptr, gatt_gap,
                        reinterpret_cast<void *>(uintptr_t(gatt_lifecycle.token())))) {
        ++gatt_failed;
        gatt_finish();
    }
}
#else
static bool gatt_busy() {
    return false;
}
static void gatt_cancel() {}
static void gatt_offer(const wiknow::BleAdvertisement &) {}
static void gatt_tick() {}
#endif

static void ble_barrier_drained(struct ble_npl_event *) {
    if (ble_scan_lifecycle.drained(post_ble_barrier))
        scan_ble();
}
static int gap_event(struct ble_gap_event *event, void *) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        const auto &d = event->disc;
        wiknow::BleFragment fragment{};
        std::reverse_copy(d.addr.val, d.addr.val + 6, fragment.key.address.begin());
        fragment.key.address_type = d.addr.type;
        fragment.key.sid = 0xff;
        fragment.key.properties = 0x10 |
                                  (d.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP ? 0x08 : 0) |
                                  (d.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ? 1 : 0);
        fragment.capture.rssi = d.rssi;
        fragment.data = d.data;
        fragment.size = d.length_data;
        receive_ble_fragment(fragment);
    }
#if CONFIG_BT_NIMBLE_EXT_ADV
    if (event->type == BLE_GAP_EVENT_EXT_DISC) {
        const auto &d = event->ext_disc;
        wiknow::BleFragment fragment{};
        std::reverse_copy(d.addr.val, d.addr.val + 6, fragment.key.address.begin());
        fragment.key.address_type = d.addr.type;
        fragment.key.sid = d.sid;
        fragment.key.properties = d.props;
        fragment.key.primary_phy = d.prim_phy;
        fragment.key.secondary_phy = d.sec_phy;
        fragment.key.direct_address_type = d.direct_addr.type;
        std::reverse_copy(d.direct_addr.val, d.direct_addr.val + 6,
                          fragment.key.direct_address.begin());
        fragment.key.periodic_interval = d.periodic_adv_itvl;
        fragment.status = static_cast<wiknow::BleDataStatus>(d.data_status);
        fragment.capture.rssi = d.rssi;
        fragment.data = d.data;
        fragment.size = d.length_data;
        receive_ble_fragment(fragment);
    }
#endif
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        ble_boundary();
    }
    return 0;
}
static void scan_ble() {
    const bool desired = ble_scan_desired() && !gatt_busy();
    const bool active = ble_scan_lifecycle.start(
        ble_ready, desired, [] { return ble_gap_disc_active(); },
        [] {
#if CONFIG_BT_NIMBLE_EXT_ADV
            ble_gap_ext_disc_params params{};
            params.passive = 1;
            params.disable_observer_mode = 0; // Pass raw fragments even when AD headers are split.
            params.itvl = 160;
            params.window = 80;
            int rc =
                ble_gap_ext_disc(own_type, 0, 0, 0, 0, 0, &params, &params, gap_event, nullptr);
#else
            ble_gap_disc_params params{};
            params.passive = 1;
            params.disable_observer_mode = 0;
            params.itvl = 160;
            params.window = 80;
            params.filter_duplicates = 0;
            int rc = ble_gap_disc(own_type, BLE_HS_FOREVER, &params, gap_event, nullptr);
#endif
            if (rc)
                ESP_LOGE("ble", "Scan start failed: %d", rc);
            return rc == 0;
        });
    if (standalone_scheduler() && desired)
        standalone_channel = active ? 0 : -1;
}
static void standalone_step() {
    namespace c = wiknow::config;
    const auto now = now_ms();
    if (c::universal_enabled && !local_scanning()) {
        pause_wifi();
        if (ble_ready && !ble_gap_disc_active())
            scan_ble();
        standalone_channel = ble_ready && ble_gap_disc_active() ? 0 : -1;
        standalone_scan = {};
        return;
    }
    if (!standalone_scan.due(now))
        return;
    gatt_cancel();
    if (gatt_busy())
        return; // Wait for disconnect before changing the radio phase.
    // Do not tune Wi-Fi until cancellation has succeeded. A failed cancellation
    // leaves the current phase intact and is retried on the next timer tick.
    if (c::ble_enabled && ble_gap_disc_active()) {
        if (ble_gap_disc_cancel() != 0) {
            standalone_channel = -1;
            return;
        }
        ble_boundary();
    }
    pause_wifi();
    if (standalone_scan.has_started())
        standalone_scan.advance(c::ble_enabled, c::channels.size());
    if (standalone_scan.ble()) {
        if (ble_ready)
            scan_ble();
        standalone_channel = ble_ready && ble_gap_disc_active() ? 0 : -1;
    } else {
        const auto channel = c::channels[standalone_scan.channel_index()].number;
        const bool ok = tune(channel);
        if (!ok)
            pause_wifi();
        standalone_channel = ok ? int(channel) : -1;
    }
    // Failed phases also get a bounded dwell before moving on; one bad channel
    // must not starve the other channels or BLE.
    standalone_scan.started(now_ms(), c::wifi_dwell_ms, c::standalone_ble_dwell_ms);
}
static void standalone_tick(struct ble_npl_event *) {
    standalone_step();
    ble_npl_callout_reset(&standalone_timer, ble_npl_time_ms_to_ticks32(10));
}
static void standalone_wifi_task(void *) {
    for (;;) {
        standalone_step();
        vTaskDelay(ticks(10));
    }
}
static void ble_sync() {
    ble_ready = ble_hs_id_infer_auto(0, &own_type) == 0;
    ble_boundary();
}
static void ble_reset(int reason) {
    ble_ready = false;
#if CONFIG_BT_NIMBLE_ROLE_CENTRAL
    gatt_schedule.cancel();
    gatt_finish();
#endif
    ble_boundary();
    if (standalone_scheduler() && standalone_channel == 0)
        standalone_channel = -1;
    ESP_LOGW("ble", "Host reset: %d", reason);
}
static ble_npl_callout ble_maintenance_timer;
static void ble_maintenance(struct ble_npl_event *) {
    portENTER_CRITICAL(&ble_assembly_lock);
    ble_assembler.expire(now_ms());
    const bool restart = ble_assembler.needs_restart();
    portEXIT_CRITICAL(&ble_assembly_lock);
    // A synchronous controller ACK is not a host-event-queue drain. The posted
    // barrier keeps all scan starts gated while queued old reports are discarded.
    if (restart && ble_ready) {
        if (ble_gap_disc_active()) {
            if (ble_gap_disc_cancel() == 0)
                ble_boundary();
        } else if (!ble_scan_lifecycle.pending()) {
            ble_boundary();
        }
    }
    // Retry failed starts only when this phase wants BLE. The lifecycle gate
    // prevents this timer, sync, or standalone callbacks bypassing the barrier.
    gatt_tick();
    scan_ble();
    ble_npl_callout_reset(&ble_maintenance_timer, ble_npl_time_ms_to_ticks32(50));
}
static void ble_task(void *) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}
void start_radios() {
#if CONFIG_BT_NIMBLE_SECURITY_ENABLE
    static_assert(!wiknow::config::gatt_enabled,
                  "Active discovery requires CONFIG_BT_NIMBLE_SECURITY_ENABLE=n (no pairing)");
#endif
#if !CONFIG_BT_NIMBLE_ROLE_CENTRAL
    static_assert(
        !wiknow::config::gatt_enabled,
        "GATT requires sdkconfig.gatt.defaults central build; see docs/gatt-discovery.md");
#endif
    set_runtime_role(role.load(), node_id.load());
    local_scanning(standalone_scheduler());
    if (role != wiknow::Role::parent || standalone_scheduler()) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        // Passive management-frame capture does not need throughput-oriented
        // station buffers or aggregation. Apply here so existing sdkconfig
        // caches also receive the bounded memory profile on rebuild.
        init.static_rx_buf_num = 4;
        init.dynamic_rx_buf_num = 16;
        init.ampdu_rx_enable = 0;
        init.ampdu_tx_enable = 0;
        init.amsdu_tx_enable = 0;
        ESP_LOGI("radio", "Before Wi-Fi: internal free=%u largest=%u",
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        const esp_err_t wifi_error = esp_wifi_init(&init);
        if (wifi_error != ESP_OK) {
            ESP_LOGE("radio", "Wi-Fi init failed: %s", esp_err_to_name(wifi_error));
            screen("Wi-Fi init fail", esp_err_to_name(wifi_error));
        }
        ESP_ERROR_CHECK(wifi_error);
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
        ESP_ERROR_CHECK(esp_wifi_set_country_code(wiknow::config::wifi_country, false));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
        wifi_started = true;
        raw_wifi = xQueueCreate(8, sizeof(RawWifi));
        configASSERT(raw_wifi);
        configASSERT(xTaskCreate(wifi_worker, "wifi_parse", 4096, nullptr, 4, nullptr) == pdPASS);
        wifi_promiscuous_filter_t filter{};
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_rx));
        // Reception starts when the first nonempty assignment is applied.
    }
    if (wiknow::config::ble_enabled &&
        (wiknow::config::universal_enabled || role != wiknow::Role::child)) {
        ESP_LOGI("radio", "Before BLE: internal free=%u largest=%u",
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        const esp_err_t ble_error = nimble_port_init();
        if (ble_error != ESP_OK) {
            ESP_LOGE("radio", "BLE init failed: %s", esp_err_to_name(ble_error));
            screen("BLE init fail", esp_err_to_name(ble_error));
        }
        ESP_ERROR_CHECK(ble_error);
        raw_ble = xQueueCreate(ble_queue_depth, sizeof(RawBle));
        configASSERT(raw_ble);
        configASSERT(xTaskCreate(ble_worker, "ble_parse", 4096, nullptr, 4, nullptr) == pdPASS);
        ESP_LOGI("ble", "Bounded assembly=%u bytes; completed queue=%u bytes; parser stack=4096",
                 unsigned(sizeof(ble_assembler)), unsigned(ble_queue_depth * sizeof(RawBle)));
        ble_npl_event_init(&ble_scan_barrier, ble_barrier_drained, nullptr);
        ble_npl_callout_init(&ble_maintenance_timer, nimble_port_get_dflt_eventq(), ble_maintenance,
                             nullptr);
        ble_npl_callout_reset(&ble_maintenance_timer, ble_npl_time_ms_to_ticks32(50));
        ble_hs_cfg.sync_cb = ble_sync;
        ble_hs_cfg.reset_cb = ble_reset;
        if (standalone_scheduler()) {
            ble_npl_callout_init(&standalone_timer, nimble_port_get_dflt_eventq(), standalone_tick,
                                 nullptr);
            ble_npl_callout_reset(&standalone_timer, ble_npl_time_ms_to_ticks32(10));
        }
        nimble_port_freertos_init(ble_task);
    }
    if (standalone_scheduler() && !wiknow::config::ble_enabled)
        configASSERT(xTaskCreate(standalone_wifi_task, "local_scan", 4096, nullptr, 4, nullptr) ==
                     pdPASS);
    ESP_LOGI("radio", "Startup complete: internal free=%u largest=%u",
             unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}
} // namespace platform
