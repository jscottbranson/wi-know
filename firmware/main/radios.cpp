#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "platform.hpp"
#include "wiknow/ble_reassembly.hpp"
#include "wiknow/ble_scan_lifecycle.hpp"
#include "wiknow/capture_policy.hpp"
#include "wiknow/observation_queue.hpp"
#include "wiknow/remote_id.hpp"
#include "wiknow/standalone_scan.hpp"
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
static void wifi_rx(void *buffer, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT)
        return;
    const auto *packet = static_cast<const wifi_promiscuous_pkt_t *>(buffer);
    size_t size = packet->rx_ctrl.sig_len;
    if (packet->rx_ctrl.rx_state != 0 || size < 40 || size - 4 > 512)
        return; // omit trailing FCS; reject truncated frames
    const auto *frame = packet->payload;
    const bool nan_action = frame[0] == 0xd0 && size >= 34 && frame[24] == 4 && frame[25] == 9 &&
                            frame[26] == 0x50 && frame[27] == 0x6f && frame[28] == 0x9a &&
                            frame[29] == 0x13;
    if ((frame[0] & 0xfc) != 0x80 && (frame[0] & 0xfc) != 0x50 && !nan_action)
        return;
    RawWifi raw{};
    raw.at = now_ms();
    raw.policy = capture_snapshot();
    raw.node = raw.policy.node;
    raw.rssi = packet->rx_ctrl.rssi;
    raw.channel = packet->rx_ctrl.channel;
    raw.size = uint16_t(size - 4);
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
    else if (result == wiknow::BleAssemblyResult::complete &&
             xQueueSend(raw_ble, &raw, 0) != pdTRUE) {
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
        fragment.key.properties =
            0x10 | (d.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP ? 0x08 : 0);
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
    const bool desired = ble_scan_desired();
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
    scan_ble();
    ble_npl_callout_reset(&ble_maintenance_timer, ble_npl_time_ms_to_ticks32(50));
}
static void ble_task(void *) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}
void start_radios() {
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
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
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
