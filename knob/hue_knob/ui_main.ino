// Main screen: room title, brightness meter, ALERT/RESET slider, ROOM and COLOR buttons,
// status, battery. Plus the red-alert theme and serial screenshots.

static lv_obj_t *main_scr, *slider, *slider_layer, *status, *title, *battery_label, *battery_block;
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

static void on_slider(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  int v = lv_slider_get_value(slider);
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (v >= SLIDE_TRIGGER) request(CMD_FLASH);
    else if (v <= -SLIDE_TRIGGER) request(CMD_RESET);
    lv_slider_set_value(slider, 0, LV_ANIM_ON);  // spring back to the middle
  }
}

static void apply_theme(bool alert) {
  for (int i = 0; i < n_panels; i++)
    paint(panels[i].obj, alert ? panels[i].alert : panels[i].normal);
  lv_obj_set_style_text_color(title, lv_color_hex(alert ? C_ALERT2 : C_ORANGE), 0);
  themed_alert = alert;
}

// Title and slider follow the selected room (the slider needs colour bulbs to flash).
static void update_room_view() {
  HueRoom room;
  bool known = get_room(sel_room, room);
  lv_label_set_text(title, known ? room.name : (model_gen ? "NO ROOM" : "ACCESSING"));
  bool can_alert = alert_active || (known && room.n_color > 0);
  if (can_alert) lv_obj_clear_flag(slider_layer, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(slider_layer, LV_OBJ_FLAG_HIDDEN);
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

  // Slider: RESET pill on the left, ALERT pill on the right, orange handle in the middle.
  slider_layer = layer(scr);
  const int SW = 236, SX = 180 - SW / 2, SY = 106, SH = 48, PW = 62;
  lv_obj_t *rp = block(slider_layer, SX, SY, PW, SH, SH / 2, C_LILAC);
  lv_obj_t *ap = block(slider_layer, SX + SW - PW, SY, PW, SH, SH / 2, C_RED);
  lv_obj_align(text(rp, &lcars_22, 0x000000, "RESET"), LV_ALIGN_CENTER, 0, 0);
  lv_obj_align(text(ap, &lcars_22, 0x000000, "ALERT"), LV_ALIGN_CENTER, 0, 0);
  lv_obj_t *rail = block(slider_layer, SX + PW, SY + SH / 2 - 2, SW - 2 * PW, 4, 0, C_PEACH);
  theme_register(rail, C_PEACH, C_ALERT2);

  slider = lv_slider_create(slider_layer);
  lv_obj_set_pos(slider, SX + SH / 2, SY);
  lv_obj_set_size(slider, SW - SH, SH);  // handle centre travels the full track
  lv_slider_set_mode(slider, LV_SLIDER_MODE_SYMMETRICAL);
  lv_slider_set_range(slider, -100, 100);
  lv_slider_set_value(slider, 0, LV_ANIM_OFF);
  lv_obj_add_flag(slider, LV_OBJ_FLAG_ADV_HITTEST);  // only the handle can be grabbed
  lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(slider, lv_color_hex(C_ORANGE), LV_PART_KNOB);
  lv_obj_set_style_radius(slider, 8, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
  lv_obj_set_style_border_color(slider, lv_color_black(), LV_PART_KNOB);
  lv_obj_set_style_border_width(slider, 4, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(slider, 0, LV_PART_KNOB);
  lv_obj_set_style_anim_time(slider, 250, 0);
  lv_obj_set_ext_click_area(slider, 12);
  lv_obj_add_event_cb(slider, on_slider, LV_EVENT_ALL, nullptr);

  status = text(scr, &lcars_34, C_GREY, "");
  lv_obj_set_width(status, 240);
  lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(status, LV_LABEL_LONG_CLIP);
  lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 162);
  set_status(C_GREY, "CONNECTING");

  // Menu buttons
  lv_obj_t *rb = pill(scr, 82, 218, 94, 34, C_SKY, "ROOM", on_room_btn, nullptr);
  lv_obj_t *cb = pill(scr, 184, 218, 94, 34, C_GOLD, "COLOR", on_color_btn, nullptr);
  theme_register(rb, C_SKY, C_ALERT3);
  theme_register(cb, C_GOLD, C_ALERT1);

  battery_label = text(scr, &lcars_16, C_PEACH, "PWR");
  lv_obj_align(battery_label, LV_ALIGN_TOP_MID, 0, 266);

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
