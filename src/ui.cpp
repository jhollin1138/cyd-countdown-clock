// On-screen touch UI: menu, weather detail, home clock, keyboard, WiFi, trips, settings, calibration
#include "gfx.h"
#include <WiFi.h>
#include <functional>
#include <algorithm>

static UiScreen scr = UI_NONE;
static bool dirty = true;
static uint32_t lastAct = 0;
static uint32_t volSaveAt = 0;  // pending save after volume buttons
static const uint32_t UI_TIMEOUT_MS = 90000;

static const char* MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char* MONTH[] = {"January", "February", "March", "April", "May", "June", "July",
                              "August", "September", "October", "November", "December"};
static const char* WDAY[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char* WEEKDAY[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// ---------- Helpers ----------
static bool hit(const TouchEvt& e, int x, int y, int w, int h) {
  return e.x >= x && e.x < x + w && e.y >= y && e.y < y + h;
}

void uiButton(int x, int y, int w, int h, const String& label, uint16_t bg, uint16_t fg) {
  tft.fillRoundRect(x, y, w, h, 6, bg);
  tft.setFreeFont(&FreeSansBold9pt7b);
  txt(tft, fit(tft, label, w - 8), x + w / 2, y + h / 2 + 1, fg, MC_DATUM, 0);
}

static void arrow(int cx, int cy, bool up, uint16_t c) {
  if (up) tft.fillTriangle(cx - 8, cy + 5, cx + 8, cy + 5, cx, cy - 6, c);
  else tft.fillTriangle(cx - 8, cy - 5, cx + 8, cy - 5, cx, cy + 6, c);
}

static void header(const String& title, bool back = true) {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 30, C_CARD);
  if (back)
    for (int t = 0; t < 3; t++) {
      tft.drawLine(20 + t, 6, 11 + t, 15, TFT_WHITE);
      tft.drawLine(11 + t, 15, 20 + t, 24, TFT_WHITE);
    }
  tft.setFreeFont(&FreeSansBold9pt7b);
  txt(tft, fit(tft, title, 200), 160, 15, TFT_WHITE, MC_DATUM, 0);
}

static bool backHit(const TouchEvt& e) { return e.type == T_TAP && hit(e, 0, 0, 56, 32); }

static void go(UiScreen s) {
  scr = s;
  dirty = true;
  lastAct = millis();
}

bool uiActive() { return scr != UI_NONE; }
UiScreen uiScreen() { return scr; }

// Where the back arrow goes from each screen (UI_NONE = the countdown)
static UiScreen parentOf[UI_INFO + 1] = {};
static void openFrom(UiScreen s, UiScreen parent);
void uiClose();
static void goBack() {
  UiScreen p = parentOf[scr];
  if (p == UI_NONE) uiClose();
  else go(p);
}

void uiClose() {
  if (volSaveAt) { volSaveAt = 0; saveConfig(); }
  scr = UI_NONE;
  needRedraw = true;
}

static String onOff(bool b) { return b ? "On" : "Off"; }

static String fmtTarget(const String& target) {
  time_t t = parseLocal(target);
  if (!t) return "Not set";
  struct tm tm;
  localtime_r(&t, &tm);
  char b[24];
  snprintf(b, sizeof b, "%s %s %d %d", WDAY[tm.tm_wday], MON[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
  return String(b) + "  " + hhmm(tm, false);
}

static String uvLabel(float uv) {
  int u = (int)lroundf(uv);
  const char* l = u < 3 ? "Low" : u < 6 ? "Moderate" : u < 8 ? "High" : u < 11 ? "Very high" : "Extreme";
  return String(u) + " " + l;
}

// ---------- Keyboard ----------
static String kbTitle, kbText;
static int kbMax = 32;
static bool kbShift = false, kbSym = false;
static std::function<void(bool, const String&)> kbCb;
static const char* KB_L[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
static const char* KB_S[3] = {"1234567890", "@#$&*()'\"", "!?/:;_+"};
static const int KB_Y[4] = {70, 112, 154, 196};
static const int KB_H = 38;

static void kbOpen(const String& title, const String& init, int maxLen, std::function<void(bool, const String&)> cb) {
  kbTitle = title;
  kbText = init;
  kbMax = maxLen;
  kbShift = kbSym = false;
  kbCb = cb;
  go(UI_KB);
}

static int kbRowX(int r, int i) { return r == 0 ? 1 + i * 32 : r == 1 ? 17 + i * 32 : 49 + i * 32; }

static void kbField() {
  tft.fillRoundRect(4, 34, 312, 30, 6, C_CARD);
  tft.setFreeFont(&FreeSans9pt7b);
  String s = kbText;
  bool cut = false;
  while (s.length() && tft.textWidth(s) > 284) { s.remove(0, 1); cut = true; }
  if (cut) s = ".." + s.substring(2);
  txt(tft, s, 12, 49, TFT_WHITE, ML_DATUM, 0);
  int cx = 12 + tft.textWidth(s) + 2;
  tft.fillRect(cx, 39, 2, 20, C_ACCENT);
}

static void kbKey(int x, int y, int w, const String& label, uint16_t bg = C_BTN, uint16_t fg = TFT_WHITE) {
  tft.fillRoundRect(x, y, w, KB_H, 5, bg);
  tft.setFreeFont(&FreeSans9pt7b);
  txt(tft, label, x + w / 2, y + KB_H / 2, fg, MC_DATUM, 0);
}

static void kbKeys() {
  tft.fillRect(0, 68, 320, 172, C_BG);
  const char** rows = kbSym ? KB_S : KB_L;
  for (int r = 0; r < 3; r++) {
    int n = strlen(rows[r]);
    for (int i = 0; i < n; i++) {
      char c = rows[r][i];
      if (kbShift && !kbSym) c = toupper(c);
      kbKey(kbRowX(r, i), KB_Y[r], 30, String(c));
    }
  }
  if (kbSym) kbKey(1, KB_Y[2], 44, "%");
  else {
    tft.fillRoundRect(1, KB_Y[2], 44, KB_H, 5, kbShift ? C_ACCENT : C_BTN);
    arrow(23, KB_Y[2] + KB_H / 2, true, kbShift ? TFT_BLACK : TFT_WHITE);
  }
  kbKey(274, KB_Y[2], 45, "Del");
  kbKey(1, KB_Y[3], 58, kbSym ? "ABC" : "123");
  kbKey(62, KB_Y[3], 30, "-");
  kbKey(95, KB_Y[3], 126, "space");
  kbKey(224, KB_Y[3], 30, ".");
  kbKey(257, KB_Y[3], 62, "OK", C_GREEN);
}

static void kbDraw() {
  header(kbTitle);
  kbField();
  kbKeys();
}

static void kbAdd(char c) {
  if ((int)kbText.length() < kbMax) kbText += c;
  kbField();
}

static void kbTouch(const TouchEvt& e) {
  // Copy the callback first: it may open another keyboard, which replaces kbCb.
  auto cb = kbCb;
  if (backHit(e)) { cb(false, kbText); return; }
  if (e.type != T_TAP) return;
  int r = -1;
  for (int i = 0; i < 4; i++)
    if (e.y >= KB_Y[i] - 2 && e.y < KB_Y[i] + KB_H + 2) r = i;
  if (r < 0) return;
  const char** rows = kbSym ? KB_S : KB_L;
  if (r < 3) {
    if (r == 2 && e.x < 47) {
      if (kbSym) kbAdd('%');
      else { kbShift = !kbShift; kbKeys(); }
      return;
    }
    if (r == 2 && e.x >= 272) {
      if (kbText.length()) kbText.remove(kbText.length() - 1);
      kbField();
      return;
    }
    int n = strlen(rows[r]);
    for (int i = 0; i < n; i++) {
      int x = kbRowX(r, i);
      if (e.x >= x - 1 && e.x < x + 31) {
        char c = rows[r][i];
        if (kbShift && !kbSym) c = toupper(c);
        kbAdd(c);
        return;
      }
    }
    return;
  }
  if (e.x < 60) { kbSym = !kbSym; kbKeys(); }
  else if (e.x < 93) kbAdd('-');
  else if (e.x < 222) kbAdd(' ');
  else if (e.x < 255) kbAdd('.');
  else { String t = kbText; t.trim(); cb(true, t); }
}

// ---------- Place search ----------
static std::vector<GeoHit> geoHits;
static std::function<void(const GeoHit&)> geoPick;
static UiScreen geoReturn = UI_NONE;

static void geoSearch(const String& q, UiScreen ret, std::function<void(const GeoHit&)> onPick) {
  geoReturn = ret;
  geoPick = onPick;
  if (!q.length()) { go(ret); return; }
  displayMessage("Searching...", q);
  String err;
  if (!geocode(q, geoHits, err) || geoHits.empty()) {
    displayMessage("No matches found", err.length() ? err : String("Try a different spelling"), "",
                   "Tip: just the city name works best");
    delay(2500);
    go(ret);
    return;
  }
  go(UI_GEO);
}

static void geoDraw() {
  header("Choose a match");
  tft.setFreeFont(&FreeSans9pt7b);
  for (size_t i = 0; i < geoHits.size() && i < 5; i++) {
    int y = 34 + i * 40;
    tft.fillRoundRect(4, y, 312, 36, 6, C_CARD);
    txt(tft, fit(tft, geoHits[i].label, 296), 12, y + 18, TFT_WHITE, ML_DATUM, 0);
  }
}

static void geoTouch(const TouchEvt& e) {
  if (backHit(e)) { go(geoReturn); return; }
  if (e.type != T_TAP) return;
  for (size_t i = 0; i < geoHits.size() && i < 5; i++)
    if (hit(e, 4, 34 + i * 40, 312, 38)) {
      auto pick = geoPick;
      GeoHit h = geoHits[i];
      pick(h);
      return;
    }
}

static void setHome(const GeoHit& h) {
  xSemaphoreTake(dataMux, portMAX_DELAY);
  cfg.homeName = h.shortName;
  cfg.homeLat = h.lat;
  cfg.homeLon = h.lon;
  homeWx = Wx();
  xSemaphoreGive(dataMux);
  commitConfig(false);
}

// ---------- Menu (hold anywhere) ----------
static void menuDraw() {
  header("Menu");
  tft.setFreeFont(&FreeSans9pt7b);
  txt(tft, "Brightness", 8, 55, TFT_WHITE, ML_DATUM, 0);
  uiButton(104, 38, 44, 34, "-");
  tft.fillRoundRect(152, 38, 64, 34, 6, C_CARD);
  String v = cfg.autoBright ? "Auto" : String((cfg.manualBright * 100 + 127) / 255) + "%";
  tft.setFreeFont(&FreeSansBold9pt7b);
  txt(tft, v, 184, 56, C_ACCENT, MC_DATUM, 0);
  uiButton(220, 38, 44, 34, "+");
  uiButton(268, 38, 48, 34, "Auto", cfg.autoBright ? C_ACCENT : C_BTN, cfg.autoBright ? TFT_BLACK : TFT_WHITE);

  tft.setFreeFont(&FreeSans9pt7b);
  txt(tft, "Volume", 8, 97, TFT_WHITE, ML_DATUM, 0);
  uiButton(104, 80, 44, 34, "-");
  tft.fillRoundRect(152, 80, 64, 34, 6, C_CARD);
  tft.setFreeFont(&FreeSansBold9pt7b);
  txt(tft, cfg.sound ? String(cfg.volume) + "%" : String("Off"), 184, 98, cfg.sound ? C_ACCENT : C_DIM, MC_DATUM, 0);
  uiButton(220, 80, 44, 34, "+");
  uiButton(268, 80, 48, 34, "Mute", cfg.sound ? C_BTN : C_RED);

  static const char* L[6] = {"Events", "Weather", "Home clock", "WiFi", "Settings", "Info"};
  for (int i = 0; i < 6; i++) uiButton(4 + (i % 3) * 106, 124 + (i / 3) * 58, 100, 52, L[i]);
}

static void menuBright(int d) {
  int curv = cfg.autoBright ? backlightLevel() : cfg.manualBright;
  cfg.autoBright = false;
  cfg.manualBright = constrain(curv + d, 5, 255);
  saveConfig();
  dirty = true;
}

static void menuVolume(int d) {
  cfg.sound = true;
  cfg.volume = constrain(cfg.volume + d, 0, 100);
  if (cfg.volume == 0) cfg.sound = false;
  audioPlay(cfg.soundMile);  // preview at the new level
  volSaveAt = millis() + 1500;  // save once the buttons stop being pressed
  dirty = true;
}

static void tripsOpen();
static void wifiOpen();

static void menuTouch(const TouchEvt& e) {
  if (backHit(e)) { uiClose(); return; }
  if (e.type != T_TAP) return;
  if (hit(e, 104, 36, 46, 38)) menuBright(-26);
  else if (hit(e, 218, 36, 48, 38)) menuBright(26);
  else if (hit(e, 266, 36, 54, 38)) { cfg.autoBright = !cfg.autoBright; saveConfig(); dirty = true; }
  else if (hit(e, 104, 78, 46, 38)) menuVolume(-10);
  else if (hit(e, 218, 78, 48, 38)) menuVolume(10);
  else if (hit(e, 266, 78, 54, 38)) {
    cfg.sound = !cfg.sound;
    if (cfg.sound && cfg.volume == 0) cfg.volume = 30;
    if (!cfg.sound) audioStop();
    saveConfig();
    dirty = true;
  } else {
    for (int i = 0; i < 6; i++) {
      if (!hit(e, 4 + (i % 3) * 106, 124 + (i / 3) * 58, 100, 54)) continue;
      switch (i) {
        case 0: openFrom(UI_TRIPS, UI_MENU); break;
        case 1: openFrom(UI_WX, UI_MENU); break;
        case 2: openFrom(UI_HOME, UI_MENU); break;
        case 3: openFrom(UI_WIFI, UI_MENU); break;
        case 4: openFrom(UI_SETTINGS, UI_MENU); break;
        case 5: openFrom(UI_INFO, UI_MENU); break;
      }
      return;
    }
  }
}

// ---------- Info ----------
static void infoDraw() {
  header("Info");
  tft.setFreeFont(&FreeSans9pt7b);
  uint32_t up = millis() / 1000;
  String lines[7] = {
    FW_NAME "  v" FW_VERSION,
    "Created by " FW_AUTHOR,
    "Settings page: http://" HOSTNAME ".local",
    "IP address: " + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("not connected")),
    "WiFi: " + WiFi.SSID() + "  (" + String(WiFi.RSSI()) + " dBm)",
    "Free memory: " + String(ESP.getFreeHeap() / 1024) + " KB",
    "Running for " + String(up / 86400) + "d " + String(up / 3600 % 24) + "h " + String(up / 60 % 60) + "m",
  };
  for (int i = 0; i < 7; i++)
    txt(tft, fit(tft, lines[i], 304), 10, 46 + i * 27, i == 1 ? C_DIM : i == 2 ? C_ACCENT : TFT_WHITE, ML_DATUM, 0);
}

// ---------- Destination weather detail ----------
static int wxMin = -1;

static void wxRow(int y, const String& label, const String& val) {
  tft.setTextFont(2);
  txt(tft, label, 192, y, C_DIM, TL_DATUM, 0);
  txt(tft, val, 314, y, TFT_WHITE, TR_DATUM, 0);
}

static void forecastCols(const Wx& w, int first, int n, int x0, int colW, int y, int iconSz, bool pop) {
  for (int k = 0; k < n; k++) {
    int i = first + k;
    if (i >= w.days) break;
    int cx = x0 + k * colW + colW / 2;
    struct tm dt = destTm(w.dDate[i], w.utcOff);
    tft.setTextFont(2);
    txt(tft, i == 0 ? String("Today") : String(WDAY[dt.tm_wday]), cx, y, TFT_WHITE, TC_DATUM, 0);
    drawIcon(cx - iconSz / 2, y + 18, iconSz, w.dCode[i], true);
    int ty = y + 22 + iconSz;
    tft.setTextFont(2);
    txt(tft, String((int)lroundf(w.dHi[i])), cx - 3, ty, TFT_WHITE, TR_DATUM, 0);
    txt(tft, "/" + String((int)lroundf(w.dLo[i])), cx - 3, ty, C_DIM, TL_DATUM, 0);
    if (pop && w.dPop[i] >= 0) {
      String p = String(w.dPop[i]) + "%";
      int pw = tft.textWidth(p) + 9;
      int px = cx - pw / 2;
      tft.fillCircle(px + 3, ty + 25, 3, C_SKYBL);
      tft.fillTriangle(px, ty + 24, px + 6, ty + 24, px + 3, ty + 18, C_SKYBL);
      txt(tft, p, px + 9, ty + 16, C_SKYBL, TL_DATUM, 0);
    }
  }
}

static void wxHeaderClock(const Wx& w) {
  if (!timeValid() || !w.valid) return;
  tft.fillRect(258, 0, 62, 30, C_CARD);
  tft.setTextFont(2);
  txt(tft, hhmm(destTm(time(nullptr), w.utcOff), true), 314, 15, C_ACCENT, MR_DATUM, 0);
}

static bool wxSnapshot(Event& e) {
  int idx = activeTrip();
  if (idx < 0) return false;
  xSemaphoreTake(dataMux, portMAX_DELAY);
  bool ok = idx < (int)cfg.events.size();
  if (ok) e = cfg.events[idx];
  xSemaphoreGive(dataMux);
  return ok;
}

static void wxDraw() {
  Event e;
  wxMin = -1;
  if (!wxSnapshot(e)) {
    header("Weather");
    tft.setFreeFont(&FreeSans9pt7b);
    txt(tft, "No event to show yet", 160, 100, C_DIM, MC_DATUM, 0);
    uiButton(70, 140, 180, 44, "Add an event", C_ACCENT, TFT_BLACK);
    return;
  }
  header(e.place.length() ? e.place : e.name);
  if (isnan(e.lat)) {
    tft.setFreeFont(&FreeSans9pt7b);
    txt(tft, "No location set for " + fit(tft, e.name, 120), 160, 100, C_DIM, MC_DATUM, 0);
    uiButton(70, 140, 180, 44, "Set location", C_ACCENT, TFT_BLACK);
    return;
  }
  const Wx& w = e.wx;
  if (!w.valid) {
    tft.setFreeFont(&FreeSans9pt7b);
    txt(tft, "Loading weather...", 160, 120, C_DIM, MC_DATUM, 0);
    return;
  }
  wxHeaderClock(w);
  drawIcon(6, 38, 56, w.code, w.isDay);
  tempTxt(w.temp, 70, 42, 0, TFT_WHITE, 6, 4, true);
  tft.setFreeFont(&FreeSansBold9pt7b);
  txt(tft, fit(tft, wxText(w.code), 176), 8, 112, TFT_WHITE, ML_DATUM, 0);
  tft.setTextFont(2);
  txt(tft, "High " + String((int)lroundf(w.hi)) + "   Low " + String((int)lroundf(w.lo)), 8, 128, C_DIM, TL_DATUM, 0);

  String deg = cfg.metric ? " C" : " F";
  wxRow(38, "Feels like", String((int)lroundf(w.feels)) + deg);
  wxRow(55, "Humidity", String(w.humidity) + "%");
  wxRow(72, "Wind", String((int)lroundf(w.wind)) + (cfg.metric ? " km/h" : " mph"));
  wxRow(89, "UV index", uvLabel(w.uv));
  if (w.sunrise) wxRow(106, "Sunrise", hhmm(destTm(w.sunrise, w.utcOff), true));
  if (w.sunset) wxRow(123, "Sunset", hhmm(destTm(w.sunset, w.utcOff), true));

  tft.drawFastHLine(8, 148, 304, C_CARD);
  forecastCols(w, 0, 5, 0, 64, 152, 30, true);
}

static void wxTick() {
  if (!timeValid()) return;
  struct tm t = homeTm(time(nullptr));
  if (t.tm_min == wxMin) return;
  wxMin = t.tm_min;
  Event e;
  if (wxSnapshot(e) && !isnan(e.lat)) wxHeaderClock(e.wx);
}

static void tripEditOpen(int idx);

static void wxTouch(const TouchEvt& e) {
  if (backHit(e)) { goBack(); return; }
  if (e.type == T_SWIPE_L || e.type == T_SWIPE_R) { goBack(); return; }
  if (e.type != T_TAP) return;
  if (hit(e, 70, 136, 180, 52)) {
    Event ev;
    if (!wxSnapshot(ev)) tripEditOpen(-1);
    else if (isnan(ev.lat)) tripEditOpen(activeTrip());
  }
}

// ---------- Home clock & weather ----------
static int hmMin = -1, hmDay = -1;

static void homeTime(const struct tm& t) {
  char b[8];
  int h = t.tm_hour;
  if (cfg.h24) snprintf(b, sizeof b, "%02d:%02d", h, t.tm_min);
  else {
    h %= 12;
    if (!h) h = 12;
    snprintf(b, sizeof b, "%d:%02d", h, t.tm_min);
  }
  int cx = cfg.h24 ? 160 : 146;
  tft.setTextFont(8);
  tft.setTextColor(TFT_WHITE, C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(tft.textWidth("88:88"));
  tft.drawString(b, cx, 76);
  if (!cfg.h24) {
    tft.setTextFont(4);
    tft.setTextColor(C_ACCENT, C_BG);
    tft.setTextDatum(ML_DATUM);
    tft.setTextPadding(tft.textWidth("PM"));
    tft.drawString(t.tm_hour < 12 ? "AM" : "PM", 282, 98);
  }
  tft.setTextPadding(0);
}

static void homeDate(const struct tm& t) {
  tft.fillRect(0, 116, 320, 20, C_BG);
  tft.setFreeFont(&FreeSans9pt7b);
  String d = String(WEEKDAY[t.tm_wday]) + ", " + MONTH[t.tm_mon] + " " + String(t.tm_mday);
  txt(tft, d, 160, 125, C_DIM, MC_DATUM, 0);
}

static void homeDraw() {
  header(cfg.homeName.length() ? cfg.homeName : String("Home"));
  hmMin = hmDay = -1;
  tft.drawFastHLine(8, 140, 304, C_CARD);
  if (isnan(cfg.homeLat)) {
    tft.setTextFont(2);
    txt(tft, "Set your home location to see local weather", 160, 160, C_DIM, MC_DATUM, 0);
    uiButton(70, 182, 180, 44, "Set home location", C_ACCENT, TFT_BLACK);
    return;
  }
  Wx w;
  xSemaphoreTake(dataMux, portMAX_DELAY);
  w = homeWx;
  xSemaphoreGive(dataMux);
  if (!w.valid) {
    tft.setTextFont(2);
    txt(tft, "Loading weather...", 160, 190, C_DIM, MC_DATUM, 0);
    return;
  }
  // Left: today. Right: next 4 days with chance of rain.
  drawIcon(2, 150, 40, w.code, w.isDay);
  tempTxt(w.temp, 46, 154, 0, TFT_WHITE, 4, 4, true);
  tft.setTextFont(2);
  txt(tft, fit(tft, wxText(w.code), 112), 6, 198, TFT_WHITE, TL_DATUM, 0);
  txt(tft, "H " + String((int)lroundf(w.hi)) + "  L " + String((int)lroundf(w.lo)), 6, 217, C_DIM, TL_DATUM, 0);
  tft.drawFastVLine(120, 148, 86, C_CARD);
  forecastCols(w, 1, 4, 122, 49, 145, 26, true);
}

static void homeTick() {
  if (!timeValid()) return;
  struct tm t = homeTm(time(nullptr));
  if (t.tm_min != hmMin) { hmMin = t.tm_min; homeTime(t); }
  if (t.tm_yday != hmDay) { hmDay = t.tm_yday; homeDate(t); }
}

static void homeTouch(const TouchEvt& e) {
  if (backHit(e)) { goBack(); return; }
  if (e.type == T_SWIPE_L || e.type == T_SWIPE_R) { goBack(); return; }
  if (e.type != T_TAP) return;
  if (isnan(cfg.homeLat) && hit(e, 70, 178, 180, 52)) {
    kbOpen("Home city", cfg.homeName, 40, [](bool ok, const String& t) {
      if (ok && t.length()) geoSearch(t, UI_HOME, [](const GeoHit& h) { setHome(h); go(UI_HOME); });
      else go(UI_HOME);
    });
  } else if (e.y > 32 && e.y < 136) {
    uiClose();  // tap the clock to return to the countdown
  }
}

// ---------- WiFi ----------
struct Net { String ssid; int rssi; bool open; };
static std::vector<Net> nets;
static int netOff = 0;

static void wifiScan() {
  displayMessage("WiFi", "Scanning for networks...");
  int n = WiFi.scanNetworks();
  nets.clear();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (Net& x : nets)
      if (x.ssid == s) { dup = true; x.rssi = max(x.rssi, (int)WiFi.RSSI(i)); break; }
    if (!dup && nets.size() < 24) nets.push_back({s, (int)WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(nets.begin(), nets.end(), [](const Net& a, const Net& b) { return a.rssi > b.rssi; });
  netOff = 0;
}

static void wifiOpen() {
  wifiScan();
  go(UI_WIFI);
}

static void wifiDraw() {
  header("Choose WiFi");
  if (nets.empty()) {
    tft.setFreeFont(&FreeSans9pt7b);
    txt(tft, "No networks found", 160, 110, C_DIM, MC_DATUM, 0);
  }
  String curSsid = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("");
  for (int i = 0; i < 5 && netOff + i < (int)nets.size(); i++) {
    const Net& n = nets[netOff + i];
    int y = 32 + i * 34;
    tft.fillRoundRect(4, y + 1, 312, 32, 6, C_CARD);
    tft.setFreeFont(&FreeSans9pt7b);
    bool isCur = n.ssid == curSsid;
    txt(tft, fit(tft, n.ssid, isCur ? 180 : 230), 12, y + 17, isCur ? C_ACCENT : TFT_WHITE, ML_DATUM, 0);
    if (isCur) {
      tft.setTextFont(1);
      txt(tft, "connected", 250, y + 17, C_ACCENT, MR_DATUM, 0);
    }
    if (!n.open) {  // padlock
      tft.drawRoundRect(258, y + 8, 8, 10, 3, C_DIM);
      tft.fillRect(256, y + 13, 12, 9, C_DIM);
    }
    static const int TH[4] = {-85, -75, -65, -55};
    for (int b = 0; b < 4; b++) {
      int h = 5 + b * 4, bx = 276 + b * 8, by = y + 26 - h;
      if (n.rssi > TH[b]) tft.fillRect(bx, by, 5, h, TFT_WHITE);
      else tft.drawRect(bx, by, 5, h, 0x632C);
    }
  }
  uiButton(4, 204, 60, 32, "");
  arrow(34, 220, true, TFT_WHITE);
  uiButton(68, 204, 60, 32, "");
  arrow(98, 220, false, TFT_WHITE);
  uiButton(132, 204, 90, 32, "Rescan");
  uiButton(226, 204, 90, 32, "Other");
}

static void wifiConnect(const String& ssid, const String& pass) {
  displayMessage("Connecting...", ssid, "", "This can take up to 20 seconds");
  String oldS = WiFi.SSID(), oldP = WiFi.psk();
  WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : nullptr);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    hwLoop();
    delay(50);
  }
  if (WiFi.status() == WL_CONNECTED) {
    displayMessage("Connected!", ssid, "http://" HOSTNAME ".local", WiFi.localIP().toString());
    delay(3500);
    scr = UI_WIFI;  // we're on the keyboard screen; return to wherever WiFi was opened from
    goBack();
    return;
  }
  displayMessage("Couldn't connect", ssid, "Check the password and try again");
  delay(3000);
  if (oldS.length() && oldS != ssid) WiFi.begin(oldS.c_str(), oldP.length() ? oldP.c_str() : nullptr);
  go(UI_WIFI);
}

static void wifiAskPass(const String& ssid) {
  kbOpen("Password: " + ssid, "", 63, [ssid](bool ok, const String& p) {
    if (ok) wifiConnect(ssid, p);
    else go(UI_WIFI);
  });
}

static void wifiTouch(const TouchEvt& e) {
  if (backHit(e)) { goBack(); return; }
  int maxOff = max(0, (int)nets.size() - 5);
  if (e.type == T_SWIPE_U) { netOff = min(maxOff, netOff + 5); dirty = true; return; }
  if (e.type == T_SWIPE_D) { netOff = max(0, netOff - 5); dirty = true; return; }
  if (e.type != T_TAP) return;
  if (e.y >= 202) {
    if (e.x < 66) netOff = max(0, netOff - 5);
    else if (e.x < 130) netOff = min(maxOff, netOff + 5);
    else if (e.x < 224) { wifiScan(); }
    else {
      kbOpen("Network name", "", 32, [](bool ok, const String& s) {
        if (ok && s.length()) wifiAskPass(s);
        else go(UI_WIFI);
      });
      return;
    }
    dirty = true;
    return;
  }
  int i = (e.y - 32) / 34;
  if (e.y < 32 || i < 0 || i > 4 || netOff + i >= (int)nets.size()) return;
  const Net& n = nets[netOff + i];
  if (n.open) wifiConnect(n.ssid, "");
  else wifiAskPass(n.ssid);
}

// ---------- Trips list ----------
static void tripsOpen() { go(UI_TRIPS); }

static void tripsDraw() {
  header("Events");
  std::vector<Event> evs;
  xSemaphoreTake(dataMux, portMAX_DELAY);
  for (const Event& e : cfg.events) {
    Event c;
    c.name = e.name;
    c.target = e.target;
    c.epoch = e.epoch;
    evs.push_back(c);
  }
  xSemaphoreGive(dataMux);
  if (evs.empty()) {
    tft.setFreeFont(&FreeSans9pt7b);
    txt(tft, "No events yet", 160, 100, C_DIM, MC_DATUM, 0);
  }
  for (size_t i = 0; i < evs.size(); i++) {
    int y = 34 + i * 28;
    tft.fillRoundRect(4, y, 312, 26, 5, C_CARD);
    tft.setFreeFont(&FreeSans9pt7b);
    txt(tft, fit(tft, evs[i].name, 176), 12, y + 13, TFT_WHITE, ML_DATUM, 0);
    tft.setTextFont(2);
    String d = "Date not set";
    if (evs[i].epoch) {
      struct tm t = homeTm(evs[i].epoch);
      d = String(MON[t.tm_mon]) + " " + String(t.tm_mday) + ", " + String(t.tm_year + 1900);
    }
    txt(tft, d, 308, y + 13, C_DIM, MR_DATUM, 0);
  }
  if (evs.size() < MAX_EVENTS) uiButton(4, 204, 312, 32, "+ Add event", C_ACCENT, TFT_BLACK);
}

static void tripsTouch(const TouchEvt& e) {
  if (backHit(e)) { goBack(); return; }
  if (e.type != T_TAP) return;
  int n = cfg.events.size();
  if (e.y >= 202 && n < MAX_EVENTS) { tripEditOpen(-1); return; }
  int i = (e.y - 34) / 28;
  if (e.y >= 34 && i >= 0 && i < n) tripEditOpen(i);
}

// ---------- Trip editor ----------
static Event ed;
static int edIdx = -1;
static String edOrigName, edOrigTarget;
static uint32_t edGen = 0;
static bool edDelArm = false;

static void tripEditOpen(int idx) {
  xSemaphoreTake(dataMux, portMAX_DELAY);
  if (idx >= 0 && idx < (int)cfg.events.size()) {
    ed = cfg.events[idx];
  } else {
    idx = -1;
    ed = Event();
    ed.name = "New event";
    time_t t = timeValid() ? time(nullptr) + 30L * 86400 : 1798790400;  // fallback 2027-01-01
    struct tm tm = homeTm(t);
    tm.tm_hour = 8;
    tm.tm_min = 0;
    ed.target = formatLocal(tm);
    ed.img = "";
    ed.shade = 1;
  }
  edGen = configGen;
  xSemaphoreGive(dataMux);
  edIdx = idx;
  edOrigName = ed.name;
  edOrigTarget = ed.target;
  edDelArm = false;
  go(UI_TRIP);
}

static const char* SHADE_NAMES[4] = {"None", "Light", "Medium", "Dark"};

static String imgLabel(const String& img) {
  if (!img.length()) return "Sunset";
  String s = img;
  int dot = s.lastIndexOf('.');
  return dot > 0 ? s.substring(0, dot) : s;
}

#define TR_Y0 34
#define TR_H 32
static void tripRow(int r, const String& label, const String& val) {
  int y = TR_Y0 + r * TR_H;
  tft.fillRoundRect(4, y, 312, TR_H - 3, 6, C_CARD);
  int cy = y + (TR_H - 3) / 2;
  tft.setTextFont(2);
  txt(tft, label, 12, cy, C_DIM, ML_DATUM, 0);
  tft.setFreeFont(&FreeSans9pt7b);
  txt(tft, fit(tft, val, 222), 296, cy, TFT_WHITE, MR_DATUM, 0);
  tft.drawLine(303, cy - 5, 308, cy, C_DIM);
  tft.drawLine(308, cy, 303, cy + 5, C_DIM);
}

static void tripDraw() {
  header(edIdx < 0 ? "New event" : "Edit event");
  tripRow(0, "Name", ed.name);
  tripRow(1, "When", fmtTarget(ed.target));
  tripRow(2, "Where", ed.place.length() ? ed.place : String("Not set"));
  tripRow(3, "Photo", imgLabel(ed.img));
  tripRow(4, "Shading", SHADE_NAMES[constrain(ed.shade, 0, 3)]);
  if (edIdx >= 0) uiButton(4, 198, 110, 38, edDelArm ? "Tap again" : "Delete", C_RED);
  uiButton(206, 198, 110, 38, "Save", C_GREEN);
}

static int findEdited() {
  // Called with dataMux held. Finds the trip being edited even if the list changed meanwhile.
  if (edIdx < 0) return -1;
  if (edGen == configGen && edIdx < (int)cfg.events.size()) return edIdx;
  for (size_t i = 0; i < cfg.events.size(); i++)
    if (cfg.events[i].name == edOrigName && cfg.events[i].target == edOrigTarget) return i;
  return -1;
}

static void tripSave() {
  time_t ep = parseLocal(ed.target);
  if (!ep) return;
  time_t now = time(nullptr);
  xSemaphoreTake(dataMux, portMAX_DELAY);
  int idx = findEdited();
  ed.epoch = ep;
  if (idx < 0 || cfg.events[idx].target != ed.target) ed.ms = dayBucket(ep, now);
  if (idx >= 0) {
    Event& o = cfg.events[idx];
    bool sameLoc = (isnan(o.lat) && isnan(ed.lat)) || (o.lat == ed.lat && o.lon == ed.lon);
    ed.wx = sameLoc ? o.wx : Wx();
    o = ed;
  } else if (cfg.events.size() < MAX_EVENTS) {
    ed.wx = Wx();
    cfg.events.push_back(ed);
    idx = cfg.events.size() - 1;
  }
  if (idx >= 0) {
    sortEvents(cfg.events);
    for (size_t i = 0; i < cfg.events.size(); i++)
      if (cfg.events[i].name == ed.name && cfg.events[i].target == ed.target) { idx = i; break; }
  }
  configGen++;
  xSemaphoreGive(dataMux);
  saveConfig();
  if (idx >= 0) showTrip(idx);
  uiClose();
}

static void tripDelete() {
  xSemaphoreTake(dataMux, portMAX_DELAY);
  int idx = findEdited();
  if (idx >= 0) cfg.events.erase(cfg.events.begin() + idx);
  configGen++;
  xSemaphoreGive(dataMux);
  saveConfig();
  go(UI_TRIPS);
}

static void dateOpen();

static void tripTouch(const TouchEvt& e) {
  if (backHit(e)) { go(UI_TRIPS); return; }
  if (e.type != T_TAP) return;
  if (e.y >= 196) {
    if (edIdx >= 0 && e.x < 118) {
      if (edDelArm) tripDelete();
      else { edDelArm = true; dirty = true; }
    } else if (e.x >= 202) {
      tripSave();
    }
    return;
  }
  int r = (e.y - TR_Y0) / TR_H;
  if (e.y < TR_Y0 || r < 0 || r > 4) return;
  edDelArm = false;
  switch (r) {
    case 0:
      kbOpen("Event name", ed.name, 32, [](bool ok, const String& t) {
        if (ok && t.length()) ed.name = t;
        go(UI_TRIP);
      });
      break;
    case 1: dateOpen(); break;
    case 2:
      kbOpen("Location (city)", ed.place, 40, [](bool ok, const String& t) {
        if (ok && t.length())
          geoSearch(t, UI_TRIP, [](const GeoHit& h) {
            ed.place = h.shortName;
            ed.lat = h.lat;
            ed.lon = h.lon;
            go(UI_TRIP);
          });
        else go(UI_TRIP);
      });
      break;
    case 4:
      ed.shade = (ed.shade + 1) % 4;
      dirty = true;
      break;
    case 3: {
      // cycle: Sunset -> each photo
      std::vector<String> opts = {""};
      for (const String& s : images) opts.push_back(s);
      size_t k = 0;
      for (size_t i = 0; i < opts.size(); i++) if (opts[i] == ed.img) k = i;
      ed.img = opts[(k + 1) % opts.size()];
      dirty = true;
      break;
    }
  }
}

// ---------- Date & time picker ----------
static struct tm dt;

static void dateOpen() {
  time_t t = parseLocal(ed.target);
  if (!t) t = time(nullptr);
  localtime_r(&t, &dt);
  dt.tm_sec = 0;
  go(UI_DATE);
}

static int daysIn(int y, int m) {
  static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return m == 1 && leap ? 29 : d[m];
}

static void dateValues() {
  tft.fillRect(0, 82, 320, 58, C_BG);
  tft.fillRect(108, 192, 104, 44, C_BG);
  int h = dt.tm_hour;
  char hb[6], mb[6];
  if (cfg.h24) snprintf(hb, sizeof hb, "%02d", h);
  else snprintf(hb, sizeof hb, "%d", h % 12 ? h % 12 : 12);
  snprintf(mb, sizeof mb, "%02d", dt.tm_min);
  String vals[5] = {MON[dt.tm_mon], String(dt.tm_mday), String(dt.tm_year + 1900), hb, mb};
  String labs[5] = {"Month", "Day", "Year", cfg.h24 ? String("Hour") : String(h < 12 ? "AM" : "PM"), "Min"};
  for (int c = 0; c < 5; c++) {
    int cx = c * 64 + 32;
    tft.setFreeFont(&FreeSansBold12pt7b);
    txt(tft, vals[c], cx, 104, TFT_WHITE, MC_DATUM, 0);
    tft.setTextFont(2);
    txt(tft, labs[c], cx, 130, c == 3 && !cfg.h24 ? C_ACCENT : C_DIM, MC_DATUM, 0);
  }
  tft.setFreeFont(&FreeSans9pt7b);
  txt(tft, WEEKDAY[dt.tm_wday], 160, 214, C_ACCENT, MC_DATUM, 0);
}

static void dateDraw() {
  header("Date & time");
  for (int c = 0; c < 5; c++) {
    int x = c * 64 + 4;
    tft.fillRoundRect(x, 38, 56, 40, 6, C_BTN);
    arrow(x + 28, 58, true, TFT_WHITE);
    tft.fillRoundRect(x, 144, 56, 40, 6, C_BTN);
    arrow(x + 28, 164, false, TFT_WHITE);
  }
  uiButton(4, 194, 100, 42, "Cancel");
  uiButton(216, 194, 100, 42, "OK", C_GREEN);
  dateValues();
}

static void dateStep(int c, int d) {
  int Y = dt.tm_year + 1900, M = dt.tm_mon, D = dt.tm_mday, h = dt.tm_hour, mi = dt.tm_min;
  switch (c) {
    case 0: M = (M + d + 12) % 12; break;
    case 1:
      D += d;
      if (D < 1) D = daysIn(Y, M);
      else if (D > daysIn(Y, M)) D = 1;
      break;
    case 2: Y = constrain(Y + d, 2024, 2099); break;
    case 3: h = (h + d + 24) % 24; break;
    case 4: mi = d > 0 ? (mi / 5 * 5 + 5) % 60 : ((mi + 4) / 5 * 5 - 5 + 60) % 60; break;
  }
  D = min(D, daysIn(Y, M));
  dt.tm_year = Y - 1900;
  dt.tm_mon = M;
  dt.tm_mday = D;
  dt.tm_hour = h;
  dt.tm_min = mi;
  dt.tm_sec = 0;
  dt.tm_isdst = -1;
  time_t t = mktime(&dt);
  localtime_r(&t, &dt);
  dateValues();
}

static void dateTouch(const TouchEvt& e) {
  if (backHit(e)) { go(UI_TRIP); return; }
  if (e.type != T_TAP) return;
  int c = min(4, e.x / 64);
  if (e.y >= 34 && e.y < 82) dateStep(c, 1);
  else if (e.y >= 140 && e.y < 188) dateStep(c, -1);
  else if (e.y >= 190 && e.x < 110) go(UI_TRIP);
  else if (e.y >= 190 && e.x >= 210) {
    ed.target = formatLocal(dt);
    go(UI_TRIP);
  }
}

// ---------- Settings ----------
enum { ST_UNITS, ST_CLOCK, ST_BRIGHT, ST_QUIET, ST_SOUND, ST_TEST, ST_LED, ST_MODE, ST_HOME, ST_WIFI,
       ST_CAL, ST_INVERT, ST_FLIP, ST_RESTART, ST_COUNT };
static int stOff = 0;

static String stLabel(int i) {
  static const char* L[ST_COUNT] = {"Units", "Clock", "Brightness", "Quiet hours", "Sound", "Test sounds", "LED effects",
                                    "Several events", "Home location", "WiFi", "Calibrate touch",
                                    "Invert colors", "Rotate 180", "Restart clock"};
  return L[i];
}

static String stValue(int i) {
  switch (i) {
    case ST_UNITS: return cfg.metric ? "C, km/h" : "F, mph";
    case ST_CLOCK: return cfg.h24 ? "24-hour" : "12-hour";
    case ST_BRIGHT: return cfg.autoBright ? "Auto" : "Manual";
    case ST_QUIET: return cfg.quietOn ? cfg.quietStart + "-" + cfg.quietEnd : String("Off");
    case ST_SOUND: return cfg.sound ? String(cfg.volume) + "%" : String("Muted");
    case ST_LED: return onOff(cfg.led);
    case ST_MODE: return cfg.mode == "rotate" ? "Rotate" : "Show next";
    case ST_HOME: return cfg.homeName.length() ? cfg.homeName : String("Not set");
    case ST_WIFI: return WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("Not connected");
    case ST_INVERT: return onOff(cfg.invert);
    case ST_FLIP: return onOff(cfg.flip);
    default: return "";
  }
}

static void settingsDraw() {
  header("Settings");
  tft.fillRoundRect(238, 2, 38, 26, 5, C_BTN);
  arrow(257, 15, true, stOff > 0 ? TFT_WHITE : 0x632C);
  tft.fillRoundRect(280, 2, 38, 26, 5, C_BTN);
  arrow(299, 15, false, stOff + 6 < ST_COUNT ? TFT_WHITE : 0x632C);
  for (int r = 0; r < 6 && stOff + r < ST_COUNT; r++) {
    int i = stOff + r, y = 32 + r * 34;
    tft.setFreeFont(&FreeSans9pt7b);
    txt(tft, stLabel(i), 10, y + 17, TFT_WHITE, ML_DATUM, 0);
    String v = stValue(i);
    if (v.length()) txt(tft, fit(tft, v, 150), 310, y + 17, C_ACCENT, MR_DATUM, 0);
    else {
      tft.drawLine(303, y + 12, 308, y + 17, C_DIM);
      tft.drawLine(308, y + 17, 303, y + 22, C_DIM);
    }
    tft.drawFastHLine(8, y + 33, 304, C_CARD);
  }
}

static void settingsAct(int i) {
  switch (i) {
    case ST_UNITS: cfg.metric = !cfg.metric; commitConfig(true); break;
    case ST_CLOCK: cfg.h24 = !cfg.h24; commitConfig(false); break;
    case ST_BRIGHT: cfg.autoBright = !cfg.autoBright; commitConfig(false); break;
    case ST_QUIET: cfg.quietOn = !cfg.quietOn; commitConfig(false); break;
    case ST_SOUND:
      cfg.sound = !cfg.sound;
      if (cfg.sound && cfg.volume == 0) cfg.volume = 30;
      if (!cfg.sound) audioStop();
      commitConfig(false);
      break;
    case ST_TEST: celebrate(0, true); uiClose(); return;
    case ST_LED: cfg.led = !cfg.led; if (!cfg.led) ledEffect(0, 0, false); commitConfig(false); break;
    case ST_MODE: cfg.mode = cfg.mode == "rotate" ? "nearest" : "rotate"; commitConfig(false); break;
    case ST_HOME:
      kbOpen("Home city", cfg.homeName, 40, [](bool ok, const String& t) {
        if (ok && t.length()) geoSearch(t, UI_SETTINGS, [](const GeoHit& h) { setHome(h); go(UI_SETTINGS); });
        else go(UI_SETTINGS);
      });
      return;
    case ST_WIFI: openFrom(UI_WIFI, UI_SETTINGS); return;
    case ST_CAL: runCalibration(); break;
    case ST_INVERT: cfg.invert = !cfg.invert; saveConfig(); applyDisplaySettings(); break;
    case ST_FLIP: cfg.flip = !cfg.flip; saveConfig(); applyDisplaySettings(); break;
    case ST_RESTART:
      displayMessage("Restarting...", "");
      delay(500);
      ESP.restart();
      return;
  }
  dirty = true;
}

static void settingsTouch(const TouchEvt& e) {
  if (backHit(e)) { goBack(); return; }
  int maxOff = ST_COUNT - 6;
  if (e.type == T_SWIPE_U) { stOff = min(maxOff, stOff + 3); dirty = true; return; }
  if (e.type == T_SWIPE_D) { stOff = max(0, stOff - 3); dirty = true; return; }
  if (e.type != T_TAP) return;
  if (e.y < 32) {
    if (e.x >= 236 && e.x < 278) { stOff = max(0, stOff - 3); dirty = true; }
    else if (e.x >= 278) { stOff = min(maxOff, stOff + 3); dirty = true; }
    return;
  }
  int r = (e.y - 32) / 34;
  if (r >= 0 && r < 6 && stOff + r < ST_COUNT) settingsAct(stOff + r);
}

// ---------- Touch calibration (blocking) ----------
static TouchEvt waitTap() {
  takeTouch();
  for (;;) {
    hwLoop();
    if (netState == NET_ONLINE) webLoop();
    TouchEvt e = takeTouch();
    if (e.type == T_TAP || e.type == T_LONG) return e;
    delay(5);
  }
}

bool runCalibration() {
  static const int TX[3] = {30, 290, 30}, TY[3] = {30, 30, 210};
  for (;;) {
    int ra[3], rb[3];
    for (int i = 0; i < 3; i++) {
      tft.fillScreen(C_BG);
      tft.setFreeFont(&FreeSansBold9pt7b);
      txt(tft, "Touch calibration", 160, 96, TFT_WHITE, MC_DATUM, 0);
      tft.setTextFont(2);
      txt(tft, "Tap the center of the cross  (" + String(i + 1) + " of 3)", 160, 124, C_DIM, MC_DATUM, 0);
      txt(tft, "A fingernail or stylus works best", 160, 144, C_DIM, MC_DATUM, 0);
      tft.drawFastHLine(TX[i] - 14, TY[i], 29, C_ACCENT);
      tft.drawFastVLine(TX[i], TY[i] - 14, 29, C_ACCENT);
      tft.drawCircle(TX[i], TY[i], 7, C_ACCENT);
      TouchEvt e = waitTap();
      ra[i] = e.ra;
      rb[i] = e.rb;
    }
    // Which raw channel moves when the finger moves across the screen?
    bool swap = abs(rb[1] - rb[0]) > abs(ra[1] - ra[0]);
    float xA = swap ? rb[0] : ra[0], xB = swap ? rb[1] : ra[1], xC = swap ? rb[2] : ra[2];
    float yA = swap ? ra[0] : rb[0], yB = swap ? ra[1] : rb[1], yC = swap ? ra[2] : rb[2];
    float x1 = (xA + xC) / 2, x2 = xB;  // raw x at screen x=30 (points 1 and 3) and x=290
    float y1 = (yA + yB) / 2, y3 = yC;  // raw y at screen y=30 (points 1 and 2) and y=210
    if (fabsf(x2 - x1) < 400 || fabsf(y3 - y1) < 400) {
      displayMessage("Let's try that again", "The taps didn't line up with the crosses");
      delay(2000);
      continue;
    }
    // Targets in unrotated panel coordinates
    int PX1 = cfg.flip ? 319 - 30 : 30, PX2 = cfg.flip ? 319 - 290 : 290;
    int PY1 = cfg.flip ? 239 - 30 : 30, PY3 = cfg.flip ? 239 - 210 : 210;
    float sx = (x2 - x1) / float(PX2 - PX1), sy = (y3 - y1) / float(PY3 - PY1);
    cfg.calSwap = swap;
    cfg.calX0 = lroundf(x1 - PX1 * sx);
    cfg.calX1 = lroundf(x1 + (320 - PX1) * sx);
    cfg.calY0 = lroundf(y1 - PY1 * sy);
    cfg.calY1 = lroundf(y1 + (240 - PY1) * sy);
    cfg.calSet = true;
    saveConfig();

    // Test: dots should land under the finger
    tft.fillScreen(C_BG);
    tft.setTextFont(2);
    txt(tft, "Tap around to test - dots should land", 160, 40, TFT_WHITE, MC_DATUM, 0);
    txt(tft, "right under your finger", 160, 58, TFT_WHITE, MC_DATUM, 0);
    uiButton(4, 194, 150, 42, "Redo");
    uiButton(166, 194, 150, 42, "Done", C_GREEN);
    bool redo = false;
    for (;;) {
      TouchEvt e = waitTap();
      if (hit(e, 166, 190, 154, 50)) break;
      if (hit(e, 0, 190, 158, 50)) { redo = true; break; }
      tft.fillCircle(e.x, e.y, 3, C_ACCENT);
    }
    if (redo) continue;
    break;
  }
  takeTouch();
  dirty = true;
  needRedraw = true;
  return true;
}

// ---------- Dispatcher ----------
static void openFrom(UiScreen s, UiScreen parent) {
  parentOf[s] = parent;
  switch (s) {
    case UI_WIFI: wifiOpen(); break;
    case UI_TRIPS: tripsOpen(); break;
    case UI_SETTINGS: stOff = 0; go(s); break;
    default: go(s); break;
  }
}

void uiOpen(UiScreen s) { openFrom(s, UI_NONE); }

void uiLoop(const TouchEvt& in) {
  TouchEvt e = in;
  if (volSaveAt && (int32_t)(millis() - volSaveAt) >= 0) {
    volSaveAt = 0;
    saveConfig();
  }
  if (e.type == T_LONG) e.type = T_TAP;  // inside the UI, a long press is just a press
  if (e.type != T_NONE) lastAct = millis();
  if (scr != UI_HOME && millis() - lastAct > UI_TIMEOUT_MS) {
    uiClose();
    return;
  }
  if (wxUpdatedMask && (scr == UI_WX || scr == UI_HOME)) {
    xSemaphoreTake(dataMux, portMAX_DELAY);
    uint32_t m = wxUpdatedMask;
    wxUpdatedMask = 0;
    xSemaphoreGive(dataMux);
    int idx = activeTrip();
    if ((scr == UI_HOME && (m & (1u << HOME_BIT))) || (scr == UI_WX && idx >= 0 && (m & (1u << idx)))) dirty = true;
  }
  if (dirty) {
    dirty = false;
    switch (scr) {
      case UI_MENU: menuDraw(); break;
      case UI_WX: wxDraw(); break;
      case UI_HOME: homeDraw(); break;
      case UI_SETTINGS: settingsDraw(); break;
      case UI_WIFI: wifiDraw(); break;
      case UI_TRIPS: tripsDraw(); break;
      case UI_TRIP: tripDraw(); break;
      case UI_DATE: dateDraw(); break;
      case UI_GEO: geoDraw(); break;
      case UI_KB: kbDraw(); break;
      case UI_INFO: infoDraw(); break;
      default: break;
    }
  }
  if (scr == UI_HOME) homeTick();
  if (scr == UI_WX) wxTick();
  if (e.type == T_NONE) return;
  switch (scr) {
    case UI_MENU: menuTouch(e); break;
    case UI_WX: wxTouch(e); break;
    case UI_HOME: homeTouch(e); break;
    case UI_SETTINGS: settingsTouch(e); break;
    case UI_WIFI: wifiTouch(e); break;
    case UI_TRIPS: tripsTouch(e); break;
    case UI_TRIP: tripTouch(e); break;
    case UI_DATE: dateTouch(e); break;
    case UI_GEO: geoTouch(e); break;
    case UI_KB: kbTouch(e); break;
    case UI_INFO: if (backHit(e) || e.type == T_TAP) goBack(); break;
    default: break;
  }
}
