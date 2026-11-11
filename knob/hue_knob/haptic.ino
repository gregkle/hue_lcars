// Vibration motor: a DRV2605 haptic driver on the touch screen's I2C bus (I2C_NUM_0, set up by
// Touch_Init). The motor is an ERM, driven open loop in real-time-playback mode, so buzz(true)
// runs it until buzz(false).

static const uint8_t DRV_ADDR = 0x5A;
static const uint8_t DRV_STATUS = 0x00, DRV_MODE = 0x01, DRV_RTPIN = 0x02, DRV_FEEDBACK = 0x1A,
                     DRV_CONTROL3 = 0x1D;
static const uint8_t DRV_MODE_RTP = 0x05, DRV_STANDBY = 0x40;
static bool haptic_ok = false;

static bool drv_write(uint8_t reg, uint8_t v) {
  uint8_t b[2] = {reg, v};
  return i2c_master_write_to_device(I2C_NUM_0, DRV_ADDR, b, 2, pdMS_TO_TICKS(50)) == ESP_OK;
}

static int drv_read(uint8_t reg) {
  uint8_t v;
  if (i2c_master_write_read_device(I2C_NUM_0, DRV_ADDR, &reg, 1, &v, 1, pdMS_TO_TICKS(50)) != ESP_OK)
    return -1;
  return v;
}

static void haptic_init() {
  int status = drv_read(DRV_STATUS);
  int id = status >> 5;  // 3/4/6/7: DRV2605 / 2604 and their L versions
  if (status < 0 || !(id == 3 || id == 4 || id == 6 || id == 7)) {
    Serial.printf("haptic: no DRV2605 (status %d)\n", status);
    return;
  }
  int fb = drv_read(DRV_FEEDBACK), c3 = drv_read(DRV_CONTROL3);
  haptic_ok = fb >= 0 && c3 >= 0 &&
              drv_write(DRV_MODE, 0x00) &&                // out of standby
              drv_write(DRV_FEEDBACK, fb & 0x7F) &&       // ERM motor
              drv_write(DRV_CONTROL3, (c3 | 0x20) & ~0x08) &&  // open loop, signed RTP values
              drv_write(DRV_RTPIN, 0) &&
              drv_write(DRV_MODE, DRV_STANDBY);
  Serial.printf("haptic: %s\n", haptic_ok ? "ready" : "init failed");
}

static void buzz(bool on) {
  if (!haptic_ok) return;
  if (on) {
    drv_write(DRV_RTPIN, BUZZ_STRENGTH);
    drv_write(DRV_MODE, DRV_MODE_RTP);
  } else {
    drv_write(DRV_RTPIN, 0);
    drv_write(DRV_MODE, DRV_STANDBY);
  }
}
