#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "font5x7.h"

static const char *TAG = "OLED_LAB9_1";

#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c"
#define BYTE_TO_BINARY(byte)  \
  ((byte) & 0x80 ? '1' : '0'), \
  ((byte) & 0x40 ? '1' : '0'), \
  ((byte) & 0x20 ? '1' : '0'), \
  ((byte) & 0x10 ? '1' : '0'), \
  ((byte) & 0x08 ? '1' : '0'), \
  ((byte) & 0x04 ? '1' : '0'), \
  ((byte) & 0x02 ? '1' : '0'), \
  ((byte) & 0x01 ? '1' : '0')

// กำหนดขาเชื่อมต่อตามแผนภาพวงจรจริง (GPIO 18, 23, 4, 2, 5)
#define OLED_PIN_SCK    (GPIO_NUM_18) // D0 (SPI Clock)
#define OLED_PIN_MOSI   (GPIO_NUM_23) // D1 (SPI MOSI Data)
#define OLED_PIN_RES    (GPIO_NUM_4)  // RES (Hardware Reset)
#define OLED_PIN_DC     (GPIO_NUM_2)  // DC (0 = Command, 1 = Data)
#define OLED_PIN_CS     (GPIO_NUM_5)  // CS (Chip Select - Active LOW)

static spi_device_handle_t s_spi_handle = NULL;
static uint8_t s_oled_buffer[1024]; // 128 คอลัมน์ x 8 เพจ = 1,024 ไบต์

// ฟังก์ชันเริ่มต้นพิน GPIO และบัสฮาร์ดแวร์ SPI2
esp_err_t oled_spi_init(void)
{
    // กำหนดขา DC และ RES เป็น Output
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << OLED_PIN_DC) | (1ULL << OLED_PIN_RES),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // กำหนดค่าบัส SPI (Master Out Only - ไม่ใช้ MISO)
    spi_bus_config_t buscfg = {
        .miso_io_num = -1,               // จอนี้ Write-Only ไม่มีขา MISO
        .mosi_io_num = OLED_PIN_MOSI,     // GPIO 23
        .sclk_io_num = OLED_PIN_SCK,      // GPIO 18
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1024 + 16,
    };

    // ใช้ SPI2_HOST (VSPI บน ESP32)
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) return ret;

    // ผูก Device เข้ากับ Bus (ความถี่ 10 MHz, SPI Mode 0)
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000, // 10 MHz
        .mode = 0,                          // Mode 0: CPOL=0, CPHA=0
        .spics_io_num = OLED_PIN_CS,        // GPIO 5
        .queue_size = 7,
    };

    return spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi_handle);
}

// ฟังก์ชันส่งคำสั่ง 1 ไบต์ (DC = 0)
void oled_send_cmd(uint8_t cmd)
{
    gpio_set_level(OLED_PIN_DC, 0); // ดึง LOW เพื่อบอกชิปว่าเป็นคำสั่ง
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 8;
    t.tx_buffer = &cmd;
    spi_device_polling_transmit(s_spi_handle, &t);
}

// ฟังก์ชันส่งบล็อกข้อมูลพิกเซล (DC = 1)
void oled_send_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    gpio_set_level(OLED_PIN_DC, 1); // ดึง HIGH เพื่อบอกชิปว่าเป็นข้อมูลพิกเซล
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = len * 8;
    t.tx_buffer = data;
    spi_device_polling_transmit(s_spi_handle, &t);
}

// ฟังก์ชันล้างหน้าจอในแรม (เคลียร์เป็นสีดำ)
void oled_clear(void)
{
    memset(s_oled_buffer, 0x00, sizeof(s_oled_buffer));
}

// ฟังก์ชันจุดหรือดับพิกเซลด้วยสูตรคณิตศาสตร์ระดับบิต
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

// ฟังก์ชันส่งข้อมูล 1,024 ไบต์จากแรมขึ้นสู่หน้าจอจริง
void oled_flush(void)
{
    oled_send_cmd(0x21); // Set Column Address
    oled_send_cmd(0x00); // Start Column (0)
    oled_send_cmd(0x7F); // End Column (127)

    oled_send_cmd(0x22); // Set Page Address
    oled_send_cmd(0x00); // Start Page (0)
    oled_send_cmd(0x07); // End Page (7)

    oled_send_data(s_oled_buffer, sizeof(s_oled_buffer));
}

// ฟังก์ชันวาดตัวอักษรเดี่ยว 1 ตัวจากตาราง Font Matrix 5x7
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
    // เว้นช่องไฟระหว่างตัวอักษร 1 พิกเซล
    for (int row = 0; row < 7; row++) {
        oled_draw_pixel(x + 5, y + row, !color);
    }
}

// ฟังก์ชันพิมพ์สตริงข้อความเรียงต่อกัน
void oled_draw_string(int x, int y, const char *str, bool color)
{
    while (*str) {
        oled_draw_char(x, y, *str, color);
        x += 6; // กว้าง 5 + ช่องไฟ 1
        if (x + 6 > 128) break;
        str++;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting OLED Bring-up Lab 9.1...");

    // 1. เริ่มต้นระบบบัส SPI2 และตั้งค่าพิน DC/RES
    ESP_ERROR_CHECK(oled_spi_init());

    // 2. ลำดับการ Hardware Reset (ขา RES)
    gpio_set_level(OLED_PIN_RES, 0); // ดึง LOW เพื่อเริ่มรีเซ็ต
    vTaskDelay(pdMS_TO_TICKS(15));
    gpio_set_level(OLED_PIN_RES, 1); // ดึง HIGH กลับพร้อมทำงาน
    vTaskDelay(pdMS_TO_TICKS(15));

    // 3. ส่งชุดคำสั่ง Magic Sequence เปิดวงจรทวีแรงดัน (Charge Pump) และเปิดจอ
    oled_send_cmd(0xAE); // Display OFF

    // =========================================================================
    // ⚠️ โหมดตีลังกา 180 องศา (Case 3 Fault State)
    // หากต้องการแก้ไขกลับเป็นปกติ ให้เปลี่ยน 0xA0 -> 0xA1 และ 0xC0 -> 0xC8
    // =========================================================================
    oled_send_cmd(0xA0); // Segment Re-map (0xA0 = พลิกซ้าย-ขวา)
    oled_send_cmd(0xC0); // COM Output Scan Direction (0xC0 = พลิกบน-ล่าง)
    // =========================================================================

    oled_send_cmd(0x8D); // Charge Pump Setting
    oled_send_cmd(0x14); // 0x14 = Enable Charge Pump (7.5V)
    oled_send_cmd(0x20); // Addressing Mode
    oled_send_cmd(0x00); // Horizontal Mode
    oled_send_cmd(0xAF); // Display ON!

    // 4. ทดสอบถมหน้าจอ (สว่างทั้งจอ 1,024 ไบต์)
    uint8_t buffer[128];
    memset(buffer, 0xFF, sizeof(buffer));
    oled_send_cmd(0x21); oled_send_cmd(0x00); oled_send_cmd(0x7F);
    oled_send_cmd(0x22); oled_send_cmd(0x00); oled_send_cmd(0x07);
    for (int page = 0; page < 8; page++) {
        oled_send_data(buffer, sizeof(buffer));
    }
    vTaskDelay(pdMS_TO_TICKS(1500)); // โชว์จอขาว 1.5 วินาที

    // 5. ทดสอบจุด 4 มุมจอ (Corner Pixels Test)
    oled_clear();
    oled_draw_pixel(0, 0, true);     // มุมบนซ้าย
    oled_draw_pixel(127, 0, true);   // มุมบนขวา
    oled_draw_pixel(0, 63, true);    // มุมล่างซ้าย
    oled_draw_pixel(127, 63, true);  // มุมล่างขวา
    oled_flush();
    vTaskDelay(pdMS_TO_TICKS(1500)); // โชว์ 4 จุด 1.5 วินาที

    // 6. พิมพ์ข้อความ Hello World และ รหัสนักศึกษา
    oled_clear();
    oled_draw_string(30, 4, "HELLO WORLD", true);
    oled_draw_string(24, 24, "ID: 67030115", true);
    oled_draw_string(24, 38, "ID: 67030170", true);
    oled_flush();

    // 7. นิติวิทยาศาสตร์หน่วยความจำ: Hex Dump Memory Inspection
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI("FORENSIC", "=== DUMPING FRAMEBUFFER PAGE 0 (Cols 30-45) ===");
    for (int col = 30; col <= 45; col++) {
        printf("Byte[%2d] (Col %2d): 0x%02X  [Binary: " BYTE_TO_BINARY_PATTERN "]\n",
               col, col, s_oled_buffer[col], BYTE_TO_BINARY(s_oled_buffer[col]));
    }

    printf("Forensic reconstruction for 'H' (ASCII 72)\n");
    for (int c = 0; c < 5; c++) {
        uint8_t line = font5x7['H' - 32][c];
        printf("Col %d: 0x%02X  [Binary: " BYTE_TO_BINARY_PATTERN "]\n",
               c, line, BYTE_TO_BINARY(line));
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}