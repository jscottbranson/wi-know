#include "device_identity.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/uart.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "wiknow/gps_time_request.hpp"
// These FAT helpers require types declared by esp_vfs_fat.h.
// clang-format off
#include "diskio_sdmmc.h"
#include "diskio_wl.h"
// clang-format on
#include "ff.h"
#include "log_helpers.hpp"
#include "log_storage.hpp"
#include "nvs.h"
#include "platform.hpp"
#include "sdmmc_cmd.h"
#include "wiknow/bus_timing.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <unistd.h>

namespace platform {
namespace c = wiknow::config;
std::atomic<wiknow::Role> role{c::role};
std::atomic<uint16_t> node_id{UINT16_MAX};
wiknow::DeviceId device_id{};
void detect_identity() {
    ESP_ERROR_CHECK(read_device_identity(device_id));
    if (!c::universal_enabled) {
        node_id = role == wiknow::Role::parent ? 0 : UINT16_MAX;
        return;
    }
    // Input only, no internal pulls: the external resistor defines disconnected wiring.
    gpio_config_t pin{};
    pin.pin_bit_mask = 1ULL << c::universal_strap_pin;
    pin.mode = GPIO_MODE_INPUT;
    pin.pull_up_en = GPIO_PULLUP_DISABLE;
    pin.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&pin));
    vTaskDelay(ticks(20));
    int high = 0;
    for (int i = 0; i < 16; ++i) {
        high += gpio_get_level(gpio_num_t(c::universal_strap_pin));
        vTaskDelay(ticks(2));
    }
    uint8_t override_role = 0;
    nvs_handle_t handle;
    esp_err_t nvs_error = nvs_open("wiknow", NVS_READONLY, &handle);
    if (nvs_error == ESP_OK) {
        nvs_error = nvs_get_u8(handle, "identity", &override_role);
        nvs_close(handle);
    }
    const auto detected = wiknow::hardware_identity(high, 16, override_role);
    if ((nvs_error != ESP_OK && nvs_error != ESP_ERR_NVS_NOT_FOUND) ||
        detected == wiknow::HardwareIdentity::invalid) {
        ESP_LOGE("identity",
                 "Unstable parent identification or invalid override; halted before outputs");
        for (;;)
            vTaskDelay(ticks(1000));
    }
    role =
        detected == wiknow::HardwareIdentity::parent ? wiknow::Role::parent : wiknow::Role::child;
    node_id = role == wiknow::Role::parent ? 0 : UINT16_MAX;
    ESP_LOGI("identity", "MAC=%02x:%02x:%02x:%02x:%02x:%02x source=%s detected=%s node=%u",
             device_id[0], device_id[1], device_id[2], device_id[3], device_id[4], device_id[5],
             override_role ? "NVS override" : "passive harness",
             role == wiknow::Role::parent ? "parent" : "worker", unsigned(node_id.load()));
}
static i2c_master_dev_handle_t lcd{};
static uint64_t beep_until{};
static QueueHandle_t log_queue{}, console_queue{};
// Each line is bounded by log_line_capacity. Keep output queues bounded without consuming the
// internal RAM needed to initialize Wi-Fi and BLE on boards without enabled PSRAM.
static constexpr unsigned log_queue_depth = 2;
static std::atomic<Storage> active_storage{Storage::starting};
static const char *log_root = "/sdcard";
static LogLimits limits{size_t(c::logging_rotate_bytes), size_t(c::logging_max_files)};
struct LogLine {
    char data[wiknow::log_line_capacity];
    LogClass classification{LogClass::unmatched};
    bool mirror{};
    uint8_t transition{}; // 1 starts a new WU session; 2 ends and syncs it
};
static bool capture_active{}; // parent task only; each queued line snapshots this
void capture_switch_init() {
    if (!c::capture_switch_enabled || role != wiknow::Role::parent)
        return;
    gpio_config_t pin{};
    pin.pin_bit_mask = 1ULL << (c::capture_switch_gpio_pin < 0 ? 0 : c::capture_switch_gpio_pin);
    pin.mode = GPIO_MODE_INPUT;
    pin.pull_down_en =
        c::capture_switch_internal_pulldown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&pin));
}
bool capture_switch_high() {
    return c::capture_switch_enabled && gpio_get_level(gpio_num_t(c::capture_switch_gpio_pin));
}
static bool gps_uart_ready{};
static esp_err_t uart_setup(int port, int tx, int rx, int rts, int baud, bool rs485) {
    uart_config_t cfg{};
    cfg.baud_rate = baud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    const auto uart = uart_port_t(port);
    // Flash erase/write disables interrupts that are not IRAM-safe. The 128-byte
    // RX FIFO overflows in about 11 ms at 115200 baud, so service it from IRAM.
    esp_err_t error = uart_driver_install(uart, 4096, 0, 0, nullptr, ESP_INTR_FLAG_IRAM);
    if (error != ESP_OK)
        return error; // Do not delete a driver this call did not install.
    error = uart_param_config(uart, &cfg);
    if (error == ESP_OK)
        error = uart_set_pin(uart, tx, rx, rts, UART_PIN_NO_CHANGE);
    if (error == ESP_OK && rs485)
        error = uart_set_mode(uart, UART_MODE_RS485_HALF_DUPLEX);
    if (error != ESP_OK)
        uart_driver_delete(uart);
    return error;
}
// Bus transport and protocol counters are accessed only by the application task.
static struct {
    uint32_t tx_frames{}, tx_errors{}, rx_bytes{}, rx_frames{}, rx_errors{}, read_errors{};
    uint32_t polls_accepted{}, reports_accepted{};
    uint32_t last_rx_type{}, last_rx_node{}, last_rx_sequence{};
    uint64_t last_rx_ms{}, last_log_ms{};
} bus_stats;
void bus_start() {
    if (!c::bus_enabled) {
        ESP_LOGI("bus", "disabled");
        return;
    }
    const bool parent = role == wiknow::Role::parent;
    const int tx = c::universal_enabled
                       ? (parent ? c::universal_parent_tx_pin : c::universal_worker_tx_pin)
                       : c::bus_tx_pin;
    const int rx = c::universal_enabled
                       ? (parent ? c::universal_parent_rx_pin : c::universal_worker_rx_pin)
                       : c::bus_rx_pin;
    const int rts = c::universal_enabled
                        ? (parent ? c::universal_parent_rts_pin : c::universal_worker_rts_pin)
                        : c::bus_rts_pin;
    ESP_ERROR_CHECK(uart_setup(c::bus_uart, tx, rx, rts, c::bus_baud, true));
    ESP_LOGI("bus", "mode=%s uart=%d baud=%d tx=%d rx=%d rts=%d nodes=%u..%u timeout_ms=%u",
             c::universal_enabled ? "universal" : "manual", int(c::bus_uart), int(c::bus_baud), tx,
             rx, rts, unsigned(c::bus_first_node_id), unsigned(c::bus_last_node_id),
             unsigned(c::bus_poll_timeout_ms));
}
void bus_poll_accepted() {
    ++bus_stats.polls_accepted;
}
void bus_report_accepted() {
    ++bus_stats.reports_accepted;
}
void bus_diagnostics_tick() {
    const auto now = now_ms();
    if (!c::bus_enabled || now - bus_stats.last_log_ms < 5000)
        return;
    bus_stats.last_log_ms = now;
    ESP_LOGI("bus",
             "tx_frames=%u tx_errors=%u rx_bytes=%u rx_frames=%u decode_errors=%u "
             "read_errors=%u polls_accepted=%u reports_accepted=%u "
             "last_rx_type=%u last_rx_node=%u last_rx_seq=%u last_rx_age_ms=%lld",
             unsigned(bus_stats.tx_frames), unsigned(bus_stats.tx_errors),
             unsigned(bus_stats.rx_bytes), unsigned(bus_stats.rx_frames),
             unsigned(bus_stats.rx_errors), unsigned(bus_stats.read_errors),
             unsigned(bus_stats.polls_accepted), unsigned(bus_stats.reports_accepted),
             unsigned(bus_stats.last_rx_type), unsigned(bus_stats.last_rx_node),
             unsigned(bus_stats.last_rx_sequence),
             bus_stats.rx_frames ? static_cast<long long>(now - bus_stats.last_rx_ms) : -1LL);
}
void bus_send(const wiknow::Packet &p) {
    uint8_t frame[wiknow::max_frame];
    size_t n = wiknow::encode_packet(p, frame, sizeof(frame));
    if (!n || !c::bus_enabled)
        return;
    int written = uart_write_bytes(uart_port_t(c::bus_uart), frame, n);
    if (written != int(n) ||
        uart_wait_tx_done(uart_port_t(c::bus_uart),
                          ticks(wiknow::bus_tx_wait_ms(uint16_t(n), c::bus_baud))) != ESP_OK) {
        ++bus_stats.tx_errors;
        ++dropped;
    } else {
        ++bus_stats.tx_frames;
    }
}
bool bus_receive(wiknow::Packet &packet, uint32_t timeout) {
    if (!c::bus_enabled)
        return false;
    static wiknow::Decoder decoder;
    static uint64_t last_byte{};
    static uint8_t burst[24];
    static size_t burst_size{}, burst_total{};
    const auto uart = uart_port_t(c::bus_uart);
    auto decoded = [&](uint64_t now) {
        ++bus_stats.rx_frames;
        bus_stats.rx_errors = decoder.errors();
        bus_stats.last_rx_type = unsigned(packet.type);
        bus_stats.last_rx_node = packet.node;
        bus_stats.last_rx_sequence = packet.sequence;
        bus_stats.last_rx_ms = now;
        return true;
    };

    // Abandon a candidate only after an inter-byte idle gap, never at a caller's
    // polling deadline.
    // A zero timeout decodes only bytes already buffered and never waits.
    constexpr uint32_t idle_ms = 20;
    uint64_t until = now_ms() + timeout;
    do {
        if (decoder.next(packet))
            return decoded(now_ms());
        size_t buffered = 0;
        if (!timeout && (uart_get_buffered_data_len(uart, &buffered) != ESP_OK || !buffered))
            return false;
        uint8_t byte{};
        int n = uart_read_bytes(uart, &byte, 1, timeout ? ticks(1) : 0);
        uint64_t now = now_ms();
        if (n == 0 && now - last_byte >= idle_ms) {
            decoder.abandon();
            // TEMPORARY diagnostic: dump the start of each received burst, at most once a second.
            static uint64_t last_dump{};
            if (burst_total && now - last_dump >= 1000) {
                last_dump = now;
                ESP_LOGI("bus", "rx burst len=%u frames=%u", unsigned(burst_total),
                         unsigned(bus_stats.rx_frames));
                ESP_LOG_BUFFER_HEX("bus", burst, burst_size);
            }
            burst_size = burst_total = 0;
        }
        if (n < 0)
            ++bus_stats.read_errors;
        if (n == 1) {
            if (burst_size < sizeof(burst))
                burst[burst_size++] = byte;
            ++burst_total;
            ++bus_stats.rx_bytes;
            last_byte = now;
            const bool complete = decoder.feed(byte, packet);
            bus_stats.rx_errors = decoder.errors();
            if (complete)
                return decoded(now);
        }
    } while (!timeout || now_ms() < until);
    return false;
}
static bool lcd_write(uint8_t byte) {
    return lcd && i2c_master_transmit(lcd, &byte, 1, 20) == ESP_OK;
}
static bool lcd_nibble(uint8_t nibble, bool rs) {
    uint8_t b = uint8_t((nibble << 4) | 8 | (rs ? 1 : 0));
    if (!lcd_write(b) || !lcd_write(b | 4))
        return false;
    esp_rom_delay_us(1);
    if (!lcd_write(b))
        return false;
    esp_rom_delay_us(50);
    return true;
}
static bool lcd_byte(uint8_t byte, bool rs) {
    return lcd_nibble(byte >> 4, rs) && lcd_nibble(byte & 15, rs);
}
void screen(const char *a, const char *b) {
    if (!lcd)
        return;
    const char *rows[] = {a, b};
    for (int row = 0; row < 2; ++row) {
        if (!lcd_byte(uint8_t(row ? 0xc0 : 0x80), false))
            return;
        size_t n = std::strlen(rows[row]);
        for (size_t i = 0; i < 16; ++i)
            if (!lcd_byte(i < n ? uint8_t(rows[row][i]) : ' ', true))
                return;
    }
}
void gps_update(wiknow::Gps &gps) {
    if (role != wiknow::Role::parent || !c::gps_enabled || !gps_uart_ready)
        return;
    uint64_t now = now_ms();
    gps.expire(now, c::gps_max_fix_age_ms);
    if (c::gps_tx_pin >= 0 && !std::strcmp(c::gps_model, "Grove GPS Air530Z")) {
        static wiknow::GpsTimeRequest request;
        size_t size{};
        const char *command = request.pending(now, size);
        if (size) {
            // TX buffer is disabled: write only available FIFO space, never wait.
            int sent = uart_tx_chars(uart_port_t(c::gps_uart), command, size);
            if (sent > 0)
                request.sent(now, size_t(sent));
        }
    }
    // One bounded, zero-wait read: neither a silent GPS nor missing RTC data gates scanning.
    uint8_t data[512];
    int n = uart_read_bytes(uart_port_t(c::gps_uart), data, sizeof(data), 0);
    for (int i = 0; i < n; ++i)
        gps.feed(char(data[i]), now);
    gps.expire(now, c::gps_max_fix_age_ms);
}

void indicators(wiknow::Trend trend, bool beep) {
    if (role != wiknow::Role::parent)
        return;
    if (c::signal_trend_enabled) {
        const auto leds = wiknow::trend_leds(trend);
        if (c::signal_trend_increasing_pin >= 0)
            gpio_set_level(gpio_num_t(c::signal_trend_increasing_pin), leds.green);
        if (c::signal_trend_decreasing_pin >= 0)
            gpio_set_level(gpio_num_t(c::signal_trend_decreasing_pin), leds.red);
    }
    if (c::buzzer_enabled) {
        uint64_t now = now_ms();
        if (beep)
            beep_until = now + c::buzzer_duration_ms;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, now < beep_until ? 512 : 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
}
static void init_lcd() {
    ESP_LOGI("lcd", "Initializing SDA=%d SCL=%d address=0x%02x", c::display_sda_pin,
             c::display_scl_pin, unsigned(c::display_i2c_address));
    i2c_master_bus_config_t bus{};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = gpio_num_t(c::display_sda_pin);
    bus.scl_io_num = gpio_num_t(c::display_scl_pin);
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t handle{};
    esp_err_t error = i2c_new_master_bus(&bus, &handle);
    if (error != ESP_OK) {
        ESP_LOGW("lcd", "Display unavailable: %s", esp_err_to_name(error));
        return;
    }
    i2c_device_config_t dev{};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = c::display_i2c_address;
    dev.scl_speed_hz = 100000;
    error = i2c_master_bus_add_device(handle, &dev, &lcd);
    if (error != ESP_OK) {
        ESP_LOGW("lcd", "Display unavailable: %s", esp_err_to_name(error));
        i2c_del_master_bus(handle);
        return;
    }
    vTaskDelay(ticks(50));
    if (!lcd_nibble(3, false)) {
        ESP_LOGE("lcd", "No response at configured I2C address");
        i2c_master_bus_rm_device(lcd);
        lcd = nullptr;
        i2c_del_master_bus(handle);
        return;
    }
    vTaskDelay(ticks(5));
    lcd_nibble(3, false);
    esp_rom_delay_us(150);
    lcd_nibble(3, false);
    lcd_nibble(2, false);
    lcd_byte(0x28, false);
    lcd_byte(0x0c, false);
    lcd_byte(0x06, false);
    lcd_byte(1, false);
    vTaskDelay(ticks(3));
    screen("wi-know", "Starting...");
}
static sdmmc_card_t *sd_card{};
static wl_handle_t flash_handle = WL_INVALID_HANDLE;
static bool mount_sd() {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = c::sd_clock_khz;
    spi_bus_config_t bus{};
    bus.mosi_io_num = c::sd_mosi_pin;
    bus.miso_io_num = c::sd_miso_pin;
    bus.sclk_io_num = c::sd_sck_pin;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    esp_err_t error = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        ESP_LOGE("sd", "SPI init: %s", esp_err_to_name(error));
        return false;
    }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = gpio_num_t(c::sd_cs_pin);
    esp_vfs_fat_sdmmc_mount_config_t mount{};
    mount.format_if_mount_failed = c::sd_format_if_mount_failed;
    if (c::sd_format_if_mount_failed)
        ESP_LOGW("sd", "Formatting authorized for unrecognized/damaged filesystem; existing data "
                       "may be erased");
    mount.max_files = 3;
    mount.allocation_unit_size = 16 * 1024;
    error = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &sd_card);
    if (error != ESP_OK) {
        ESP_LOGE("sd", "Mount failed (format_if_mount_failed=%s): %s",
                 c::sd_format_if_mount_failed ? "true" : "false", esp_err_to_name(error));
        spi_bus_free(SPI2_HOST);
        return false;
    }
    return true;
}
static bool mount_flash() {
    const auto *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "logs");
    if (!partition) {
        ESP_LOGE("storage", "Flash log partition missing; flash the updated partition table");
        return false;
    }
    const bool blank =
        erased_partition(partition->size, [partition](size_t offset, void *data, size_t n) {
            return esp_partition_read(partition, offset, data, n) == ESP_OK;
        });
    esp_vfs_fat_mount_config_t mount{};
    mount.format_if_mount_failed = blank;
    mount.max_files = 3;
    mount.allocation_unit_size = CONFIG_WL_SECTOR_SIZE;
    if (blank)
        ESP_LOGI("storage", "Initializing erased flash log partition");
    const esp_err_t error =
        esp_vfs_fat_spiflash_mount_rw_wl("/flashlog", "logs", &mount, &flash_handle);
    if (error != ESP_OK) {
        ESP_LOGE("storage", "Flash mount failed: %s; existing contents are not reformatted",
                 esp_err_to_name(error));
        return false;
    }
    return true;
}
static bool storage_free(uint64_t &free) {
    const BYTE number = std::strcmp(log_root, "/sdcard") == 0 ? ff_diskio_get_pdrv_card(sd_card)
                                                              : ff_diskio_get_pdrv_wl(flash_handle);
    if (number == 0xff)
        return false;
    char drive[8];
    std::snprintf(drive, sizeof(drive), "%u:", unsigned(number));
    FATFS *fs = nullptr;
    DWORD clusters = 0;
    if (f_getfree(drive, &clusters, &fs) != FR_OK)
        return false;
    size_t sector = FF_MIN_SS;
#if FF_MAX_SS != FF_MIN_SS
    sector = fs->ssize;
#endif
    limits.cluster_bytes = size_t(fs->csize) * sector;
    free = uint64_t(clusters) * limits.cluster_bytes;
    return limits.cluster_bytes != 0;
}
static bool readable_logs() {
    LogFiles index;
    uint64_t free{};
    return scan_logs(log_root, index) && storage_free(free);
}
static void logger(void *) {
    LogWriter writer;
    uint64_t last_flush = now_ms(), last_drop_report = 0;
    uint32_t retention_drops = 0;
    bool written = false;
    auto flash_fallback = [&] {
        if (!mount_flash())
            return false;
        log_root = "/flashlog";
        limits = log_limits(Storage::flash, c::logging_rotate_bytes, c::logging_max_files);
        if (readable_logs())
            return true;
        esp_vfs_fat_spiflash_unmount_rw_wl("/flashlog", flash_handle);
        return false;
    };
    auto sd_to_flash = [&] {
        ESP_LOGW("storage", "SD unavailable; trying flash");
        writer.close_all();
        esp_vfs_fat_sdcard_unmount("/sdcard", sd_card);
        spi_bus_free(SPI2_HOST);
        return flash_fallback();
    };
    Storage selected = select_storage(
        c::sd_enabled,
        [&] {
            if (!mount_sd())
                return false;
            if (readable_logs())
                return true;
            ESP_LOGW("storage", "SD log directory unavailable; trying flash");
            esp_vfs_fat_sdcard_unmount("/sdcard", sd_card);
            spi_bus_free(SPI2_HOST);
            return false;
        },
        flash_fallback);
    active_storage = selected;
    ESP_LOGI("storage", "Logging=%s rotate=%u max_files=%u", storage_label(selected),
             unsigned(limits.rotate_bytes), unsigned(limits.max_files));
    LogLine line{};
    for (;;) {
        if (xQueueReceive(log_queue, &line, ticks(100)) == pdTRUE) {
            if (active_storage != Storage::off) {
                const auto result = append_with_fallback(
                    selected, written,
                    [&] {
                        const auto appended = writer.record(
                            log_root, limits, line.classification, line.data, line.mirror,
                            line.transition,
                            [](size_t reserve) {
                                uint64_t free{};
                                if (!storage_free(free))
                                    return -1;
                                return free >= reserve ? 1 : 0;
                            },
                            [](FILE *file) { return fsync(fileno(file)) == 0; });
                        if (appended == LogResult::stored && !written &&
                            !writer.flush([](FILE *file) { return fsync(fileno(file)) == 0; }))
                            return LogResult::error;
                        return appended;
                    },
                    sd_to_flash);
                active_storage = selected;
                if (result != LogResult::stored)
                    ++dropped;
                if (result == LogResult::blocked) {
                    ++retention_drops;
                    if (!last_drop_report || now_ms() - last_drop_report >= 5000) {
                        ESP_LOGW("storage",
                                 "Retention dropped %s record/copy; total=%u (protected logs or no "
                                 "space)",
                                 line.mirror                                ? "capture"
                                 : line.classification == LogClass::matched ? "matched"
                                                                            : "unmatched",
                                 unsigned(retention_drops));
                        last_drop_report = now_ms();
                    }
                } else if (result == LogResult::error) {
                    ESP_LOGE("storage", "%s write/rotation failed; logging disabled until reboot",
                             storage_label(selected));
                    writer.close_all();
                    active_storage = Storage::off;
                }
            } else
                ++dropped;
        }
        const uint64_t flush_ms =
            selected == Storage::flash
                ? std::max(uint64_t(c::logging_flush_interval_ms), uint64_t(5000))
                : uint64_t(c::logging_flush_interval_ms);
        if (active_storage != Storage::off && now_ms() - last_flush >= flush_ms) {
            if (!writer.flush([](FILE *file) { return fsync(fileno(file)) == 0; })) {
                ++dropped; // Buffered records on the failed medium cannot be recovered.
                if (selected == Storage::sd && sd_to_flash()) {
                    selected = Storage::flash;
                    written = false;
                    active_storage = selected;
                } else {
                    ESP_LOGE("storage",
                             "Flush failed and no fallback; logging disabled until reboot");
                    writer.close_all();
                    active_storage = Storage::off;
                }
            }
            last_flush = now_ms();
        }
    }
}
static void console_writer(void *) {
    LogLine line{};
    for (;;)
        if (xQueueReceive(console_queue, &line, portMAX_DELAY) == pdTRUE)
            std::fputs(line.data, stdout);
}
void write_log(const char *json, LogClass classification) {
    // Both output paths are bounded and independent of the parent polling task.
    LogLine line{};
    size_t n = std::strlen(json);
    if (n >= sizeof(line.data)) {
        ++dropped;
        return;
    }
    std::memcpy(line.data, json, n + 1);
    line.classification = classification;
    line.mirror = capture_active && classification == LogClass::matched;
    if (console_queue && xQueueSend(console_queue, &line, 0) != pdTRUE)
        ++dropped;
    // Reserve one slot for matches/control under full-capture pressure. Single producer.
    if (!log_queue ||
        (capture_active && classification == LogClass::unmatched &&
         uxQueueSpacesAvailable(log_queue) <= 1) ||
        xQueueSend(log_queue, &line, 0) != pdTRUE)
        ++dropped;
}
void capture_transition(const char *json, bool on) {
    LogLine line{};
    std::snprintf(line.data, sizeof(line.data), "%s", json);
    line.transition = on ? 1 : 2;
    // Ordered barrier: new file before subsequent observations, close after prior ones.
    // The logger always drains commands even when storage is OFF.
    if (!log_queue || xQueueSend(log_queue, &line, portMAX_DELAY) != pdTRUE)
        ++dropped;
    if (console_queue && xQueueSend(console_queue, &line, 0) != pdTRUE)
        ++dropped;
    capture_active = on;
}
void storage_tick() {}
const char *storage_name() {
    return storage_label(active_storage.load());
}
static void boot_indicator_test() {
    if (!c::signal_trend_enabled && !c::buzzer_enabled)
        return;
    ESP_LOGI("peripherals",
             "Boot test: green/red/both 200 ms each, then buzzer 100 ms (enabled outputs only)");
    if (c::signal_trend_enabled) {
        for (auto trend :
             {wiknow::Trend::increasing, wiknow::Trend::decreasing, wiknow::Trend::steady}) {
            const auto leds = wiknow::trend_leds(trend);
            if (!(leds.green && c::signal_trend_increasing_pin >= 0) &&
                !(leds.red && c::signal_trend_decreasing_pin >= 0))
                continue;
            indicators(trend, false);
            vTaskDelay(ticks(200));
            indicators(wiknow::Trend::unknown, false);
        }
    }
    if (c::buzzer_enabled) {
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
        vTaskDelay(ticks(100));
        ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0));
        ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));
    }
    // This hardware check does not create a detection or consume its alert cooldown.
    ESP_LOGI("peripherals", "Boot test complete; outputs off (confirm light/sound physically)");
}
void start_peripherals() {
    if (role != wiknow::Role::parent) {
        ESP_LOGI("peripherals", "Worker: parent peripherals and boot test skipped");
        return;
    }
    ESP_LOGI("peripherals", "Parent: LEDs=%s buzzer=%s LCD=%s",
             c::signal_trend_enabled ? "enabled" : "disabled",
             c::buzzer_enabled ? "enabled" : "disabled",
             c::display_enabled ? "enabled" : "disabled");
    console_queue = xQueueCreate(log_queue_depth, sizeof(LogLine));
    if (!console_queue)
        esp_system_abort("Console queue allocation failed");
    if (xTaskCreate(console_writer, "console_writer", 4608, nullptr, 1, nullptr) != pdPASS)
        esp_system_abort("Console writer task allocation failed");
    if (c::gps_enabled) {
        const esp_err_t error =
            uart_setup(c::gps_uart, c::gps_tx_pin, c::gps_rx_pin, -1, c::gps_baud, false);
        gps_uart_ready = error == ESP_OK;
        if (!gps_uart_ready)
            ESP_LOGW("gps", "UART unavailable (%s); continuing without GPS until reboot",
                     esp_err_to_name(error));
    }
    if (c::signal_trend_enabled) {
        for (int pin : {c::signal_trend_increasing_pin, c::signal_trend_decreasing_pin}) {
            if (pin < 0)
                continue;
            gpio_reset_pin(gpio_num_t(pin));
            gpio_set_direction(gpio_num_t(pin), GPIO_MODE_OUTPUT);
            gpio_set_level(gpio_num_t(pin), 0);
        }
    }
    if (c::buzzer_enabled) {
        ledc_timer_config_t timer{};
        timer.speed_mode = LEDC_LOW_SPEED_MODE;
        timer.duty_resolution = LEDC_TIMER_10_BIT;
        timer.timer_num = LEDC_TIMER_0;
        timer.freq_hz = c::buzzer_frequency_hz;
        timer.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&timer));
        ledc_channel_config_t channel{};
        channel.gpio_num = c::buzzer_pin;
        channel.speed_mode = LEDC_LOW_SPEED_MODE;
        channel.channel = LEDC_CHANNEL_0;
        channel.timer_sel = LEDC_TIMER_0;
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
    }
    boot_indicator_test();
    if (c::display_enabled)
        init_lcd();
}
void start_storage() {
    if (role != wiknow::Role::parent)
        return;
    log_queue = xQueueCreate(log_queue_depth, sizeof(LogLine));
    if (!log_queue || xTaskCreate(logger, "log_writer", 6656, nullptr, 2, nullptr) != pdPASS) {
        if (log_queue)
            vQueueDelete(log_queue);
        log_queue = nullptr;
        active_storage = Storage::off;
        ESP_LOGE("storage", "No RAM for log writer; console logging only");
    }
}
} // namespace platform
