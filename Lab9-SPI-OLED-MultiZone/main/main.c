#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font5x7.h"

static const char *TAG = "OLED_MULTIZONE";

// GPIO Pinout
#define OLED_PIN_SCK    (GPIO_NUM_18) // D0
#define OLED_PIN_MOSI   (GPIO_NUM_23) // D1
#define OLED_PIN_RES    (GPIO_NUM_4)  // RES
#define OLED_PIN_DC     (GPIO_NUM_2)  // DC
#define OLED_PIN_CS     (GPIO_NUM_5)  // CS
#define POT_ADC_CHANNEL (ADC_CHANNEL_6) // GPIO 34 (ADC1_CH6)

static spi_device_handle_t s_spi_handle = NULL;
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static uint8_t s_oled_buffer[1024];

esp_err_t oled_spi_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << OLED_PIN_DC) | (1ULL << OLED_PIN_RES),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    spi_bus_config_t buscfg = {
        .miso_io_num = -1,
        .mosi_io_num = OLED_PIN_MOSI,
        .sclk_io_num = OLED_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1024 + 16,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) return ret;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000, // 10 MHz
        .mode = 0,
        .spics_io_num = OLED_PIN_CS,
        .queue_size = 7,
    };

    return spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi_handle);
}

void oled_send_cmd(uint8_t cmd)
{
    gpio_set_level(OLED_PIN_DC, 0);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = &cmd;
    spi_device_polling_transmit(s_spi_handle, &t);
}

void oled_send_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(OLED_PIN_DC, 1);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = len * 8;
    t.tx_buffer = data;
    spi_device_polling_transmit(s_spi_handle, &t);
}

void oled_init_display(void)
{
    // Hardware Reset
    gpio_set_level(OLED_PIN_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(15));
    gpio_set_level(OLED_PIN_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(15));

    oled_send_cmd(0xAE); // Display OFF
    oled_send_cmd(0xD5); oled_send_cmd(0x80); // Set Display Clock Divide Ratio
    oled_send_cmd(0xA8); oled_send_cmd(0x3F); // Set Multiplex Ratio (64 lines)
    oled_send_cmd(0xD3); oled_send_cmd(0x00); // Set Display Offset
    oled_send_cmd(0x40);                      // Set Start Line 0
    oled_send_cmd(0x8D); oled_send_cmd(0x14); // Enable Charge Pump (7.5V)
    oled_send_cmd(0x20); oled_send_cmd(0x00); // Horizontal Addressing Mode
    oled_send_cmd(0xA1);                      // Segment Re-map (Fix upside down)
    oled_send_cmd(0xC8);                      // COM Scan Direction (Fix upside down)
    oled_send_cmd(0xDA); oled_send_cmd(0x12); // Set COM Pins Hardware Config
    oled_send_cmd(0x81); oled_send_cmd(0xCF); // Set Contrast
    oled_send_cmd(0xD9); oled_send_cmd(0xF1); // Set Pre-charge Period
    oled_send_cmd(0xDB); oled_send_cmd(0x40); // Set VCOMH Deselect Level
    oled_send_cmd(0xA4);                      // Entire Display ON (Resume to RAM)
    oled_send_cmd(0xA6);                      // Normal Display
    oled_send_cmd(0xAF);                      // Display ON
}

void oled_clear(void)
{
    memset(s_oled_buffer, 0x00, sizeof(s_oled_buffer));
}

void oled_draw_pixel(int x, int y, bool color)
{
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
    int byte_index = x + (y / 8) * 128;
    int bit_offset = y % 8;
    if (color) {
        s_oled_buffer[byte_index] |= (1 << bit_offset);
    } else {
        s_oled_buffer[byte_index] &= ~(1 << bit_offset);
    }
}

void oled_draw_char(int x, int y, char c, bool color)
{
    if (c < 32 || c > 126) c = '?';
    int font_idx = c - 32;
    for (int col = 0; col < 5; col++) {
        uint8_t line = font5x7[font_idx][col];
        for (int row = 0; row < 7; row++) {
            if (line & (1 << row)) {
                oled_draw_pixel(x + col, y + row, color);
            } else {
                oled_draw_pixel(x + col, y + row, !color);
            }
        }
    }
    for (int row = 0; row < 7; row++) {
        oled_draw_pixel(x + 5, y + row, !color);
    }
}

void oled_draw_string(int x, int y, const char *str, bool color)
{
    while (*str) {
        oled_draw_char(x, y, *str, color);
        x += 6;
        if (x + 6 > 128) break;
        str++;
    }
}

void oled_draw_hline(int x, int y, int w, bool color)
{
    for (int i = 0; i < w; i++) {
        oled_draw_pixel(x + i, y, color);
    }
}

void oled_draw_rect(int x, int y, int w, int h, bool color)
{
    for (int i = 0; i < w; i++) {
        oled_draw_pixel(x + i, y, color);
        oled_draw_pixel(x + i, y + h - 1, color);
    }
    for (int j = 0; j < h; j++) {
        oled_draw_pixel(x, y + j, color);
        oled_draw_pixel(x + w - 1, y + j, color);
    }
}

void oled_fill_rect(int x, int y, int w, int h, bool color)
{
    for (int i = 0; i < w; i++) {
        for (int j = 0; j < h; j++) {
            oled_draw_pixel(x + i, y + j, color);
        }
    }
}

void oled_flush(void)
{
    oled_send_cmd(0x21); oled_send_cmd(0x00); oled_send_cmd(0x7F);
    oled_send_cmd(0x22); oled_send_cmd(0x00); oled_send_cmd(0x07);
    oled_send_data(s_oled_buffer, sizeof(s_oled_buffer));
}

void render_multizone_ui(int percent, int raw_val, const char *footer_msg)
{
    oled_clear();

    // Zone 1: Status & Header (Y = 0..15)
    oled_draw_string(10, 2, "ESP32 | 115 - 170", true);
    oled_draw_hline(0, 13, 128, true);

    // Zone 2: Telemetry & Bar Gauge (Y = 14..47)
    char val_str[32];
    snprintf(val_str, sizeof(val_str), "RAW:%04d  %3d%%", raw_val, percent);
    oled_draw_string(14, 18, val_str, true);

    // วาดกรอบ Bar Gauge (พิกัด X=10, Y=30, กว้าง=108, สูง=12)
    oled_draw_rect(10, 30, 108, 12, true);
    int fill_w = (percent * 104) / 100;
    if (fill_w > 104) fill_w = 104;
    if (fill_w < 0) fill_w = 0;
    oled_fill_rect(12, 32, fill_w, 8, true);

    // Zone 3: Footer (Y = 48..63)
    oled_draw_hline(0, 47, 128, true);
    oled_draw_string(4, 52, footer_msg ? footer_msg : "EDGE: LOCAL EDGE", true);

    oled_flush();
}

void init_adc(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &s_adc_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc_handle, POT_ADC_CHANNEL, &config));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing SPI OLED and ADC...");
    ESP_ERROR_CHECK(oled_spi_init());
    oled_init_display();
    init_adc();

    int raw_val = 0;
    char footer_msg[32] = "EDGE: LOCAL EDGE";

    while (1) {
        // อ่านค่า ADC จาก Potentiometer (GPIO 34)
        adc_oneshot_read(s_adc_handle, POT_ADC_CHANNEL, &raw_val);

        // คำนวณเปอร์เซ็นต์ (สเกล 0-4095 เป็น 0-100%)
        int percent = (raw_val * 100) / 4095;
        if (percent > 100) percent = 100;

        // สตรีมค่าขึ้น Serial สำหรับ Kestrel: "ADC:<raw_val>,<timestamp_ms>"
        uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000);
        printf("ADC:%d,%lu\n", raw_val, (unsigned long)ts);

        // อัปเดตหน้าจอ Multi-Zone UI
        render_multizone_ui(percent, raw_val, footer_msg);

        vTaskDelay(pdMS_TO_TICKS(50)); // 20 Hz
    }
}