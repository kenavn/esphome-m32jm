#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "m32jm";

#define I2C_PORT             I2C_NUM_0
#define PIN_SDA              GPIO_NUM_21
#define PIN_SCL              GPIO_NUM_22
#define I2C_FREQ_HZ          100000
#define SENSOR_ADDR          0x28
#define I2C_TIMEOUT_MS       100
#define MR_TO_DF_DELAY_MS    10
#define READ_INTERVAL_MS     500

#define PRESSURE_FS_PSI      100.0f
#define PRESSURE_COUNTS_MIN  1000.0f
#define PRESSURE_COUNTS_MAX  15000.0f

static esp_err_t i2c_init(void) {
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_PORT, &cfg);
    if (err != ESP_OK) return err;
    return i2c_driver_install(I2C_PORT, cfg.mode, 0, 0, 0);
}

// MEAS/Honeywell "Measurement Request": READ header + immediate STOP, no data bytes.
// Not a register write — do not replace with beginTransmission/endTransmission patterns.
static esp_err_t m32jm_measurement_request(void) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SENSOR_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

// Data Fetch: fresh START (datasheet §1.8 forbids repeated-start during data), 4 bytes, NACK+STOP.
static esp_err_t m32jm_data_fetch(uint8_t *buf) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (SENSOR_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, buf, 3, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &buf[3], I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return err;
}

static const char *status_str(uint8_t s) {
    switch (s) {
        case 0x00: return "OK";
        case 0x01: return "reserved";
        case 0x02: return "stale";
        case 0x03: return "fault";
        default:   return "?";
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "M32JM-000105-100PG prototype starting");
    ESP_ERROR_CHECK(i2c_init());
    vTaskDelay(pdMS_TO_TICKS(20));

    while (1) {
        esp_err_t err = m32jm_measurement_request();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MR failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(MR_TO_DF_DELAY_MS));

        uint8_t buf[4] = {0};
        err = m32jm_data_fetch(buf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "DF failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
            continue;
        }

        uint8_t status = (buf[0] >> 6) & 0x03;
        uint16_t p_counts = ((uint16_t)(buf[0] & 0x3F) << 8) | buf[1];
        uint16_t t_counts = ((uint16_t)buf[2] << 3) | (buf[3] >> 5);

        float psi = ((float)p_counts - PRESSURE_COUNTS_MIN)
                    / (PRESSURE_COUNTS_MAX - PRESSURE_COUNTS_MIN) * PRESSURE_FS_PSI;
        float bar = psi * 0.0689476f;
        float temp_c = (float)t_counts * 200.0f / 2048.0f - 50.0f;

        ESP_LOGI(TAG,
                 "raw=%02X %02X %02X %02X  status=%s  P=%.2f psi (%.3f bar)  T=%.2f C",
                 buf[0], buf[1], buf[2], buf[3],
                 status_str(status), psi, bar, temp_c);

        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
    }
}
