#!/bin/sh
# Fetch LVGL v8.4.0 (with Waveshare's one-line display fix), ArduinoJson and the ESP32 Arduino core.
set -e
cd "$(dirname "$0")"

if [ ! -d libraries/lvgl ]; then
    git clone --depth 1 -b v8.4.0 https://github.com/lvgl/lvgl libraries/lvgl
    git -C libraries/lvgl apply ../../lvgl-waveshare.patch
fi

if command -v arduino-cli >/dev/null 2>&1; then
    arduino-cli config add board_manager.additional_urls \
        https://espressif.github.io/arduino-esp32/package_esp32_index.json 2>/dev/null || true
    arduino-cli core update-index
    arduino-cli core install esp32:esp32@3.2.0
    arduino-cli lib install ArduinoJson@7.2.1
else
    echo "arduino-cli not found: install it, then re-run (see README.md)" >&2
fi

[ -f hue_knob/secrets.h ] || echo "Now copy hue_knob/secrets.example.h to hue_knob/secrets.h and fill it in."
