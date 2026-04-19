#include "m32jm.h"
#include "esphome/core/log.h"

namespace esphome {
namespace m32jm {

static const char *const TAG = "m32jm";

// M3200 datasheet §2.1: bridge counts 1000 = 0% FS, 15000 = 100% FS.
static constexpr float COUNTS_MIN = 1000.0f;
static constexpr float COUNTS_MAX = 15000.0f;

// Datasheet response time: 3 ms non-sleep, 8.4 ms sleep mode. 11 ms gives margin.
static constexpr uint32_t MR_TO_DF_DELAY_MS = 11;

void M32JM::setup() {
  ESP_LOGCONFIG(TAG, "Setting up M32JM...");
  delay(20);  // sensor power-on settling
}

void M32JM::dump_config() {
  ESP_LOGCONFIG(TAG, "M32JM:");
  LOG_I2C_DEVICE(this);
  ESP_LOGCONFIG(TAG, "  Full scale: %.1f psi", full_scale_psi_);
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Pressure", pressure_sensor_);
  LOG_SENSOR("  ", "Temperature", temperature_sensor_);
}

void M32JM::update() {
  // MEAS/Honeywell Measurement Request: READ header + immediate STOP, no data
  // bytes. Not a register write. The ESP-IDF I²C bus emits exactly this when
  // called with len=0.
  i2c::ErrorCode mr_err = this->read(nullptr, 0);
  if (mr_err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "Measurement request failed: %d", (int) mr_err);
    this->status_set_warning();
    return;
  }

  // Datasheet §1.8 forbids repeated-start during data, so we wait then issue a
  // fresh START via a scheduled callback (separate transaction).
  this->set_timeout("df", MR_TO_DF_DELAY_MS, [this]() { this->read_data_(); });
}

void M32JM::read_data_() {
  uint8_t buf[4];
  i2c::ErrorCode err = this->read(buf, sizeof(buf));
  if (err != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "Data fetch failed: %d", (int) err);
    this->status_set_warning();
    return;
  }

  uint8_t status = (buf[0] >> 6) & 0x03;
  if (status == 0x03) {
    ESP_LOGW(TAG, "Sensor fault status");
    this->status_set_warning();
    return;
  }
  if (status == 0x02) {
    // Stale = the previous sample, returned because MR-to-DF was too short.
    // Value is still valid, just one cycle behind.
    ESP_LOGD(TAG, "Stale data");
  }

  uint16_t p_counts = ((uint16_t) (buf[0] & 0x3F) << 8) | buf[1];
  uint16_t t_counts = ((uint16_t) buf[2] << 3) | (buf[3] >> 5);

  float psi = ((float) p_counts - COUNTS_MIN) / (COUNTS_MAX - COUNTS_MIN) * full_scale_psi_;
  float temp_c = (float) t_counts * 200.0f / 2048.0f - 50.0f;

  ESP_LOGD(TAG, "raw=%02X %02X %02X %02X  P=%.2f psi  T=%.2f C", buf[0], buf[1], buf[2], buf[3], psi, temp_c);

  if (this->pressure_sensor_ != nullptr)
    this->pressure_sensor_->publish_state(psi);
  if (this->temperature_sensor_ != nullptr)
    this->temperature_sensor_->publish_state(temp_c);

  this->status_clear_warning();
}

}  // namespace m32jm
}  // namespace esphome
