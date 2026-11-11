// LCARS building blocks. Everything here runs in the LVGL task (or with the LVGL lock held).

static Panel panels[24];  // themed panels on the main screen (red alert recolours them)
static int n_panels = 0;

static void theme_register(lv_obj_t *o, uint32_t color, uint32_t alert_color) {
  if (n_panels < 24) panels[n_panels++] = {o, color, alert_color};
}

static void plain(lv_obj_t *o) {
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_shadow_width(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *block(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t color) {
  lv_obj_t *o = lv_obj_create(parent);
  plain(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *t) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, t);
  return l;
}

// A transparent full-screen layer, for showing / hiding a group of objects together.
static lv_obj_t *layer(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  plain(o);
  lv_obj_set_size(o, 360, 360);
  lv_obj_set_pos(o, 0, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  return o;
}

// A ring segment centred on the screen. Angles in degrees, 0 = 3 o'clock, clockwise.
static lv_obj_t *arc(lv_obj_t *parent, int diameter, int width, int a0, int a1, uint32_t color,
                     bool rounded) {
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_set_size(a, diameter, diameter);
  lv_obj_align(a, LV_ALIGN_CENTER, 0, 0);
  lv_arc_set_bg_angles(a, a0 % 360, a1 % 360);
  lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
  lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, rounded, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_pad_all(a, 0, 0);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  return a;
}

// Colours a panel, whether it's a block or a ring segment.
static void paint(lv_obj_t *o, uint32_t color) {
  if (lv_obj_check_type(o, &lv_arc_class)) lv_obj_set_style_arc_color(o, lv_color_hex(color), LV_PART_MAIN);
  else lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
}

// Black lettering at angle `deg`, radius `r` from the centre.
static void ring_code(lv_obj_t *parent, int deg, int r, const char *t) {
  lv_obj_t *l = text(parent, &lcars_16, 0x000000, t);
  float rad = deg * PI / 180;
  lv_obj_align(l, LV_ALIGN_CENTER, lroundf(r * cosf(rad)), lroundf(r * sinf(rad)));
}

// The LCARS frame bent into a ring, open on the right: thick down the left (the classic LCARS
// column), stepping down to thinner bars across the top and bottom. Returns the small block in
// the bottom bar (the main screen uses it for battery status). Themed when `themed`.
static const int RING_D = 352, RING_THICK = 34, RING_THIN = 20;
static lv_obj_t *lcars_ring(lv_obj_t *parent, bool themed, const char *codes[4]) {
  struct Seg { int w, a0, a1; uint32_t c, alert; bool round; };
  static const Seg segs[] = {
    {RING_THICK, 216, 246, C_ORANGE, C_ALERT1, false},  // top elbow: thick part...
    {RING_THIN,  246, 302, C_ORANGE, C_ALERT1, false},  // ...running into the top bar
    {RING_THICK, 182, 214, C_SKY,    C_ALERT3, false},
    {RING_THICK, 148, 180, C_LILAC,  C_ALERT1, false},
    {RING_THICK, 116, 146, C_PEACH,  C_ALERT2, false},  // bottom elbow: thick part...
    {RING_THIN,   72, 116, C_PEACH,  C_ALERT2, false},  // ...running into the bottom bar
    {RING_THIN,  304, 324, C_PEACH,  C_ALERT3, false},
    {RING_THIN,  326, 346, C_LILAC,  C_ALERT2, true},
    {RING_THIN,   52,  70, C_SKY,    C_ALERT3, false},  // status block
    {RING_THIN,   32,  50, C_PURPLE, C_ALERT1, true},
  };
  lv_obj_t *status_block = nullptr;
  for (const Seg &s : segs) {
    lv_obj_t *a = arc(parent, RING_D, s.w, s.a0, s.a1, s.c, s.round);
    if (themed) theme_register(a, s.c, s.alert);
    if (s.a0 == 52) status_block = a;
  }
  const int r = RING_D / 2 - RING_THICK / 2;
  const int angles[4] = {228, 198, 164, 131};
  for (int i = 0; i < 4; i++) ring_code(parent, angles[i], r, codes[i]);
  return status_block;
}

// An LCARS pill button with black lettering. `cb` gets LV_EVENT_CLICKED with `user`.
static lv_obj_t *pill(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, const char *label,
                      lv_event_cb_t cb, void *user) {
  lv_obj_t *b = block(parent, x, y, w, h, h / 2, color);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(b, 4);
  lv_obj_set_style_bg_color(b, lv_color_white(), LV_STATE_PRESSED);
  lv_obj_t *l = text(b, h >= 34 ? &lcars_22 : &lcars_16, 0x000000, label);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
  return b;
}

static void pill_label(lv_obj_t *pill, const char *t) { lv_label_set_text(lv_obj_get_child(pill, 0), t); }

// ---------- Colour helpers ----------
// Hue (0..65535) + saturation (0..254) + value (0..1) -> 0xRRGGBB
static uint32_t hsv_rgb(int hue, int sat, float v) {
  float h = hue / 65536.0f * 6, s = sat / 254.0f;
  int i = (int)h % 6;
  float f = h - (int)h, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  float r, g, b;
  switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return ((uint32_t)(r * 255) << 16) | ((uint32_t)(g * 255) << 8) | (uint32_t)(b * 255);
}

// Colour temperature in mireds (153 cool .. 500 warm) -> approximate 0xRRGGBB, scaled by v.
static uint32_t ct_rgb(int ct, float v) {
  float t = constrain((ct - 153) / 347.0f, 0.0f, 1.0f);  // 0 = cool, 1 = warm
  float r = 225 + 30 * t, g = 235 - 85 * t, b = 255 - 175 * t;
  return ((uint32_t)(r * v) << 16) | ((uint32_t)(g * v) << 8) | (uint32_t)(b * v);
}
