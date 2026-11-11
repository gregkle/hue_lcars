// Main screen: room title, brightness meter, RED ALERT / RESET hold button, ROOM and COLOR buttons,
// status, battery. Plus the red-alert theme and serial screenshots.

static lv_obj_t *main_scr, *status, *title, *battery_label, *battery_block;
static lv_obj_t *alert_btn, *alert_text;  // alert_btn is an lv_bar: its indicator is the hold fill
static bool alert_fired = false;  // this press already triggered
static const int AB_W = 190, AB_H = 46, AB_X = 180 - AB_W / 2, AB_Y = 104, AB_INSET = 5;
static const int N_LEVEL = 12;
static lv_obj_t *level_blocks[N_LEVEL];
static bool themed_alert = false;
static int shown_model_gen = -1;
static char shown_room[6] = "";

static void set_status(uint32_t color, const char *t) {
  lv_obj_set_style_text_color(status, lv_color_hex(color), 0);
  lv_label_set_text(status, t);
}

static void show(uint32_t color, const char *t) {
  if (example_lvgl_lock(-1)) {
    set_status(color, t);
    example_lvgl_unlock();
  }
}

// ---------- RED ALERT / RESET hold button ----------
// Pressing sinks the button a little and grows a fill across it; it fires when the fill completes
// (HOLD_MS). Letting go early drains it again. The button is a bar widget, so the fill stays
// inside its rounded ends; the press effect changes its real size (no transform layers).
static void fill_exec(void *, int32_t v) { lv_bar_set_value(alert_btn, v, LV_ANIM_OFF); }

static void inset_exec(void *, int32_t d) {
  lv_obj_set_pos(alert_btn, AB_X + d, AB_Y + d / 2);
  lv_obj_set_size(alert_btn, AB_W - 2 * d, AB_H - d);
}

static void run_anim(lv_anim_exec_xcb_t exec, int from, int to, int ms, lv_anim_ready_cb_t done) {
  lv_anim_del(alert_btn, exec);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, alert_btn);
  lv_anim_set_exec_cb(&a, exec);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_time(&a, max(ms, 1));
  if (done) lv_anim_set_ready_cb(&a, done);
  lv_anim_start(&a);
}

static int current_inset() { return (AB_W - lv_obj_get_width(alert_btn)) / 2; }

static void fill_complete(lv_anim_t *) {
  alert_fired = true;
  request(alert_active ? CMD_RESET : CMD_FLASH);
  lv_obj_set_style_bg_color(alert_btn, lv_color_white(), LV_PART_INDICATOR);  // brief white: "got it"
}

static void press_look(bool pressed) {
  run_anim(inset_exec, current_inset(), pressed ? AB_INSET : 0, 90, nullptr);
}

static void on_alert_btn(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  int v = lv_bar_get_value(alert_btn);
  if (code == LV_EVENT_PRESSED) {
    alert_fired = false;
    lv_obj_set_style_bg_color(alert_btn, lv_color_hex(alert_active ? C_PEACH : C_GOLD), LV_PART_INDICATOR);
    press_look(true);
    run_anim(fill_exec, v, 1000, HOLD_MS * (1000 - v) / 1000, fill_complete);
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    press_look(false);
    run_anim(fill_exec, v, 0, alert_fired ? 400 : 200, nullptr);
  }
}

// Serial 'P' / 'p': show / clear the pressed look without firing (for screenshots).
static void debug_press(bool on) {
  lv_anim_del(alert_btn, nullptr);
  inset_exec(nullptr, on ? AB_INSET : 0);
  lv_obj_set_style_bg_color(alert_btn, lv_color_hex(alert_active ? C_PEACH : C_GOLD), LV_PART_INDICATOR);
  lv_bar_set_value(alert_btn, on ? 500 : 0, LV_ANIM_OFF);
}

static void build_alert_button(lv_obj_t *scr) {
  alert_btn = lv_bar_create(scr);
  lv_obj_set_pos(alert_btn, AB_X, AB_Y);
  lv_obj_set_size(alert_btn, AB_W, AB_H);
  lv_bar_set_range(alert_btn, 0, 1000);
  lv_bar_set_value(alert_btn, 0, LV_ANIM_OFF);
  for (lv_part_t part : {LV_PART_MAIN, LV_PART_INDICATOR}) {
    lv_obj_set_style_radius(alert_btn, AB_H / 2, part);
    lv_obj_set_style_bg_opa(alert_btn, LV_OPA_COVER, part);
    lv_obj_set_style_border_width(alert_btn, 0, part);
    lv_obj_set_style_pad_all(alert_btn, 0, part);
  }
  lv_obj_set_style_bg_color(alert_btn, lv_color_hex(C_RED), LV_PART_MAIN);
  lv_obj_set_style_bg_color(alert_btn, lv_color_hex(C_GOLD), LV_PART_INDICATOR);
  lv_obj_set_style_anim_time(alert_btn, 0, 0);
  lv_obj_add_flag(alert_btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(alert_btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_ext_click_area(alert_btn, 8);
  alert_text = text(alert_btn, &lcars_22, 0x000000, "RED ALERT");
  lv_obj_center(alert_text);
  lv_obj_add_event_cb(alert_btn, on_alert_btn, LV_EVENT_ALL, nullptr);
}

static void apply_theme(bool alert) {
  for (int i = 0; i < n_panels; i++)
    paint(panels[i].obj, alert ? panels[i].alert : panels[i].normal);
  lv_obj_set_style_text_color(title, lv_color_hex(alert ? C_ALERT2 : C_ORANGE), 0);
  themed_alert = alert;
}

// Title and alert button follow the selected room (red alert needs colour bulbs).
static void update_room_view() {
  HueRoom room;
  bool known = get_room(sel_room, room);
  lv_label_set_text(title, known ? room.name : (model_gen ? "NO ROOM" : "ACCESSING"));
  bool can_alert = alert_active || (known && room.n_color > 0);
  if (can_alert) lv_obj_clear_flag(alert_btn, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(alert_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(alert_btn, lv_color_hex(alert_active ? C_LILAC : C_RED), 0);
  lv_label_set_text(alert_text, alert_active ? "RESET" : "RED ALERT");
  shown_model_gen = model_gen;
  strcpy(shown_room, sel_room);
}

// Runs in the LVGL task every 20 ms.
static void ui_timer_cb(lv_timer_t *) {
  bool alert = alert_active || preview_alert;
  if (alert != themed_alert) { apply_theme(alert); update_room_view(); }

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

  if (shown_model_gen != model_gen || strcmp(shown_room, sel_room)) update_room_view();

  if (level_dirty) {
    level_dirty = false;
    int bri = level;
    int lit = bri >= 0 && lights_on ? (bri * N_LEVEL + 253) / 254 : 0;
    for (int i = 0; i < N_LEVEL; i++) paint(level_blocks[i], i < lit ? C_GOLD : C_DIM);
  }

  menu_tick();
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

static void battery_timer_cb(lv_timer_t *) {
  static float smooth = 0;
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(BATTERY_PIN);
  float v = mv / 16.0f * 2 / 1000.0f;
  smooth = smooth == 0 ? v : smooth * 0.8f + v * 0.2f;  // steady the reading

  int pct = battery_percent(smooth);
  bool charging = smooth > 4.25f;  // above a full cell = charger is driving it
  if (charging) lv_label_set_text(battery_label, "PWR EXT");
  else          lv_label_set_text_fmt(battery_label, "PWR %d%%", pct);
  // The status block in the bottom bar: blue normally, gold on charge, red when low.
  uint32_t c = charging ? C_GOLD : pct <= 15 ? C_RED : C_SKY;
  for (int i = 0; i < n_panels; i++)
    if (panels[i].obj == battery_block) panels[i].normal = c;
  if (!themed_alert) paint(battery_block, c);
}

static void on_room_btn(lv_event_t *) { open_room_picker(); }
static void on_color_btn(lv_event_t *) { open_room_control(); }

extern "C" void ui_build(void) {
  main_scr = lv_scr_act();
  lv_obj_t *scr = main_scr;
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  static const char *codes[4] = {"93", "43", "07", "12"};
  battery_block = lcars_ring(scr, true, codes);

  // Brightness: curved blocks across the top, lit by the room's brightness
  for (int i = 0; i < N_LEVEL; i++) {
    int a0 = 216 + i * 9;
    level_blocks[i] = arc(scr, 270, 12, a0, a0 + 7, C_DIM, false);
  }

  title = text(scr, &lcars_22, C_ORANGE, "ACCESSING");
  lv_obj_set_size(title, 200, 30);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 66);

  build_alert_button(scr);

  status = text(scr, &lcars_34, C_GREY, "");
  lv_obj_set_width(status, 240);
  lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(status, LV_LABEL_LONG_CLIP);
  lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 158);
  set_status(C_GREY, "CONNECTING");

  // Menu buttons
  lv_obj_t *rb = pill(scr, 88, 214, 88, 34, C_SKY, "ROOM", on_room_btn, nullptr);
  lv_obj_t *cb = pill(scr, 184, 214, 88, 34, C_GOLD, "COLOR", on_color_btn, nullptr);
  theme_register(rb, C_SKY, C_ALERT3);
  theme_register(cb, C_GOLD, C_ALERT1);

  battery_label = text(scr, &lcars_16, C_PEACH, "PWR");
  lv_obj_align(battery_label, LV_ALIGN_TOP_MID, 0, 260);

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
