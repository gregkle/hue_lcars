# Hue

Philips Hue projects:

- **[`knob/`](knob/README.md)**: firmware for an LCARS-style Hue remote on the Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (turn to dim, red alert, room and colour control).
- **`flash_red.py` / `flash_red.sh`**: small scripts that flash Hue lights red (for example from a router or a cron job).

No credentials are stored in this repo. Each project reads them from a git-ignored file that you create from an example (see below).

## Flashing the knob

You need the **Waveshare ESP32-S3-Knob-Touch-LCD-1.8**, a USB-C data cable, and a Linux or macOS machine with `git` and [`arduino-cli`](https://arduino.github.io/arduino-cli/latest/installation/).

### 1. Install arduino-cli

```sh
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh
```

(or `brew install arduino-cli` on macOS). Make sure `~/.local/bin` is on your `PATH`.

### 2. Fetch the dependencies

```sh
cd knob
./setup.sh
```

This clones LVGL 8.4.0 into `knob/libraries/lvgl` and applies Waveshare's one-line display fix (`lvgl-waveshare.patch`). It also installs the ESP32 Arduino core 3.2.0 and ArduinoJson 7.2.1. These exact versions are the ones the firmware is tested with. Run it once.

### 3. Get a Hue API username

Find the bridge's IP address in the Hue app (Settings → My Hue system → the bridge → ⓘ) or at <https://discovery.meethue.com>. Then **press the round link button on the bridge** and within 30 seconds run:

```sh
curl -X POST -d '{"devicetype":"hue_knob#esp32"}' http://<bridge-ip>/api
```

The reply contains `"username": "..."`. That long string is your API key.

### 4. Fill in your secrets

```sh
cp hue_knob/secrets.example.h hue_knob/secrets.h
```

Edit `hue_knob/secrets.h`:

| Setting      | Value                                                  |
|--------------|--------------------------------------------------------|
| `WIFI_SSID`  | your Wi-Fi network name; it must be **2.4 GHz** (the ESP32 can't use 5 GHz) |
| `WIFI_PASS`  | the Wi-Fi password                                     |
| `HUE_BRIDGE` | the bridge IP from step 3, e.g. `192.168.1.20`          |
| `HUE_USER`   | the username from step 3                               |

`secrets.h` is git-ignored, so it can't be committed by accident. Give the bridge a DHCP reservation in your router so its IP doesn't change.

### 5. Connect the knob and find its port

Plug the knob in over USB-C. On Linux it shows up as `/dev/ttyACM0` (check with `arduino-cli board list`). On macOS it looks like `/dev/cu.usbmodem*`.

On Linux, give your user access to the port once:

```sh
echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"' | sudo tee /etc/udev/rules.d/99-esp32.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

### 6. Compile and flash

From `knob/`:

```sh
arduino-cli compile --upload -p /dev/ttyACM0 \
  --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc" \
  --libraries libraries --output-dir build hue_knob
```

Replace `/dev/ttyACM0` with your port. The board options matter: the firmware needs the 16 MB flash, the octal PSRAM, and USB-CDC serial.

The knob reboots into the LCARS UI, joins Wi-Fi and loads the rooms from the bridge.

### Troubleshooting

- **Upload fails / port not found:** try another USB-C cable (some only carry power) and check `arduino-cli board list`. If the port still doesn't appear, or the old firmware has crashed, put the ESP32-S3 into download mode: hold its **BOOT** button while plugging in USB (Waveshare's wiki for the board shows where it is). Then upload again and power-cycle afterwards.
- **Stuck on connecting:** check that the network is 2.4 GHz and the SSID/password in `secrets.h` are correct. Watch the log with `arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200`.
- **Bridge errors / no rooms:** check `HUE_BRIDGE` and `HUE_USER`. `curl http://<bridge-ip>/api/<username>/groups` should return JSON, not an `unauthorized user` error.
- **Display glitches:** make sure `setup.sh` applied `lvgl-waveshare.patch`. If unsure, delete `knob/libraries/lvgl` and re-run it.

See [`knob/README.md`](knob/README.md) for how to use the remote, settings, and dev tools.

## Red-flash scripts

```sh
cp config.example.json config.json   # fill in bridge IP and username (from step 3 above)
python3 flash_red.py                 # needs `pip install requests`
python3 flash_red.py --reset         # back to white
```

`flash_red.sh` does the same with only `curl` and `jsonfilter`, for OpenWrt routers. Set the light IDs at the top of either script.
