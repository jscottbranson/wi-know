#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_vfs_fat.h"
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
static i2c_master_dev_handle_t lcd{};
static uint64_t beep_until{};
static QueueHandle_t log_queue{}, console_queue{};
static std::atomic<bool> sd_ready{};
struct LogLine {
    char data[wiknow::log_line_capacity];
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
        uart_setup(c::bus_uart, c::bus_tx_pin, c::bus_rx_pin, c::bus_rts_pin, c::bus_baud, true);
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
    if (c::role != wiknow::Role::parent || !c::gps_enabled)
        return;
    uint8_t data[512];
    int n = uart_read_bytes(uart_port_t(c::gps_uart), data, sizeof(data), 0);
    uint64_t now = now_ms();
    for (int i = 0; i < n; ++i)
        gps.feed(char(data[i]), now);
}
void indicators(wiknow::Trend trend, bool beep) {
    if (c::role != wiknow::Role::parent)
        return;
    if (c::signal_trend_enabled) {
        gpio_set_level(gpio_num_t(c::signal_trend_increasing_pin),
                       trend == wiknow::Trend::increasing);
        gpio_set_level(gpio_num_t(c::signal_trend_decreasing_pin),
                       trend == wiknow::Trend::decreasing);
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
    mount.format_if_mount_failed = false;
    mount.max_files = 3;
    mount.allocation_unit_size = 16 * 1024;
    sdmmc_card_t *card{};
    error = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount, &card);
    if (error != ESP_OK) {
        ESP_LOGE("sd", "Mount failed (card is never auto-formatted): %s", esp_err_to_name(error));
        spi_bus_free(SPI2_HOST);
        return false;
    }
    return true;
}
struct LogFiles {
    uint32_t newest{}, oldest{UINT32_MAX};
    size_t count{};
};
static LogFiles files() {
    LogFiles result{};
    DIR *dir = opendir("/sdcard");
    if (!dir)
        return result;
    while (auto *entry = readdir(dir)) {
        unsigned id{};
        int consumed{};
        if (std::sscanf(entry->d_name, "WK%8u.LOG%n", &id, &consumed) == 1 &&
            consumed == int(std::strlen(entry->d_name))) {
            result.newest = std::max(result.newest, uint32_t(id));
            result.oldest = std::min(result.oldest, uint32_t(id));
            ++result.count;
        }
    }
    closedir(dir);
    return result;
}
static FILE *next_log() {
    auto index = files();
    if (index.newest >= 99999999)
        return nullptr;
    while (index.count >= size_t(c::logging_max_files)) {
        char name[48];
        std::snprintf(name, sizeof(name), "/sdcard/WK%08u.LOG", unsigned(index.oldest));
        if (std::remove(name) != 0)
            return nullptr;
        index = files();
    }
    char name[48];
    std::snprintf(name, sizeof(name), "/sdcard/WK%08u.LOG", unsigned(index.newest + 1));
    return std::fopen(name, "w");
}
static void logger(void *) {
    FILE *file = nullptr;
    size_t bytes = 0;
    uint64_t last_flush = now_ms();
    if (mount_sd())
        file = next_log();
    sd_ready = file != nullptr;
    LogLine line{};
    for (;;) {
        if (xQueueReceive(log_queue, &line, ticks(100)) == pdTRUE) {
            size_t size = std::strlen(line.data);
            if (file && bytes + size > size_t(c::logging_rotate_bytes)) {
                bool clean = std::fclose(file) == 0;
                file = clean ? next_log() : nullptr;
                bytes = 0;
                sd_ready = file != nullptr;
            }
            if (file) {
                if (std::fwrite(line.data, 1, size, file) != size) {
                    ESP_LOGE("sd", "Write failed; logging disabled until reboot");
                    std::fclose(file);
                    file = nullptr;
                    sd_ready = false;
                    ++dropped;
                } else
                    bytes += size;
            } else
                ++dropped;
        }
        if (file && now_ms() - last_flush >= uint64_t(c::logging_flush_interval_ms)) {
            if (std::fflush(file) != 0 || fsync(fileno(file)) != 0) {
                ESP_LOGE("sd", "Flush failed; logging disabled until reboot");
                std::fclose(file);
                file = nullptr;
                sd_ready = false;
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
void write_log(const char *json) {
    // Both output paths are bounded and independent of the parent polling task.
    LogLine line{};
    size_t n = std::strlen(json);
    if (n >= sizeof(line.data)) {
        ++dropped;
        return;
    }
    std::memcpy(line.data, json, n + 1);
    if (console_queue && xQueueSend(console_queue, &line, 0) != pdTRUE)
        ++dropped;
    if (log_queue && xQueueSend(log_queue, &line, 0) != pdTRUE)
        ++dropped;
}
void storage_tick() {}
bool storage_ready() {
    return sd_ready;
}
void start_peripherals() {
    if (c::role != wiknow::Role::parent)
        return;
    console_queue = xQueueCreate(8, sizeof(LogLine));
    configASSERT(console_queue);
    configASSERT(xTaskCreate(console_writer, "console_writer", 4096, nullptr, 1, nullptr) ==
                 pdPASS);
    if (c::gps_enabled)
        uart_setup(c::gps_uart, c::gps_tx_pin, c::gps_rx_pin, -1, c::gps_baud, false);
    if (c::signal_trend_enabled) {
        for (int pin : {c::signal_trend_increasing_pin, c::signal_trend_decreasing_pin,
                        c::signal_trend_steady_pin}) {
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
    if (c::display_enabled)
        init_lcd();
    if (c::sd_enabled) {
        log_queue = xQueueCreate(8, sizeof(LogLine));
        configASSERT(log_queue);
        configASSERT(xTaskCreate(logger, "sd_writer", 6144, nullptr, 2, nullptr) == pdPASS);
    }
}
} // namespace platform
