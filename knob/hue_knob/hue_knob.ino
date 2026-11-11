// Hue alert knob for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//   Slide right to ALERT  -> flash the light red until reset
//   Slide left to RESET   -> reset the light to neutral white           (same as flash_red.py --reset)
//   Turn the knob         -> dim / brighten every light in the Treehouse room
// Talks to the Hue bridge directly over WiFi; no computer needed once flashed.
#include <WiFi.h>
#include <HTTPClient.h>
#include "lcd_bsp.h"
#include "cst816.h"
#include "lcd_bl_pwm_bsp.h"
#include "bidi_switch_knob.h"
#include "secrets.h"

static const char *LIGHTS[] = {"43"};  // Treehouse color
static const int INTERVAL_MS = 500;    // per on/off phase

static const char *DIM_GROUP = "93";  // "Treehouse" room on the bridge
static const int DIM_STEP = 16;        // brightness change per knob click (of 254)
static const bool CLOCKWISE_BRIGHTER = true;  // flip if the knob feels backwards
static const int ENCODER_A = 8, ENCODER_B = 7;
static const int BATTERY_PIN = 1;     // GPIO1 / ADC1_CH0, battery through a 1:2 divider
static const int SLIDE_TRIGGER = 90;   // how far (of 100) the slider must go to count

static const char *RED   = "{\"on\":true,\"bri\":254,\"xy\":[0.675,0.322],\"transitiontime\":0}";
static const char *DARK  = "{\"bri\":1,\"transitiontime\":0}";

// LCARS palette
static const uint32_t C_ORANGE = 0xFF9900, C_PEACH = 0xFFCC99, C_LILAC = 0xCC99CC, C_PURPLE = 0x9999FF,
                      C_SKY = 0x99CCFF, C_GOLD = 0xFFCC66, C_RED = 0xFF3333, C_ALERT1 = 0xCC2222,
                      C_ALERT2 = 0xFF5544, C_ALERT3 = 0x992222, C_DIM = 0x2A2A2A;
static const uint32_t C_WARM = C_PEACH, C_GREY = C_PURPLE;  // status colours used below

enum Cmd : uint8_t { CMD_NONE, CMD_FLASH, CMD_RESET };
static volatile Cmd pending = CMD_NONE;  // latest request wins
static TaskHandle_t worker = nullptr;
static TaskHandle_t dimmer = nullptr;
static volatile bool flashing = false;  // the alert light is mid-flash

// Dimmer state. The knob updates `level` instantly (so the ring keeps up with your hand);
// the bridge is only asked for the real value once the knob has been still for a moment.
static volatile int knob_delta = 0;      // clicks not yet sent to the bridge
static volatile int level = -1;          // estimated group brightness 1..254, -1 = unknown
static volatile bool lights_on = false;
static volatile bool level_dirty = false;
static volatile uint32_t last_knob_ms = 0;

extern "C" {
extern const lv_font_t lcars_18, lcars_26, lcars_40;
}

static lv_obj_t *slider, *status, *title, *battery_label, *battery_block;
static lv_obj_t *level_blocks[10];
static volatile bool preview_alert = false;  // serial 'T': show the red-alert look without touching lights
static volatile bool alert_active = false;   // alert light is red: keep it out of the dimmer
static bool themed_alert = false;

// Every coloured LCARS panel, with its normal and red-alert colour.
struct Panel { lv_obj_t *obj; uint32_t normal, alert; };
static Panel panels[24];
static int n_panels = 0;

// ---------- UI (call only with the LVGL lock held) ----------
static void set_status(uint32_t color, const char *text) {
  lv_obj_set_style_text_color(status, lv_color_hex(color), 0);
  lv_label_set_text(status, text);
}

static void show(uint32_t color, const char *text) {
  if (example_lvgl_lock(-1)) {
    set_status(color, text);
    example_lvgl_unlock();
  }
}

static void request(Cmd c) {
  Serial.printf("[%lu] request %s\n", millis(), c == CMD_FLASH ? "FLASH" : "RESET");
  pending = c;
  if (worker) xTaskNotifyGive(worker);
}

static void on_slider(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  int v = lv_slider_get_value(slider);
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    Serial.printf("[%lu] slider released at %d\n", millis(), v);
    if (v >= SLIDE_TRIGGER) request(CMD_FLASH);
    else if (v <= -SLIDE_TRIGGER) request(CMD_RESET);
    lv_slider_set_value(slider, 0, LV_ANIM_ON);  // spring back to the middle
  }
}

static lv_obj_t *block(int x, int y, int w, int h, int radius, uint32_t color, uint32_t alert_color) {
  lv_obj_t *o = lv_obj_create(lv_scr_act());
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  if (color != 0 && n_panels < 24) panels[n_panels++] = {o, color, alert_color};
  return o;
}
static lv_obj_t *black(int x, int y, int w, int h, int radius) { return block(x, y, w, h, radius, 0, 0); }

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *t) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, t);
  return l;
}

// Black lettering tucked into the bottom-right of a panel, LCARS style.
static void panel_code(lv_obj_t *panel, const char *t) {
  lv_obj_t *l = text(panel, &lcars_18, 0x000000, t);
  lv_obj_align(l, LV_ALIGN_BOTTOM_RIGHT, -6, -1);
}

static void apply_theme(bool alert) {
  for (int i = 0; i < n_panels; i++)
    lv_obj_set_style_bg_color(panels[i].obj, lv_color_hex(alert ? panels[i].alert : panels[i].normal), 0);
  lv_obj_set_style_text_color(title, lv_color_hex(alert ? C_ALERT2 : C_ORANGE), 0);
  themed_alert = alert;
}

// Runs in the LVGL task every 20 ms: brightness bar, theme, red-alert blink.
static void ui_timer_cb(lv_timer_t *) {
  bool alert = alert_active || preview_alert;
  if (alert != themed_alert) apply_theme(alert);

  static uint32_t last_blink = 0;
  static bool blink_on = true;
  if (alert && millis() - last_blink > 500) {
    last_blink = millis();
    blink_on = !blink_on;
    lv_obj_set_style_text_opa(status, blink_on ? LV_OPA_COVER : LV_OPA_20, 0);
  } else if (!alert && !blink_on) {
    blink_on = true;
    lv_obj_set_style_text_opa(status, LV_OPA_COVER, 0);
  }

  if (level_dirty) {
    level_dirty = false;
    int bri = level;
    if (bri >= 0) {
      int lit = lights_on ? (bri * 10 + 253) / 254 : 0;
      for (int i = 0; i < 10; i++)
        lv_obj_set_style_bg_color(level_blocks[i], lv_color_hex(i < lit ? C_GOLD : C_DIM), 0);
    }
  }
}

// ---------- Battery ----------
// Single-cell LiPo resting voltage -> charge, roughly.
static int battery_percent(float v) {
  static const float V[] = {3.30, 3.50, 3.60, 3.70, 3.75, 3.80, 3.85, 3.90, 4.00, 4.10, 4.20};
  static const int   P[] = {   0,    5,   10,   20,   30,   40,   50,   60,   75,   90,  100};
  if (v <= V[0]) return 0;
  for (int i = 1; i < 11; i++)
    if (v < V[i]) return P[i - 1] + (P[i] - P[i - 1]) * (v - V[i - 1]) / (V[i] - V[i - 1]);
  return 100;
}

static void battery_timer_cb(lv_timer_t *) {  // runs in the LVGL task
  static float smooth = 0;
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BATTERY_PIN);
  float v = mv / 16.0f * 2 / 1000.0f;
  smooth = smooth == 0 ? v : smooth * 0.8f + v * 0.2f;  // steady the reading

  int pct = battery_percent(smooth);
  bool charging = smooth > 4.25f;  // above a full cell = charger is driving it
  if (charging) lv_label_set_text(battery_label, "PWR EXT");
  else          lv_label_set_text_fmt(battery_label, "PWR %d%%", pct);
  // Little status block next to the bar: blue normally, gold on charge, red when low.
  uint32_t c = charging ? C_GOLD : pct <= 15 ? C_RED : C_SKY;
  for (int i = 0; i < n_panels; i++)
    if (panels[i].obj == battery_block) panels[i].normal = c;
  if (!themed_alert) lv_obj_set_style_bg_color(battery_block, lv_color_hex(c), 0);
  static int n = 0;
  if (n++ % 6 == 0) Serial.printf("battery %.2f V (%d%%)\n", smooth, pct);
}

extern "C" void ui_build(void) {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // The frame: a top elbow and a bottom elbow joined by a segmented left column.
  // Each elbow is a big rounded panel with a black rounded panel cut out of it; the round
  // display itself trims the outer corners.
  lv_obj_t *top = block(40, 40, 190, 200, 44, C_ORANGE, C_ALERT1);
  black(92, 72, 230, 140, 22);
  lv_obj_t *bottom = block(40, 200, 190, 148, 44, C_PEACH, C_ALERT2);
  black(92, 100, 230, 216, 22);  // starts high so its rounded corner hides inside the top cutout
  // column segments (drawn over the elbows' inner ends), separated by 4 px black gaps
  black(40, 164, 52, 4, 0);
  lv_obj_t *seg1 = block(40, 168, 52, 44, 0, C_SKY, C_ALERT3);
  black(40, 212, 52, 4, 0);
  lv_obj_t *seg2 = block(40, 216, 52, 28, 0, C_LILAC, C_ALERT1);
  black(40, 244, 52, 4, 0);
  lv_obj_set_pos(text(scr, &lcars_18, 0x000000, "93"), 68, 140);  // bottom of the top elbow's column
  panel_code(seg1, "43");
  panel_code(seg2, "07");
  (void)bottom;
  // top bar continues to the right
  block(234, 40, 36, 32, 0, C_PEACH, C_ALERT3);
  block(274, 40, 60, 32, 16, C_LILAC, C_ALERT2);
  // bottom bar: battery readout in the elbow, then a status block
  battery_label = text(bottom, &lcars_18, 0x000000, "PWR");
  lv_obj_align(battery_label, LV_ALIGN_BOTTOM_RIGHT, -8, -5);
  battery_block = block(234, 316, 30, 32, 0, C_SKY, C_ALERT3);
  block(268, 316, 60, 32, 16, C_PURPLE, C_ALERT1);

  title = text(scr, &lcars_26, C_ORANGE, "TREEHOUSE");
  lv_obj_align(title, LV_ALIGN_TOP_RIGHT, -46, 76);

  // Illumination: ten blocks, lit by the room's brightness
  lv_obj_t *il = text(scr, &lcars_18, C_PEACH, "ILLUMINATION");
  lv_obj_set_pos(il, 104, 110);
  for (int i = 0; i < 10; i++)
    level_blocks[i] = block(104 + i * 22, 134, 18, 24, 0, 0, 0);  // coloured by ui_timer_cb
  for (int i = 0; i < 10; i++) lv_obj_set_style_bg_color(level_blocks[i], lv_color_hex(C_DIM), 0);

  // Slider: RESET pill on the left, ALERT pill on the right, orange handle in the middle.
  const int SX = 104, SY = 172, SW = 216, SH = 56;
  lv_obj_t *rp = block(SX, SY, 64, SH, SH / 2, C_LILAC, C_LILAC);
  lv_obj_t *ap = block(SX + SW - 64, SY, 64, SH, SH / 2, C_RED, C_RED);
  lv_obj_align(text(rp, &lcars_26, 0x000000, "RESET"), LV_ALIGN_CENTER, 0, 0);
  lv_obj_align(text(ap, &lcars_26, 0x000000, "ALERT"), LV_ALIGN_CENTER, 0, 0);
  block(SX + 64, SY + SH / 2 - 2, SW - 128, 4, 0, C_PEACH, C_ALERT2);  // rail

  slider = lv_slider_create(scr);
  lv_obj_set_pos(slider, SX + SH / 2, SY);
  lv_obj_set_size(slider, SW - SH, SH);  // handle centre travels the full track
  lv_slider_set_mode(slider, LV_SLIDER_MODE_SYMMETRICAL);
  lv_slider_set_range(slider, -100, 100);
  lv_slider_set_value(slider, 0, LV_ANIM_OFF);
  lv_obj_add_flag(slider, LV_OBJ_FLAG_ADV_HITTEST);  // only the handle can be grabbed
  lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_hex(C_ORANGE), LV_PART_KNOB);
  lv_obj_set_style_radius(slider, 10, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
  lv_obj_set_style_border_color(slider, lv_color_black(), LV_PART_KNOB);
  lv_obj_set_style_border_width(slider, 4, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
  lv_obj_set_style_anim_time(slider, 250, 0);
  lv_obj_set_ext_click_area(slider, 12);
  lv_obj_add_event_cb(slider, on_slider, LV_EVENT_ALL, nullptr);

  status = text(scr, &lcars_40, C_GREY, "");
  lv_obj_set_width(status, 216);
  lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(status, 104, 240);
  set_status(C_GREY, "CONNECTING");

  lv_timer_create(ui_timer_cb, 20, nullptr);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
  lv_timer_t *bt = lv_timer_create(battery_timer_cb, 5000, nullptr);
  lv_timer_ready(bt);  // first reading right away
}

// ---------- Screenshots over serial ----------
// Send 'S' and the board answers "SNAP", width, height (uint16 LE), then raw RGB565 pixels.
static void send_snapshot() {
  const uint32_t size = 360 * 360 * 2;
  uint8_t *buf = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (!buf) return;
  lv_img_dsc_t dsc;
  bool ok = false;
  if (example_lvgl_lock(-1)) {
    ok = lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR, &dsc, buf, size) == LV_RES_OK;
    example_lvgl_unlock();
  }
  if (ok) {
    uint16_t wh[2] = {(uint16_t)dsc.header.w, (uint16_t)dsc.header.h};
    Serial.write((const uint8_t *)"SNAP", 4);
    Serial.write((const uint8_t *)wh, 4);
    for (uint32_t i = 0; i < size; i += 4096) Serial.write(buf + i, min<uint32_t>(4096, size - i));
    Serial.flush();
  }
  free(buf);
}

// ---------- Hue ----------
static String hue_url(const String &path) {
  return String("http://") + HUE_BRIDGE + "/api/" + HUE_USER + path;
}

static bool set_state(const char *body) {
  if (WiFi.status() != WL_CONNECTED) return false;
  bool ok = true;
  for (const char *light : LIGHTS) {
    HTTPClient http;
    http.setTimeout(5000);
    http.begin(hue_url(String("/lights/") + light + "/state"));
    http.addHeader("Content-Type", "application/json");
    int code = http.PUT((uint8_t *)body, strlen(body));
    String resp = code > 0 ? http.getString() : String();
    http.end();
    Serial.printf("PUT light %s -> %d %s\n", light, code, resp.c_str());
    if (code != 200 || resp.indexOf("\"error\"") >= 0) ok = false;
  }
  return ok;
}

// ---------- Treehouse dimmer ----------
static String group_lights[16];          // light ids in the Treehouse room, read from the bridge
static int n_group_lights = 0;

static bool is_alert_light(const String &id) {
  for (const char *l : LIGHTS) if (id == l) return true;
  return false;
}

static bool knob_recently_used(uint32_t ms) { return millis() - last_knob_ms < ms; }

static bool hue_get(const String &path, String &out) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(hue_url(path));
  bool ok = http.GET() == 200;
  if (ok) out = http.getString();
  http.end();
  return ok;
}

static int hue_put(const String &path, const char *body) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(hue_url(path));
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT((uint8_t *)body, strlen(body));
  http.end();
  return code;
}

static void load_group_lights() {
  String r;
  if (!hue_get(String("/groups/") + DIM_GROUP, r)) return;
  int a = r.indexOf("\"lights\":[");
  if (a < 0) return;
  int end = r.indexOf(']', a), p = a + 10, n = 0;
  while (n < 16) {
    int q1 = r.indexOf('"', p);
    if (q1 < 0 || q1 > end) break;
    int q2 = r.indexOf('"', q1 + 1);
    group_lights[n++] = r.substring(q1 + 1, q2);
    p = q2 + 1;
  }
  n_group_lights = n;
  Serial.printf("Treehouse has %d lights\n", n);
}

// Average brightness of the room's lights that are on, optionally skipping the alert light.
// Returns -1 if none are on.
static int average_bri(bool skip_alert) {
  int sum = 0, count = 0;
  for (int i = 0; i < n_group_lights; i++) {
    if (skip_alert && is_alert_light(group_lights[i])) continue;
    String r;
    if (!hue_get("/lights/" + group_lights[i], r)) continue;
    if (r.indexOf("\"on\":true") < 0) continue;
    int b = r.indexOf("\"bri\":");
    if (b < 0) continue;
    sum += r.substring(b + 6).toInt();
    count++;
  }
  return count ? (sum + count / 2) / count : -1;
}

// Reads the real brightness (picks up changes from the Hue app, corrects drift).
static void refresh_level() {
  if (n_group_lights == 0) load_group_lights();
  if (n_group_lights == 0) return;
  int bri = average_bri(alert_active);
  if (knob_recently_used(1500)) return;  // don't fight the user mid-turn
  lights_on = bri >= 0;
  if (bri >= 0) level = bri;
  else if (level < 0) level = 1;
  level_dirty = true;
}

static void on_knob(void *, void *dir) {  // runs in the encoder's timer callback
  int d = (intptr_t)dir;
  if (!CLOCKWISE_BRIGHTER) d = -d;
  __atomic_add_fetch(&knob_delta, d, __ATOMIC_RELAXED);
  last_knob_ms = millis();
  if (level >= 0) {
    if (d > 0) lights_on = true;  // turning up switches the lights on
    level = constrain(level + d * DIM_STEP, 1, 254);
    level_dirty = true;
  }
  if (dimmer) xTaskNotifyGive(dimmer);
}

static void dimmer_task(void *) {
  bool settle = true;  // fetch the real level once things go quiet
  for (;;) {
    // Wake on knob movement; otherwise re-sync 1.5 s after the last turn and every 30 s.
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(settle ? 1500 : 30000)) == 0) {
      refresh_level();
      settle = false;
      continue;
    }
    int clicks;
    while ((clicks = __atomic_exchange_n(&knob_delta, 0, __ATOMIC_RELAXED)) != 0) {
      if (WiFi.status() != WL_CONNECTED) break;
      int inc = constrain(clicks * DIM_STEP, -254, 254);
      char body[64];
      snprintf(body, sizeof body, inc > 0 ? "{\"on\":true,\"bri_inc\":%d,\"transitiontime\":2}"
                                           : "{\"bri_inc\":%d,\"transitiontime\":2}", inc);
      if (!alert_active || n_group_lights == 0) {
        int code = hue_put(String("/groups/") + DIM_GROUP + "/action", body);
        Serial.printf("[%lu] dim room %+d -> %d\n", millis(), inc, code);
        vTaskDelay(pdMS_TO_TICKS(250));  // Hue asks for <= ~4 group commands/s; extra clicks batch up
      } else {
        // Red alert: dim every Treehouse light except the alert light, one at a time.
        for (int i = 0; i < n_group_lights; i++) {
          if (is_alert_light(group_lights[i])) continue;
          int code = hue_put("/lights/" + group_lights[i] + "/state", body);
          Serial.printf("[%lu] dim light %s %+d -> %d\n", millis(), group_lights[i].c_str(), inc, code);
        }
        vTaskDelay(pdMS_TO_TICKS(800));  // per-light commands are rate limited harder (and 43 is flashing)
      }
    }
    settle = true;
  }
}

// ---------- Alert / reset ----------
// Sleeps for ms, returning true early if a new command arrived (so a reset can interrupt a flash).
static bool interrupted(int ms) {
  return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms)) > 0;
}

static const char *ALERT_TEXT = "RED ALERT";
static void idle_screen() {
  if (alert_active) show(C_RED, ALERT_TEXT);
  else              show(C_ORANGE, "STANDBY");
}

static void worker_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (pending != CMD_NONE) {
      Cmd c = pending;
      pending = CMD_NONE;
      bool ok = true;
      if (c == CMD_RESET) {
        show(C_WARM, "RESETTING");
        // Back to white, matched to the rest of the Treehouse.
        int bri = average_bri(true);
        if (bri < 0) bri = 254;
        char white[64];
        snprintf(white, sizeof white, "{\"on\":true,\"bri\":%d,\"ct\":366}", bri);
        ok = set_state(white);
        if (ok) alert_active = false;
        if (ok) { show(C_WARM, "LIGHT RESET"); interrupted(1500); }
      } else {
        // Flash until a reset (or a new alert) comes in.
        alert_active = true;
        flashing = true;
        show(C_RED, ALERT_TEXT);
        bool was_ok = true;
        for (uint32_t i = 1; pending == CMD_NONE; i++) {
          bool cycle_ok = set_state(RED);
          if (interrupted(INTERVAL_MS)) break;
          cycle_ok &= set_state(DARK);
          if (interrupted(INTERVAL_MS)) break;
          if (cycle_ok != was_ok) {  // only touch the screen when things change
            if (cycle_ok) show(C_RED, ALERT_TEXT);
            else show(C_ORANGE, WiFi.status() == WL_CONNECTED ? "NO BRIDGE" : "NO WIFI");
            was_ok = cycle_ok;
          }
          if (i % 20 == 0) Serial.printf("[%lu] still flashing (%lu cycles)\n", millis(), (unsigned long)i);
        }
        flashing = false;
        Serial.printf("[%lu] flash stopped, pending=%d\n", millis(), pending);
      }
      if (!ok) {
        show(C_ORANGE, WiFi.status() == WL_CONNECTED ? "BRIDGE ERROR" : "NO WIFI");
        interrupted(2500);
      }
      if (pending == CMD_NONE) idle_screen();
    }
  }
}

// ---------- WiFi ----------
static void on_wifi(WiFiEvent_t ev) {
  if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.printf("WiFi up: %s\n", WiFi.localIP().toString().c_str());
    idle_screen();
    if (dimmer) xTaskNotifyGive(dimmer);  // triggers a level refresh (no clicks pending)
  } else if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    show(C_GREY, "CONNECTING");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("boot, reset reason %d\n", (int)esp_reset_reason());
  Touch_Init();
  lcd_lvgl_Init();                 // calls ui_build()
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

  xTaskCreate(worker_task, "hue", 6144, nullptr, 3, &worker);
  xTaskCreate(dimmer_task, "dim", 6144, nullptr, 3, &dimmer);

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
    if (c == 'T') {
      preview_alert = !preview_alert;
      if (preview_alert) show(C_RED, "RED ALERT");
      else idle_screen();
    }
  }
  vTaskDelay(pdMS_TO_TICKS(50));
}
