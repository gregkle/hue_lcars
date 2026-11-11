// Red alert: the selected room's colour bulbs flash red until reset.
// Reset: those bulbs go back to white at the average brightness of the room's other lights.

static const char *RED  = "{\"on\":true,\"bri\":254,\"xy\":[0.675,0.322],\"transitiontime\":0}";
static const char *DARK = "{\"bri\":1,\"transitiontime\":0}";

static void request(Cmd c) {
  Serial.printf("[%lu] request %s\n", millis(), c == CMD_FLASH ? "FLASH" : "RESET");
  pending = c;
  if (worker) xTaskNotifyGive(worker);
}

// Sleeps for ms, returning true early if a new command arrived (so a reset can interrupt a flash).
static bool interrupted(int ms) {
  return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms)) > 0;
}

static void idle_screen() {
  if (alert_active || preview_alert) show(C_RED, "RED ALERT");
  else                               show(C_ORANGE, "STANDBY");
}

static bool set_lights(char (*ids)[6], int n, const char *body) {
  bool ok = true;
  for (int i = 0; i < n; i++) {
    int code = hue_put(String("/lights/") + ids[i] + "/state", body);
    if (code != 200) {
      ok = false;
      Serial.printf("PUT light %s -> %d\n", ids[i], code);
    }
  }
  return ok;
}

// The colour bulbs of a room.
static int colour_bulbs(const HueRoom &room, char (*ids)[6], int max) {
  int n = 0;
  for (int i = 0; i < room.n && n < max; i++) {
    HueLight l;
    if (get_light(room.member[i], l) && l.color) strcpy(ids[n++], l.id);
  }
  return n;
}

static void start_alert() {
  HueRoom room;
  static char ids[16][6];
  int n = get_room(sel_room, room) ? colour_bulbs(room, ids, 16) : 0;
  if (n == 0) {
    show(C_ORANGE, "NO COLOR BULBS");
    interrupted(2000);
    return;
  }
  memcpy(alert_ids, ids, sizeof ids);
  n_alert = n;
  alert_active = true;
  Serial.printf("[%lu] red alert: %d bulbs in %s\n", millis(), n, room.name);
  show(C_RED, "RED ALERT");

  // Two requests per bulb per cycle; slow down for many bulbs to stay inside Hue's rate limit.
  const int phase = max(FLASH_MS, 120 * n);
  bool was_ok = true;
  while (pending == CMD_NONE) {
    bool cycle_ok = set_lights(alert_ids, n, RED);
    if (interrupted(phase)) break;
    cycle_ok &= set_lights(alert_ids, n, DARK);
    if (interrupted(phase)) break;
    if (cycle_ok != was_ok) {  // only touch the screen when things change
      if (cycle_ok) show(C_RED, "RED ALERT");
      else show(C_ORANGE, WiFi.status() == WL_CONNECTED ? "NO BRIDGE" : "NO WIFI");
      was_ok = cycle_ok;
    }
  }
}

// Resets the alert bulbs (or, with no alert running, the selected room's colour bulbs) to
// white at the average brightness of the other lights in the room.
static bool reset_alert() {
  show(C_WARM, "RESETTING");
  static char ids[16][6];
  int n;
  HueRoom room;
  if (alert_active) {
    n = n_alert;
    memcpy(ids, alert_ids, sizeof ids);
    // Match the selected room if the alert bulbs are in it, otherwise the whole house.
    bool in_sel = get_room(sel_room, room);
    for (int i = 0; in_sel && i < n; i++) {
      bool found = false;
      for (int j = 0; j < room.n; j++) found |= !strcmp(room.member[j], ids[i]);
      in_sel = found;
    }
    if (!in_sel) get_room("0", room);
  } else {
    if (!get_room(sel_room, room)) return false;
    n = colour_bulbs(room, ids, 16);
    if (n == 0) { show(C_ORANGE, "NO COLOR BULBS"); interrupted(2000); return true; }
  }

  int bri = room_average_bri(room, ids, n);
  if (bri < 0) bri = 254;
  char white[64];
  snprintf(white, sizeof white, "{\"on\":true,\"bri\":%d,\"ct\":366}", bri);
  if (!set_lights(ids, n, white)) return false;  // alert stays on; RESET can be tried again
  alert_active = false;
  n_alert = 0;
  show(C_WARM, "LIGHTS RESET");
  interrupted(1500);
  return true;
}

static void worker_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (pending != CMD_NONE) {
      Cmd c = pending;
      pending = CMD_NONE;
      bool ok = true;
      if (c == CMD_RESET) ok = reset_alert();
      else start_alert();
      if (!ok) {
        show(C_ORANGE, WiFi.status() == WL_CONNECTED ? "BRIDGE ERROR" : "NO WIFI");
        interrupted(2500);
      }
      if (pending == CMD_NONE) idle_screen();
    }
  }
}
