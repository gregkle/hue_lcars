# Hue alert knob

Firmware for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8, with an LCARS-style UI.

- **Slide right to ALERT:** the alert light (43) flashes red until reset.
- **Slide left to RESET:** light 43 goes back to white, matched to the other Treehouse lights.
- **Turn the knob:** dims or brightens the Treehouse room (Hue group 93). During an alert, light 43 is left out.

It talks to the Hue bridge directly over WiFi. Settings (lights, group, step size, timing) are at the top of `hue_knob/hue_knob.ino`.

## Build

```sh
./setup.sh                                  # LVGL 8.4.0 + Waveshare patch, ESP32 core 3.2.0
cp hue_knob/secrets.example.h hue_knob/secrets.h   # then fill it in
arduino-cli compile --upload -p /dev/ttyACM0 \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc" \
  --libraries libraries --output-dir build hue_knob
```

On Linux the serial port needs a udev rule, for example:
`SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"`.

## Tools

- `tools/snap.py out.png [--toggle-alert]` saves a screenshot of the device over serial. `--toggle-alert` previews the red-alert look without touching any lights.
- `tools/make_font.py FONT.ttf NAME SIZE [WEIGHT]` turns a font into an LVGL C font. The `lcars_*.c` fonts come from `fonts/Antonio.ttf` (SIL OFL, see `fonts/OFL.txt`).

Display, touch, encoder and backlight drivers come from Waveshare's demo code.
