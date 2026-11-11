// LCARS Hue remote for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//
// Main screen (for the selected room or zone):
//   Turn the knob          -> dim / brighten the room
//   Hold RED ALERT (1 s)   -> red alert: the room's colour bulbs flash red until reset
//   Hold RESET (1 s)       -> colour bulbs back to white, matched to the rest of the room
//   ROOM                   -> pick which room / zone the remote controls (remembered)
//   COLOR                  -> colour / white temperature / intensity for the room, or (LIGHTS) one light
//
// Talks to the Hue bridge directly over WiFi; no computer needed once flashed.
// The other .ino files in this folder are compiled together with this one, in name order:
//   alert.ino   red alert / reset worker
//   bridge.ino  bridge HTTP, room + light model, room dimmer
//   ui_kit.ino  LCARS building blocks
//   ui_main.ino main screen, battery, screenshots
//   ui_menu.ino room picker, light list, colour control
#include <algorithm>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "bidi_switch_knob.h"
#include "secrets.h"

// ---------- Settings ----------
static const char *DEFAULT_ROOM = "93";        // used until a room is picked on the device
static const int FLASH_MS = 500;               // red-alert on/off phase (longer for many bulbs)
static const int DIM_STEP = 16;                // main-screen brightness change per knob click
static const int CT_STEP = 10;                 // colour control: white temperature (mireds) per click
static const int BRI_STEP = 13;                // colour control: ~5% brightness per click
static const bool CLOCKWISE_BRIGHTER = true;   // flip if the knob feels backwards
static const int ENCODER_A = 8, ENCODER_B = 7;
static const int BATTERY_PIN = 1;              // GPIO1 / ADC1_CH0, battery through a 1:2 divider
static const int HOLD_MS = 1000;               // how long to hold RED ALERT / RESET
static const bool BUZZ_ON_ALERT = true;        // vibrate on and off with the red-alert flashes
static const uint8_t BUZZ_STRENGTH = 127;      // vibration strength, 1..127

// LCARS palette
static const uint32_t C_ORANGE = 0xFF9900, C_PEACH = 0xFFCC99, C_LILAC = 0xCC99CC, C_PURPLE = 0x9999FF,
                      C_SKY = 0x99CCFF, C_GOLD = 0xFFCC66, C_RED = 0xFF3333, C_ALERT1 = 0xCC2222,
                      C_ALERT2 = 0xFF5544, C_ALERT3 = 0x992222, C_DIM = 0x2A2A2A;
static const uint32_t C_WARM = C_PEACH, C_GREY = C_PURPLE;

// ---------- Bridge model (filled by load_model, guarded by model_mux) ----------
static const int MAX_LIGHTS = 64, MAX_ROOMS = 40, MAX_MEMBERS = 48;
struct HueLight { char id[6]; char name[32]; bool color, ct; };
struct HueRoom  { char id[6]; char name[32]; char member[MAX_MEMBERS][6]; uint8_t n, n_color; };
static HueLight lights[MAX_LIGHTS];
static HueRoom rooms[MAX_ROOMS];  // rooms[0] is the synthetic "ALL LIGHTS" (group 0)
static int n_lights = 0, n_rooms = 0;
static SemaphoreHandle_t model_mux;
static volatile int model_gen = 0;  // bumped after every successful load

// The room / zone the main screen controls.
static char sel_room[6];
static Preferences prefs;

// ---------- Red alert ----------
enum Cmd : uint8_t { CMD_NONE, CMD_FLASH, CMD_RESET };
static volatile Cmd pending = CMD_NONE;     // latest request wins
static volatile bool alert_active = false;  // alert bulbs are red: the dimmer leaves them alone
static char alert_ids[16][6];               // the bulbs flashing
static volatile int n_alert = 0;
static volatile bool preview_alert = false; // serial 'T': red-alert look without touching lights
static TaskHandle_t worker = nullptr;

// ---------- Main-screen dimmer ----------
// The knob updates `level` instantly (so the display keeps up with your hand);
// the bridge is only asked for the real value once the knob has been still for a moment.
static TaskHandle_t dimmer = nullptr;
static volatile int knob_delta = 0;   // clicks not yet sent to the bridge
static volatile int level = -1;       // estimated room brightness 1..254 (0 = off), -1 = unknown
static volatile bool lights_on = false;
static volatile bool level_dirty = false;
static volatile uint32_t last_knob_ms = 0;

// ---------- UI ----------
enum UiMode : uint8_t { MODE_MAIN, MODE_LIST, MODE_CONTROL };
static volatile UiMode ui_mode = MODE_MAIN;  // where knob clicks go
static volatile int menu_clicks = 0;         // knob clicks for the menus, consumed by the LVGL task

// What the colour control is pointed at: a room / zone (group) or a single light.
struct Target { bool group; char id[6]; char name[32]; bool color, ct; };

// Every themed LCARS panel on the main screen, with its normal and red-alert colour.
struct Panel { lv_obj_t *obj; uint32_t normal, alert; };

extern "C" {
extern const lv_font_t lcars_16, lcars_22, lcars_34;
}

// ---------- Cross-file declarations ----------
// bridge.ino
static String hue_url(const String &path);
static bool hue_get(const String &path, String &out);
static int hue_put(const String &path, const char *body);
static bool load_model();
static bool get_room(const char *id, HueRoom &out);
static bool get_light(const char *id, HueLight &out);
static bool is_alert_light(const char *id);
static int room_average_bri(const HueRoom &room, char (*skip)[6], int n_skip);
static void select_room(const char *id);
static void on_knob(void *, void *dir);
static void dimmer_task(void *);
// alert.ino
static void request(Cmd c);
static void idle_screen();
static void worker_task(void *);
static void haptic_init();
static void buzz(bool on);
// ui_main.ino
static void show(uint32_t color, const char *text);
static void send_snapshot();
static void debug_press(bool on);
// ui_menu.ino
static void menu_init();
static void menu_tick();
static void open_room_picker();
static void open_room_control();
static void debug_nav(char c);

// ---------- WiFi ----------
static void on_wifi(WiFiEvent_t ev) {
  if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("WiFi up: %s\n", WiFi.localIP().toString().c_str());
    idle_screen();
    if (dimmer) xTaskNotifyGive(dimmer);  // loads the rooms and the current brightness
  } else if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    show(C_GREY, "CONNECTING");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("boot, reset reason %d\n", (int)esp_reset_reason());
  model_mux = xSemaphoreCreateMutex();
  prefs.begin("knob");
  strlcpy(sel_room, prefs.getString("room", DEFAULT_ROOM).c_str(), sizeof sel_room);

  Touch_Init();
  haptic_init();                   // the vibration motor shares the touch I2C bus
  lcd_lvgl_Init();                 // calls ui_build()
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

  xTaskCreate(worker_task, "alert", 6144, nullptr, 3, &worker);
  xTaskCreate(dimmer_task, "dim", 8192, nullptr, 3, &dimmer);
  menu_init();

  knob_config_t kcfg = {.gpio_encoder_a = ENCODER_A, .gpio_encoder_b = ENCODER_B};
  knob_handle_t knob = iot_knob_create(&kcfg);
  if (knob) {
    iot_knob_register_cb(knob, KNOB_RIGHT, on_knob, (void *)(intptr_t)1);
    iot_knob_register_cb(knob, KNOB_LEFT, on_knob, (void *)(intptr_t)-1);
  } else {
    Serial.println("knob create failed");
  }

  WiFi.onEvent(on_wifi);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void loop() {
  // Serial debug commands: S = screenshot, T = toggle red-alert preview (lights untouched)
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'S') send_snapshot();
    if (c == 'B') { buzz(true); delay(300); buzz(false); }  // test the vibration motor
    if (c == 'T') {
      preview_alert = !preview_alert;
      idle_screen();
    }
    // Navigation only (no light changes): R room picker, C colour control, L light list,
    // 0/1/2 control tabs, M main screen.
    if (strchr("RCL012M", c) && c && example_lvgl_lock(-1)) {
      debug_nav(c);
      example_lvgl_unlock();
    }
    // P / p: show / clear the alert button's pressed look (never fires it)
    if ((c == 'P' || c == 'p') && example_lvgl_lock(-1)) {
      debug_press(c == 'P');
      example_lvgl_unlock();
    }
  }
  vTaskDelay(pdMS_TO_TICKS(50));
}
