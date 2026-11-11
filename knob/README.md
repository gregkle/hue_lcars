# LCARS Hue remote

Firmware for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8: a Hue remote with an LCARS-style UI.
It talks to the Hue bridge directly over WiFi.

Main screen, for the selected room or zone:

- **Turn the knob:** dims or brightens the room. The curved meter across the top shows its brightness.
- **Slide right to ALERT:** red alert. The room's colour bulbs flash red until reset, and the screen goes red. The slider only appears in rooms with colour bulbs.
- **Slide left to RESET:** the colour bulbs go back to white, matched to the brightness of the room's other lights.
- **ROOM:** pick the room or zone to control, or ALL LIGHTS. The choice is remembered across restarts.
- **COLOR:** colour control for the room. **LIGHTS** there lists the room's lights so you can control one on its own.

The colour control has three tabs:

- **COLOR:** 8 colours and 4 whites.
- **HUE:** the outer ring becomes a colour wheel; turn the knob to move the hue.
- **INTENSITY:** the outer ring becomes a brightness gauge; turn the knob to set it.

Swatches and the hue tab are greyed out for lights that can't do them. ON/OFF switches the target.

Settings (default room, step sizes, flash timing) are at the top of `hue_knob/hue_knob.ino`.

## Build

```sh
./setup.sh                                  # LVGL 8.4.0 + Waveshare patch, ArduinoJson, ESP32 core 3.2.0
cp hue_knob/secrets.example.h hue_knob/secrets.h   # then fill it in
arduino-cli compile --upload -p /dev/ttyACM0 \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc" \
  --libraries libraries --output-dir build hue_knob
```

On Linux the serial port needs a udev rule, for example:
`SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"`.

## Tools

- `tools/snap.py out.png [--toggle-alert]` saves a screenshot of the device over serial. `--toggle-alert` previews the red-alert look without touching any lights.
- Serial commands for testing (no light changes): `R` room picker, `C` colour control, `L` light list, `0`/`1`/`2` control tabs, `M` main screen, `S` screenshot, `T` red-alert preview.
- `tools/make_font.py FONT.ttf NAME SIZE [WEIGHT]` turns a font into an LVGL C font. The `lcars_*.c` fonts come from `fonts/Antonio.ttf` (SIL OFL, see `fonts/OFL.txt`).

Display, touch, encoder and backlight drivers come from Waveshare's demo code.
