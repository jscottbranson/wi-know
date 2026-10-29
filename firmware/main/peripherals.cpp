#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_vfs_fat.h"
#include "diskio_sdmmc.h"
#include "diskio_wl.h"
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
std::atomic<uint16_t> node_id{c::node_id};
wiknow::DeviceId device_id{};
void detect_identity() {
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(device_id.data()));
    if (!c::universal_enabled)
        return;
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
};
static void uart_setup(int port, int tx, int rx, int rts, int baud, bool rs485) {
    uart_config_t cfg{};
    cfg.baud_rate = baud;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(uart_port_t(port), 4096, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_port_t(port), &cfg));
    ESP_ERROR_CHECK(uart_set_pin(uart_port_t(port), tx, rx, rts, UART_PIN_NO_CHANGE));
    if (rs485)
        ESP_ERROR_CHECK(uart_set_mode(uart_port_t(port), UART_MODE_RS485_HALF_DUPLEX));
}
void bus_start() {
    if (c::bus_enabled)
        uart_setup(
            c::bus_uart,
            c::universal_enabled ? (role == wiknow::Role::parent ? c::universal_parent_tx_pin
                                                                 : c::universal_worker_tx_pin)
                                 : c::bus_tx_pin,
            c::universal_enabled ? (role == wiknow::Role::parent ? c::universal_parent_rx_pin
                                                                 : c::universal_worker_rx_pin)
                                 : c::bus_rx_pin,
            c::universal_enabled ? (role == wiknow::Role::parent ? c::universal_parent_rts_pin
                                                                 : c::universal_worker_rts_pin)
                                 : c::bus_rts_pin,
            c::bus_baud, true);
}
void bus_send(const wiknow::Packet &p) {
    uint8_t frame[wiknow::max_frame];
    size_t n = wiknow::encode_packet(p, frame, sizeof(frame));
    if (!n || !c::bus_enabled)
        return;
    int written = uart_write_bytes(uart_port_t(c::bus_uart), frame, n);
    if (written != int(n) ||
        uart_wait_tx_done(uart_port_t(c::bus_uart),
                          ticks(wiknow::bus_tx_wait_ms(uint16_t(n), c::bus_baud))) != ESP_OK)
        ++dropped;
}
bool bus_receive(wiknow::Packet &packet, uint32_t timeout) {
    if (!c::bus_enabled)
        return false;
    static wiknow::Decoder decoder;
    static uint64_t last_byte{};
    static uint32_t reported_errors{};
    // Reset only after an inter-byte idle gap, never at a caller's polling deadline.
    constexpr uint32_t idle_ms = 20;
    uint64_t until = now_ms() + timeout;
    do {
        uint8_t byte{};
        int n = uart_read_bytes(uart_port_t(c::bus_uart), &byte, 1, ticks(1));
        uint64_t now = now_ms();
        if (n == 0 && now - last_byte >= idle_ms)
            decoder.reset();
        if (n == 1) {
            last_byte = now;
            if (decoder.feed(byte, packet))
                return true;
            if (decoder.errors() != reported_errors) {
                reported_errors = decoder.errors();
                ESP_LOGW("bus", "Rejected incompatible or corrupt frame; errors=%u",
                         unsigned(reported_errors));
            }
        }
    } while (now_ms() < until);
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
    if (role != wiknow::Role::parent || !c::gps_enabled)
        return;
    uint8_t data[512];
    int n = uart_read_bytes(uart_port_t(c::gps_uart), data, sizeof(data), 0);
    uint64_t now = now_ms();
    for (int i = 0; i < n; ++i)
        gps.feed(char(data[i]), now);
}
void indicators(wiknow::Trend trend, bool beep) {
    if (role != wiknow::Role::parent)
        return;
    if (c::signal_trend_enabled) {
        if (c::signal_trend_increasing_pin >= 0)
            gpio_set_level(gpio_num_t(c::signal_trend_increasing_pin),
                           trend == wiknow::Trend::increasing);
        if (c::signal_trend_decreasing_pin >= 0)
            gpio_set_level(gpio_num_t(c::signal_trend_decreasing_pin),
                           trend == wiknow::Trend::decreasing);
        if (c::signal_trend_steady_pin >= 0)
            gpio_set_level(gpio_num_t(c::signal_trend_steady_pin), trend == wiknow::Trend::steady);
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
    i2c_master_bus_handle_t handle{};
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &handle));
    i2c_device_config_t dev{};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = c::display_i2c_address;
    dev.scl_speed_hz = 100000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(handle, &dev, &lcd));
    vTaskDelay(ticks(50));
    if (!lcd_nibble(3, false)) {
        ESP_LOGE("lcd", "No response at configured I2C address");
        lcd = nullptr;
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
    const BYTE number = std::strcmp(log_root, "/sdcard") == 0
                            ? ff_diskio_get_pdrv_card(sd_card)
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
        }, flash_fallback);
    active_storage = selected;
    ESP_LOGI("storage", "Logging=%s rotate=%u max_files=%u", storage_label(selected),
             unsigned(limits.rotate_bytes), unsigned(limits.max_files));
    LogLine line{};
    for (;;) {
        if (xQueueReceive(log_queue, &line, ticks(100)) == pdTRUE) {
            if (active_storage != Storage::off) {
                const auto result = append_with_initial_fallback(selected, written, [&] {
                    const auto appended = writer.append(log_root, limits, line.classification, line.data,
                        [](size_t reserve) {
                            uint64_t free{};
                            if (!storage_free(free))
                                return -1;
                            return free >= reserve ? 1 : 0;
                        });
                    if (appended == LogResult::stored && !written &&
                        !writer.flush([](FILE *file) { return fsync(fileno(file)) == 0; }))
                        return LogResult::error;
                    return appended;
                }, [&] {
                    ESP_LOGW("storage", "Initial SD write failed; trying flash");
                    writer.close_all();
                    esp_vfs_fat_sdcard_unmount("/sdcard", sd_card);
                    spi_bus_free(SPI2_HOST);
                    return flash_fallback();
                });
                active_storage = selected;
                if (result != LogResult::stored)
                    ++dropped;
                if (result == LogResult::blocked) {
                    ++retention_drops;
                    if (!last_drop_report || now_ms() - last_drop_report >= 5000) {
                        ESP_LOGW("storage", "Retention dropped %s record; total=%u (protected logs or no space)",
                                 line.classification == LogClass::matched ? "matched" : "unmatched",
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
                ESP_LOGE("storage", "%s flush failed; logging disabled until reboot",
                         storage_label(selected));
                writer.close_all();
                active_storage = Storage::off;
                ++dropped;
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
    if (console_queue && xQueueSend(console_queue, &line, 0) != pdTRUE)
        ++dropped;
    if (log_queue && xQueueSend(log_queue, &line, 0) != pdTRUE)
        ++dropped;
}
void storage_tick() {}
const char *storage_name() {
    return storage_label(active_storage.load());
}
static void boot_indicator_test() {
    if (!c::signal_trend_enabled && !c::buzzer_enabled)
        return;
    ESP_LOGI("peripherals",
             "Boot test: LEDs 200 ms each, then buzzer 100 ms (enabled outputs only)");
    if (c::signal_trend_enabled) {
        for (int pin : {c::signal_trend_increasing_pin, c::signal_trend_decreasing_pin,
                        c::signal_trend_steady_pin}) {
            if (pin < 0)
                continue;
            ESP_ERROR_CHECK(gpio_set_level(gpio_num_t(pin), 1));
            vTaskDelay(ticks(200));
            ESP_ERROR_CHECK(gpio_set_level(gpio_num_t(pin), 0));
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
    configASSERT(console_queue);
    configASSERT(xTaskCreate(console_writer, "console_writer", 4096, nullptr, 1, nullptr) ==
                 pdPASS);
    if (c::gps_enabled)
        uart_setup(c::gps_uart, c::gps_tx_pin, c::gps_rx_pin, -1, c::gps_baud, false);
    if (c::signal_trend_enabled) {
        for (int pin : {c::signal_trend_increasing_pin, c::signal_trend_decreasing_pin,
                        c::signal_trend_steady_pin}) {
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
    if (!log_queue || xTaskCreate(logger, "log_writer", 6144, nullptr, 2, nullptr) != pdPASS) {
        if (log_queue)
            vQueueDelete(log_queue);
        log_queue = nullptr;
        active_storage = Storage::off;
        ESP_LOGE("storage", "No RAM for log writer; console logging only");
    }
}
} // namespace platform
