// Hue bridge access, the room / light model, and the main-screen room dimmer.

static String hue_url(const String &path) {
  return String("http://") + HUE_BRIDGE + "/api/" + HUE_USER + path;
}

static bool hue_get(const String &path, String &out) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setTimeout(4000);
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

// Bridge names -> LCARS labels: upper case, plain ASCII (the font has nothing else).
static void lcars_name(char *dst, const char *src, size_t n) {
  size_t o = 0;
  while (*src && o + 1 < n) {
    uint8_t c = *src;
    if (c < 0x80) { dst[o++] = toupper(c); src++; continue; }
    if ((uint8_t)src[0] == 0xE2 && (uint8_t)src[1] == 0x80 && ((uint8_t)src[2] == 0x98 || (uint8_t)src[2] == 0x99))
      dst[o++] = '\'';  // curly quotes
    src++;
    while (((uint8_t)*src & 0xC0) == 0x80) src++;  // skip the rest of the UTF-8 sequence
  }
  dst[o] = 0;
}

// Reads every light and every room / zone from the bridge.
static bool load_model() {
  static HueLight L[MAX_LIGHTS];
  static HueRoom R[MAX_ROOMS];
  int nl = 0, nr = 1;
  String body;
  {
    if (!hue_get("/lights", body)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    for (JsonPair kv : doc.as<JsonObject>()) {
      if (nl >= MAX_LIGHTS) break;
      HueLight &l = L[nl++];
      strlcpy(l.id, kv.key().c_str(), sizeof l.id);
      lcars_name(l.name, kv.value()["name"] | "?", sizeof l.name);
      const char *type = kv.value()["type"] | "";
      l.color = strstr(type, "olor light") != nullptr;          // "Extended color light", "Color light"
      l.ct = l.color || strstr(type, "temperature") != nullptr;  // "Color temperature light"
    }
  }
  // Room 0: every light on the bridge.
  HueRoom &all = R[0];
  strcpy(all.id, "0");
  strcpy(all.name, "ALL LIGHTS");
  all.n = all.n_color = 0;
  for (int i = 0; i < nl && all.n < MAX_MEMBERS; i++) {
    strcpy(all.member[all.n++], L[i].id);
    if (L[i].color) all.n_color++;
  }
  {
    if (!hue_get("/groups", body)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    body = String();
    for (JsonPair kv : doc.as<JsonObject>()) {
      const char *type = kv.value()["type"] | "";
      if (strcmp(type, "Room") && strcmp(type, "Zone")) continue;
      if (nr >= MAX_ROOMS) break;
      HueRoom &r = R[nr];
      strlcpy(r.id, kv.key().c_str(), sizeof r.id);
      lcars_name(r.name, kv.value()["name"] | "?", sizeof r.name);
      r.n = r.n_color = 0;
      for (JsonVariant v : kv.value()["lights"].as<JsonArray>()) {
        if (r.n >= MAX_MEMBERS) break;
        const char *lid = v | "";
        strlcpy(r.member[r.n++], lid, 6);
        for (int i = 0; i < nl; i++)
          if (!strcmp(L[i].id, lid) && L[i].color) r.n_color++;
      }
      if (r.n) nr++;  // skip empty zones
    }
  }
  // Rooms in name order (after ALL LIGHTS).
  std::sort(R + 1, R + nr, [](const HueRoom &a, const HueRoom &b) { return strcmp(a.name, b.name) < 0; });

  xSemaphoreTake(model_mux, portMAX_DELAY);
  memcpy(lights, L, sizeof(HueLight) * nl);
  memcpy(rooms, R, sizeof(HueRoom) * nr);
  n_lights = nl;
  n_rooms = nr;
  xSemaphoreGive(model_mux);
  model_gen++;
  Serial.printf("bridge: %d lights, %d rooms/zones\n", nl, nr - 1);
  return true;
}

static bool get_room(const char *id, HueRoom &out) {
  bool found = false;
  xSemaphoreTake(model_mux, portMAX_DELAY);
  for (int i = 0; i < n_rooms && !found; i++)
    if (!strcmp(rooms[i].id, id)) { out = rooms[i]; found = true; }
  xSemaphoreGive(model_mux);
  return found;
}

static bool get_light(const char *id, HueLight &out) {
  bool found = false;
  xSemaphoreTake(model_mux, portMAX_DELAY);
  for (int i = 0; i < n_lights && !found; i++)
    if (!strcmp(lights[i].id, id)) { out = lights[i]; found = true; }
  xSemaphoreGive(model_mux);
  return found;
}

static bool is_alert_light(const char *id) {
  if (!alert_active) return false;
  for (int i = 0; i < n_alert; i++)
    if (!strcmp(alert_ids[i], id)) return true;
  return false;
}

// Average brightness of the room's lights that are on, leaving out the `skip` bulbs.
// Returns -1 if none are on, -2 if the bridge couldn't be read.
static int room_average_bri(const HueRoom &room, char (*skip)[6], int n_skip) {
  String body;
  if (!hue_get("/lights", body)) return -2;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return -2;
  int sum = 0, count = 0;
  for (int i = 0; i < room.n; i++) {
    bool skipped = false;
    for (int k = 0; k < n_skip; k++) skipped |= !strcmp(skip[k], room.member[i]);
    if (skipped) continue;
    JsonObject st = doc[room.member[i]]["state"];
    if (!(st["on"] | false) || st["bri"].isNull()) continue;
    sum += st["bri"].as<int>();
    count++;
  }
  return count ? (sum + count / 2) / count : -1;
}

// Switches the main screen to another room / zone, and remembers it across restarts.
static void select_room(const char *id) {
  strlcpy(sel_room, id, sizeof sel_room);
  prefs.putString("room", sel_room);
  level = -1;
  level_dirty = true;
  __atomic_store_n(&knob_delta, 0, __ATOMIC_RELAXED);
  if (dimmer) xTaskNotifyGive(dimmer);  // fetch the new room's brightness
}

// ---------- Main-screen dimmer ----------
static bool knob_recently_used(uint32_t ms) { return millis() - last_knob_ms < ms; }

// Reads the real brightness (picks up changes from the Hue app, corrects drift).
static void refresh_level() {
  if (model_gen == 0 && !load_model()) return;
  HueRoom room;
  if (!get_room(sel_room, room)) {
    // The saved room is gone from the bridge: fall back to the default, then ALL LIGHTS.
    select_room(get_room(DEFAULT_ROOM, room) ? DEFAULT_ROOM : "0");
    if (!get_room(sel_room, room)) return;
  }
  int bri = room_average_bri(room, alert_ids, alert_active ? n_alert : 0);
  if (bri == -2 || knob_recently_used(1500)) return;  // don't fight the user mid-turn
  lights_on = bri >= 0;
  if (bri >= 0) level = bri;
  else if (level < 0) level = 1;
  level_dirty = true;
}

static void on_knob(void *, void *dir) {  // runs in the encoder's timer callback
  int d = (intptr_t)dir;
  if (!CLOCKWISE_BRIGHTER) d = -d;
  if (ui_mode != MODE_MAIN) {  // the menus use the knob for scrolling / adjusting
    __atomic_add_fetch(&menu_clicks, d, __ATOMIC_RELAXED);
    return;
  }
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
      HueRoom room;
      if (!get_room(sel_room, room)) break;
      int inc = constrain(clicks * DIM_STEP, -254, 254);
      char body[64];
      snprintf(body, sizeof body, inc > 0 ? "{\"on\":true,\"bri_inc\":%d,\"transitiontime\":2}"
                                           : "{\"bri_inc\":%d,\"transitiontime\":2}", inc);
      bool has_alert = false;
      for (int i = 0; i < room.n; i++) has_alert |= is_alert_light(room.member[i]);
      if (!has_alert) {
        int code = hue_put(String("/groups/") + room.id + "/action", body);
        Serial.printf("[%lu] dim %s %+d -> %d\n", millis(), room.name, inc, code);
        vTaskDelay(pdMS_TO_TICKS(250));  // Hue asks for <= ~4 group commands/s; extra clicks batch up
      } else {
        // Red alert in this room: dim every other bulb, one at a time.
        for (int i = 0; i < room.n; i++) {
          if (is_alert_light(room.member[i])) continue;
          hue_put(String("/lights/") + room.member[i] + "/state", body);
        }
        Serial.printf("[%lu] dim %s (minus alert bulbs) %+d\n", millis(), room.name, inc);
        vTaskDelay(pdMS_TO_TICKS(150 * room.n));  // per-light commands are rate limited harder
      }
    }
    settle = true;
  }
}
