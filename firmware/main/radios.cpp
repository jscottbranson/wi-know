#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "platform.hpp"
#include "wiknow/capture_policy.hpp"
#include "wiknow/observation_queue.hpp"
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
static wiknow::CapturePolicy capture_policy{wiknow::config::role};
static portMUX_TYPE capture_lock = portMUX_INITIALIZER_UNLOCKED;
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
static void enqueue_observation(const wiknow::Observation &o) {
    // Match outside the radio driver's raw callback and outside the queue lock.
    const bool matching =
        wiknow::best_match(wiknow::config::rules.data(), wiknow::config::rules.size(), o).rule;
    portENTER_CRITICAL(&observation_lock);
    const bool queued = observation_queue.push(o, matching);
    portEXIT_CRITICAL(&observation_lock);
    if (!queued)
        ++dropped;
}
uint64_t now_ms() {
    return uint64_t(esp_timer_get_time() / 1000);
}
void submit(wiknow::Observation &o) {
    if (o.rssi > 0 || o.rssi == -128)
        return;
    o.node = wiknow::config::node_id;
    o.boot = boot_id;
    o.sequence = ++sequence;
    o.at_ms = now_ms();
    if (capture_snapshot().accepts(o))
        enqueue_observation(o);
}
// Driver callback copies bounded packets only; parsing happens in a worker task.
struct RawWifi {
    wiknow::CapturePolicy policy;
    uint64_t at;
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
    if ((packet->payload[0] & 0xfc) != 0x80 && (packet->payload[0] & 0xfc) != 0x50)
        return;
    RawWifi raw{};
    raw.at = now_ms();
    raw.policy = capture_snapshot();
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
                o.node = wiknow::config::node_id;
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
// Standalone transitions run on the NimBLE host queue, serializing them with
// sync/reset/discovery callbacks. With BLE disabled a dedicated task drives them.
static std::atomic<int> standalone_channel{-2}; // -2 starting, -1 error, 0 BLE
int standalone_scan_channel() {
    return standalone_channel.load();
}
static wiknow::StandaloneScan standalone_scan;
static ble_npl_callout standalone_timer;
static bool ble_ready{};
static uint8_t own_type{};
static void scan_ble();
static int gap_event(struct ble_gap_event *event, void *) {
    wiknow::Observation o{};
    if (event->type == BLE_GAP_EVENT_DISC) {
        const auto &d = event->disc;
        std::reverse_copy(d.addr.val, d.addr.val + 6, o.address.begin());
        o.address_type =
            d.addr.type == BLE_ADDR_PUBLIC || d.addr.type == BLE_ADDR_PUBLIC_ID ? 0 : 1;
        o.rssi = d.rssi;
        if (wiknow::parse_ble(d.data, d.length_data, o))
            submit(o);
    }
#if CONFIG_BT_NIMBLE_EXT_ADV
    if (event->type == BLE_GAP_EVENT_EXT_DISC) {
        const auto &d = event->ext_disc;
        // Fragmented/truncated advertising reports are not complete AD structures.
        if (d.data_status != 0)
            return 0;
        std::reverse_copy(d.addr.val, d.addr.val + 6, o.address.begin());
        o.address_type =
            d.addr.type == BLE_ADDR_PUBLIC || d.addr.type == BLE_ADDR_PUBLIC_ID ? 0 : 1;
        o.rssi = d.rssi;
        if (wiknow::parse_ble(d.data, d.length_data, o))
            submit(o);
    }
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE && !wiknow::config::standalone_enabled)
        scan_ble();
#else
    if (event->type == BLE_GAP_EVENT_DISC_COMPLETE && !wiknow::config::standalone_enabled)
        scan_ble();
#endif
    return 0;
}
static void scan_ble() {
#if CONFIG_BT_NIMBLE_EXT_ADV
    ble_gap_ext_disc_params params{};
    params.passive = 1;
    params.itvl = 160;
    params.window = 80;
    int rc = ble_gap_ext_disc(own_type, 0, 0, 0, 0, 0, &params, &params, gap_event, nullptr);
#else
    ble_gap_disc_params params{};
    params.passive = 1;
    params.itvl = 160;
    params.window = 80;
    params.filter_duplicates = 0;
    int rc = ble_gap_disc(own_type, BLE_HS_FOREVER, &params, gap_event, nullptr);
#endif
    if (rc)
        ESP_LOGE("ble", "Scan start failed: %d", rc);
}
static void standalone_step() {
    namespace c = wiknow::config;
    const auto now = now_ms();
    if (!standalone_scan.due(now))
        return;
    // Do not tune Wi-Fi until cancellation has succeeded. A failed cancellation
    // leaves the current phase intact and is retried on the next timer tick.
    if (c::ble_enabled && ble_gap_disc_active()) {
        if (ble_gap_disc_cancel() != 0) {
            standalone_channel = -1;
            return;
        }
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
    if (ble_ready && !wiknow::config::standalone_enabled)
        scan_ble();
}
static void ble_reset(int reason) {
    ble_ready = false;
    if (wiknow::config::standalone_enabled && standalone_channel == 0)
        standalone_channel = -1;
    ESP_LOGW("ble", "Host reset: %d", reason);
}
static void ble_task(void *) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}
void start_radios() {
    if (wiknow::config::role != wiknow::Role::parent || wiknow::config::standalone_enabled) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
        ESP_ERROR_CHECK(esp_wifi_set_country_code(wiknow::config::wifi_country, false));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO));
        wifi_started = true;
        raw_wifi = xQueueCreate(16, sizeof(RawWifi));
        configASSERT(raw_wifi);
        configASSERT(xTaskCreate(wifi_worker, "wifi_parse", 4096, nullptr, 4, nullptr) == pdPASS);
        wifi_promiscuous_filter_t filter{};
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
        ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_rx));
        // Reception starts when the first nonempty assignment is applied.
    }
    if (wiknow::config::ble_enabled && wiknow::config::role != wiknow::Role::child) {
        ESP_ERROR_CHECK(nimble_port_init());
        ble_hs_cfg.sync_cb = ble_sync;
        ble_hs_cfg.reset_cb = ble_reset;
        if (wiknow::config::standalone_enabled) {
            ble_npl_callout_init(&standalone_timer, nimble_port_get_dflt_eventq(), standalone_tick,
                                 nullptr);
            ble_npl_callout_reset(&standalone_timer, ble_npl_time_ms_to_ticks32(10));
        }
        nimble_port_freertos_init(ble_task);
    }
    if (wiknow::config::standalone_enabled && !wiknow::config::ble_enabled)
        configASSERT(xTaskCreate(standalone_wifi_task, "local_scan", 4096, nullptr, 4, nullptr) ==
                     pdPASS);
}
} // namespace platform
