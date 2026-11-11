// Menus:
//   ROOM  (main)  -> room picker list  -> picks the main screen's room
//   COLOR (main)  -> colour control for the room -> LIGHTS -> light list -> colour control for one light
// The colour control has three tabs: COLOR (swatches), HUE (outer ring is a colour wheel) and
// INTENSITY (outer ring is a brightness gauge). The knob scrolls lists and adjusts hue / intensity.
//
// UI code runs in the LVGL task. Bridge traffic for the menus runs in menu_task.

// ---------- What the colour control is pointed at, and what we want it to look like ----------
static Target target;
static volatile bool t_on = true, t_use_ct = false;
static volatile int t_bri = 254, t_hue = 0, t_sat = 254, t_ct = 366;
enum : uint32_t { F_ON = 1, F_BRI = 2, F_HS = 4, F_CT = 8 };
static volatile uint32_t t_dirty = 0;     // fields to send
static volatile bool want_state = false;  // fetch the target's current state
static volatile bool want_model = false;  // re-read rooms + lights
static volatile bool state_ready = false, model_ready = false;
static TaskHandle_t menu_task_h = nullptr;

// ---------- Screens ----------
enum ListKind : uint8_t { LIST_ROOMS, LIST_LIGHTS };
static lv_obj_t *list_scr, *list_title, *list_box, *list_note;
static ListKind list_kind;
static char list_room[6];                 // LIST_LIGHTS: whose lights
static char list_ids[MAX_MEMBERS + 1][6]; // id behind each list row

static lv_obj_t *ctrl_scr, *ctrl_title, *ctrl_frame, *ctrl_wheel, *wheel_marker, *ctrl_gauge;
static lv_obj_t *swatch_layer, *preview, *preview_label, *power_btn, *lights_btn;
static lv_obj_t *tabs[3];
static lv_obj_t *swatches[12];
static int tab = 0;                       // 0 COLOR, 1 HUE, 2 INTENSITY
static bool ctrl_from_list = false;       // BACK goes to the light list (else the main screen)

static const uint32_t ROW_COLORS[] = {C_ORANGE, C_PEACH, C_LILAC, C_SKY, C_GOLD, C_PURPLE};

struct Preset { uint32_t rgb; bool ct; uint16_t hue; uint16_t ct_val; };
static const Preset PRESETS[12] = {
  {0xFF2020, false, 0, 0},     {0xFF8C1A, false, 5461, 0},  {0xFFE033, false, 10923, 0}, {0x33DD55, false, 21845, 0},
  {0x33DDEE, false, 32768, 0}, {0x3355FF, false, 43690, 0}, {0x9944FF, false, 50972, 0}, {0xFF55BB, false, 58254, 0},
  {0xFFA050, true, 0, 454},    {0xFFC890, true, 0, 370},    {0xFFE8D0, true, 0, 286},    {0xE0ECFF, true, 0, 175},
};

static void go(lv_obj_t *scr, UiMode mode) {
  ui_mode = mode;
  __atomic_store_n(&menu_clicks, 0, __ATOMIC_RELAXED);
  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, 120, 0, false);
}

static void kick_menu_task() { if (menu_task_h) xTaskNotifyGive(menu_task_h); }

// ---------- List screen ----------
static void fill_list();
static void open_control_light(const char *id);
static void open_control_room(const char *id);

static void on_list_row(lv_event_t *e) {
  int i = (intptr_t)lv_event_get_user_data(e);
  if (list_kind == LIST_ROOMS) {
    select_room(list_ids[i]);
    go(main_scr, MODE_MAIN);
  } else {
    open_control_light(list_ids[i]);
  }
}

static void on_list_back(lv_event_t *) {
  if (list_kind == LIST_LIGHTS) open_control_room(list_room);  // back to the room's colour control
  else go(main_scr, MODE_MAIN);
}

static void add_row(int i, const char *name, const char *code, bool selected) {
  lv_obj_t *row = lv_obj_create(list_box);
  plain(row);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(row, lv_pct(100), 36);
  lv_obj_set_style_radius(row, 18, 0);
  lv_obj_set_style_bg_color(row, lv_color_hex(selected ? C_RED : ROW_COLORS[i % 6]), 0);
  lv_obj_set_style_bg_color(row, lv_color_white(), LV_STATE_PRESSED);
  lv_obj_t *l = text(row, &lcars_22, 0x000000, name);
  lv_obj_set_size(l, 160, 30);  // fixed height so long names end in "..." instead of wrapping
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 16, 0);
  lv_obj_align(text(row, &lcars_16, 0x000000, code), LV_ALIGN_RIGHT_MID, -14, 0);
  lv_obj_add_event_cb(row, on_list_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
}

static void fill_list() {
  lv_obj_clean(list_box);
  int n = 0;
  xSemaphoreTake(model_mux, portMAX_DELAY);
  if (list_kind == LIST_ROOMS) {
    lv_label_set_text(list_title, "SELECT ROOM");
    for (int i = 0; i < n_rooms; i++) {
      strcpy(list_ids[n], rooms[i].id);
      char code[12];
      snprintf(code, sizeof code, "%02d", rooms[i].n);
      add_row(n++, rooms[i].name, code, !strcmp(rooms[i].id, sel_room));
    }
  } else {
    HueRoom *room = nullptr;
    for (int i = 0; i < n_rooms; i++) if (!strcmp(rooms[i].id, list_room)) room = &rooms[i];
    lv_label_set_text(list_title, room ? room->name : "");
    for (int m = 0; room && m < room->n; m++) {
      for (int i = 0; i < n_lights; i++) {
        if (strcmp(lights[i].id, room->member[m])) continue;
        strcpy(list_ids[n], lights[i].id);
        add_row(n++, lights[i].name, lights[i].color ? "CLR" : lights[i].ct ? "WHT" : "DIM", false);
      }
    }
  }
  xSemaphoreGive(model_mux);
  if (n) lv_obj_add_flag(list_note, LV_OBJ_FLAG_HIDDEN);
  else {
    lv_label_set_text(list_note, model_gen ? "NO ENTRIES" : "ACCESSING BRIDGE");
    lv_obj_clear_flag(list_note, LV_OBJ_FLAG_HIDDEN);
  }
}

static void build_list_screen() {
  list_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(list_scr, lv_color_black(), 0);
  lv_obj_clear_flag(list_scr, LV_OBJ_FLAG_SCROLLABLE);
  static const char *codes[4] = {"21", "04", "88", "16"};
  lcars_ring(list_scr, false, codes);

  list_title = text(list_scr, &lcars_22, C_ORANGE, "");
  lv_obj_set_size(list_title, 190, 30);
  lv_obj_set_style_text_align(list_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(list_title, LV_LABEL_LONG_DOT);
  lv_obj_align(list_title, LV_ALIGN_TOP_MID, 0, 46);

  list_box = lv_obj_create(list_scr);
  plain(list_box);
  lv_obj_add_flag(list_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_opa(list_box, LV_OPA_TRANSP, 0);
  lv_obj_set_size(list_box, 228, 172);
  lv_obj_set_pos(list_box, 66, 80);
  lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(list_box, 6, 0);

  list_note = text(list_scr, &lcars_22, C_PEACH, "");
  lv_obj_align(list_note, LV_ALIGN_CENTER, 0, -10);

  pill(list_scr, 130, 262, 100, 34, C_LILAC, "BACK", on_list_back, nullptr);
}

// ---------- Colour control ----------
static void refresh_ctrl() {
  bool on = t_on;
  float v = on ? 0.35f + 0.65f * t_bri / 254.0f : 0;
  uint32_t rgb = !on ? C_DIM : t_use_ct ? ct_rgb(t_ct, v) : hsv_rgb(t_hue, t_sat, v);
  lv_obj_set_style_bg_color(preview, lv_color_hex(rgb), 0);
  if (!on) lv_label_set_text(preview_label, "OFF");
  else if (tab == 1 && !t_use_ct) lv_label_set_text_fmt(preview_label, "H %d", (int)(t_hue * 360L / 65536));
  else lv_label_set_text_fmt(preview_label, "%d%%", (t_bri * 100 + 127) / 254);
  lv_obj_set_style_text_color(preview_label, lv_color_hex(on && t_bri > 60 ? 0x000000 : C_PEACH), 0);

  int deg = 270 + (int)(t_hue * 360L / 65536);
  lv_arc_set_bg_angles(wheel_marker, (deg - 3 + 360) % 360, (deg + 3) % 360);
  lv_arc_set_value(ctrl_gauge, on ? t_bri : 0);

  pill_label(power_btn, on ? "ON" : "OFF");
  lv_obj_set_style_bg_color(power_btn, lv_color_hex(on ? C_GOLD : C_DIM), 0);
  lv_obj_set_style_text_color(lv_obj_get_child(power_btn, 0), lv_color_hex(on ? 0x000000 : C_PEACH), 0);
}

static void show_tab(int t) {
  tab = t;
  for (int i = 0; i < 3; i++) paint(tabs[i], i == t ? C_ORANGE : C_PURPLE);
  auto vis = [](lv_obj_t *o, bool show) { if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); };
  vis(ctrl_frame, t == 0);
  vis(swatch_layer, t == 0);
  vis(ctrl_wheel, t == 1);
  vis(ctrl_gauge, t == 2);
  vis(preview, t != 0);
  refresh_ctrl();
}

static void mark(uint32_t fields) {
  t_on = true;
  __atomic_fetch_or(&t_dirty, fields | F_ON, __ATOMIC_RELAXED);
  kick_menu_task();
  refresh_ctrl();
}

static void on_tab(lv_event_t *e) {
  int t = (intptr_t)lv_event_get_user_data(e);
  if (t == 1 && !target.color) return;
  show_tab(t);
}

static void on_swatch(lv_event_t *e) {
  const Preset &p = PRESETS[(intptr_t)lv_event_get_user_data(e)];
  if (p.ct) { t_use_ct = true; t_ct = p.ct_val; mark(F_CT); }
  else { t_use_ct = false; t_hue = p.hue; t_sat = 254; mark(F_HS); }
}

static void on_power(lv_event_t *) {
  t_on = !t_on;
  __atomic_fetch_or(&t_dirty, F_ON, __ATOMIC_RELAXED);
  kick_menu_task();
  refresh_ctrl();
}

static void on_ctrl_back(lv_event_t *) {
  if (ctrl_from_list) {  // back to the room's light list
    list_kind = LIST_LIGHTS;
    fill_list();
    go(list_scr, MODE_LIST);
  } else {
    go(main_scr, MODE_MAIN);
  }
}

static void on_lights_btn(lv_event_t *) {
  list_kind = LIST_LIGHTS;
  strcpy(list_room, target.id);
  fill_list();
  go(list_scr, MODE_LIST);
}

static void point_control(const Target &t, bool from_list) {
  target = t;
  ctrl_from_list = from_list;
  lv_label_set_text(ctrl_title, t.name);
  // Swatches and the hue tab only for bulbs that can do them.
  for (int i = 0; i < 12; i++) {
    bool ok = PRESETS[i].ct ? t.ct : t.color;
    lv_obj_set_style_opa(swatches[i], ok ? LV_OPA_COVER : LV_OPA_20, 0);
    if (ok) lv_obj_add_flag(swatches[i], LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_clear_flag(swatches[i], LV_OBJ_FLAG_CLICKABLE);
  }
  lv_obj_set_style_opa(tabs[1], t.color ? LV_OPA_COVER : LV_OPA_20, 0);
  if (t.group) lv_obj_clear_flag(lights_btn, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(lights_btn, LV_OBJ_FLAG_HIDDEN);
  show_tab(0);
  lv_label_set_text(preview_label, "...");
  want_state = true;
  kick_menu_task();
  go(ctrl_scr, MODE_CONTROL);
}

static void open_control_light(const char *id) {
  HueLight l;
  if (!get_light(id, l)) return;
  Target t = {false, "", "", l.color, l.ct};
  strcpy(t.id, l.id);
  strcpy(t.name, l.name);
  point_control(t, true);
}

static void open_control_room(const char *id) {
  HueRoom room;
  if (!get_room(id, room)) return;
  Target t = {true, "", "", true, true};
  strcpy(t.id, room.id);
  strcpy(t.name, room.name);
  point_control(t, false);
}

static void open_room_control() { open_control_room(sel_room); }

static void open_room_picker() {
  list_kind = LIST_ROOMS;
  fill_list();
  want_model = true;  // refresh from the bridge; the list re-fills when it arrives
  kick_menu_task();
  go(list_scr, MODE_LIST);
}

static void build_control_screen() {
  ctrl_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(ctrl_scr, lv_color_black(), 0);
  lv_obj_clear_flag(ctrl_scr, LV_OBJ_FLAG_SCROLLABLE);

  // Outer ring: LCARS frame (COLOR tab), colour wheel (HUE) or brightness gauge (INTENSITY).
  ctrl_frame = layer(ctrl_scr);
  static const char *codes[4] = {"35", "62", "09", "47"};
  lcars_ring(ctrl_frame, false, codes);

  ctrl_wheel = layer(ctrl_scr);
  for (int i = 0; i < 36; i++)  // hue 0 at the top, increasing clockwise
    arc(ctrl_wheel, RING_D, RING_THIN, 270 + i * 10, 270 + i * 10 + 9, hsv_rgb(i * 65536 / 36 + 910, 254, 1), false);
  wheel_marker = arc(ctrl_wheel, RING_D, 28, 267, 273, 0xFFFFFF, true);

  ctrl_gauge = lv_arc_create(ctrl_scr);
  lv_obj_set_size(ctrl_gauge, RING_D, RING_D);
  lv_obj_center(ctrl_gauge);
  lv_arc_set_bg_angles(ctrl_gauge, 135, 45);
  lv_arc_set_range(ctrl_gauge, 0, 254);
  lv_obj_remove_style(ctrl_gauge, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(ctrl_gauge, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(ctrl_gauge, RING_THIN, LV_PART_MAIN);
  lv_obj_set_style_arc_width(ctrl_gauge, RING_THIN, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(ctrl_gauge, false, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(ctrl_gauge, false, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ctrl_gauge, lv_color_hex(C_DIM), LV_PART_MAIN);
  lv_obj_set_style_arc_color(ctrl_gauge, lv_color_hex(C_GOLD), LV_PART_INDICATOR);
  lv_obj_set_style_anim_time(ctrl_gauge, 0, 0);

  ctrl_title = text(ctrl_scr, &lcars_22, C_ORANGE, "");
  lv_obj_set_size(ctrl_title, 170, 30);
  lv_obj_set_style_text_align(ctrl_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(ctrl_title, LV_LABEL_LONG_DOT);
  lv_obj_align(ctrl_title, LV_ALIGN_TOP_MID, 0, 46);

  static const char *tab_names[3] = {"COLOR", "HUE", "INTENSITY"};
  for (int i = 0; i < 3; i++)
    tabs[i] = pill(ctrl_scr, 66 + i * 78, 80, 72, 28, C_PURPLE, tab_names[i], on_tab, (void *)(intptr_t)i);

  // COLOR tab: 8 colours and 4 whites
  swatch_layer = layer(ctrl_scr);
  for (int i = 0; i < 12; i++) {
    int x = 96 + (i % 4) * 44, y = 118 + (i / 4) * 44;
    lv_obj_t *s = block(swatch_layer, x, y, 36, 36, LV_RADIUS_CIRCLE, PRESETS[i].rgb);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s, 4);
    lv_obj_set_style_border_color(s, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s, 3, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s, on_swatch, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    swatches[i] = s;
  }

  // HUE / INTENSITY tabs: a preview of the current colour with its value
  preview = block(ctrl_scr, 120, 118, 120, 120, LV_RADIUS_CIRCLE, C_DIM);
  preview_label = text(preview, &lcars_34, 0x000000, "");
  lv_obj_center(preview_label);

  pill(ctrl_scr, 70, 256, 70, 34, C_LILAC, "BACK", on_ctrl_back, nullptr);
  power_btn = pill(ctrl_scr, 145, 256, 70, 34, C_GOLD, "ON", on_power, nullptr);
  lights_btn = pill(ctrl_scr, 220, 256, 70, 34, C_SKY, "LIGHTS", on_lights_btn, nullptr);
}

// ---------- Called from the LVGL task every 20 ms ----------
static void menu_tick() {
  if (model_ready) {
    model_ready = false;
    if (lv_scr_act() == list_scr) fill_list();
  }
  if (state_ready) {
    state_ready = false;
    refresh_ctrl();
  }
  int clicks = __atomic_exchange_n(&menu_clicks, 0, __ATOMIC_RELAXED);
  if (!clicks) return;
  if (ui_mode == MODE_LIST) {
    lv_obj_scroll_by_bounded(list_box, 0, -clicks * 42, LV_ANIM_ON);
  } else if (ui_mode == MODE_CONTROL) {
    if (tab == 1 && target.color) {
      t_use_ct = false;
      t_sat = 254;
      t_hue = (t_hue + clicks * HUE_STEP) & 0xFFFF;
      mark(F_HS);
    } else {
      t_bri = constrain(t_bri + clicks * BRI_STEP, 1, 254);
      mark(F_BRI);
    }
  }
}

// ---------- Bridge side ----------
static void load_target_state() {
  String body;
  String path = String(target.group ? "/groups/" : "/lights/") + target.id;
  if (!hue_get(path, body)) return;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  JsonObject st = target.group ? doc["action"] : doc["state"];
  t_on = target.group ? (doc["state"]["any_on"] | false) : (st["on"] | false);
  t_bri = st["bri"] | 254;
  t_hue = st["hue"] | 0;
  t_sat = st["sat"] | 254;
  t_ct = st["ct"] | 366;
  const char *mode = st["colormode"] | (target.color ? "hs" : "ct");
  t_use_ct = !strcmp(mode, "ct") || !target.color;
  state_ready = true;
}

static void send_target(uint32_t f) {
  JsonDocument d;
  if (f & F_ON) d["on"] = (bool)t_on;
  if (t_on) {
    if (f & F_BRI) d["bri"] = (int)t_bri;
    if (f & F_HS) { d["hue"] = (int)t_hue; d["sat"] = (int)t_sat; }
    if (f & F_CT) d["ct"] = (int)t_ct;
  }
  d["transitiontime"] = 2;
  char body[128];
  serializeJson(d, body, sizeof body);
  String path = target.group ? String("/groups/") + target.id + "/action" : String("/lights/") + target.id + "/state";
  int code = hue_put(path, body);
  Serial.printf("[%lu] %s %s -> %d\n", millis(), target.name, body, code);
}

static void menu_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    if (want_model) { want_model = false; if (load_model()) model_ready = true; }
    if (want_state) { want_state = false; __atomic_store_n(&t_dirty, 0, __ATOMIC_RELAXED); load_target_state(); }
    uint32_t f = __atomic_exchange_n(&t_dirty, 0, __ATOMIC_RELAXED);
    if (f) {
      send_target(f);
      // Hue rate limits: ~10 light commands/s, ~1 group command/s. Changes made meanwhile batch up.
      vTaskDelay(pdMS_TO_TICKS(target.group ? 400 : 120));
      if (t_dirty) xTaskNotifyGive(xTaskGetCurrentTaskHandle());
    }
  }
}

static void debug_nav(char c) {
  switch (c) {
    case 'R': open_room_picker(); break;
    case 'C': open_room_control(); break;
    case 'L': list_kind = LIST_LIGHTS; strcpy(list_room, sel_room); fill_list(); go(list_scr, MODE_LIST); break;
    case '0': case '1': case '2': show_tab(c - '0'); break;
    case 'M': go(main_scr, MODE_MAIN); break;
  }
}

static void menu_init() {
  if (example_lvgl_lock(-1)) {
    build_list_screen();
    build_control_screen();
    example_lvgl_unlock();
  }
  xTaskCreate(menu_task, "menu", 8192, nullptr, 3, &menu_task_h);
}
