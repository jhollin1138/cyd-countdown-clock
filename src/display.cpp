// Main countdown screen: background + darkened text panels, flicker-free dynamic regions
#include "gfx.h"
#include <TJpg_Decoder.h>
#include <WiFi.h>

TFT_eSPI tft;
static TFT_eSprite spr(&tft);

struct Rect { int16_t x, y, w, h; };
struct Region { Rect r; uint16_t* buf; };

// Layout (320x240 landscape)
static const Rect R_TOP = {0, 0, 320, 26};     // trip name + home clock
static const Rect R_MID = {14, 32, 292, 142};  // days + h/m/s
static const Rect R_BOT = {0, 180, 320, 60};   // weather
static const Rect* PANELS[] = {&R_TOP, &R_MID, &R_BOT};

// Regions that change between full redraws. Their background pixels are cached
// during the full redraw so they can be repainted without flicker.
static Region RG_CLOCK = {{204, 0, 116, 26}, nullptr};
static Region RG_TIME = {{30, 136, 260, 34}, nullptr};
static Region RG_DEST = {{196, 199, 124, 18}, nullptr};
static Region* REGIONS[] = {&RG_CLOCK, &RG_TIME, &RG_DEST};
static const int SPR_W = 260, SPR_H = 34;

static int activeIdx = -1;
static Event cur;          // snapshot of the active event
static bool haveCur = false;
static bool sprOK = false;
static uint32_t manualUntil = 0, lastRotate = 0, infoUntil = 0, celebUntil = 0;
static int lastBucket = -99999, lastMinute = -1;
static uint16_t shadeMul = 179;  // text panel brightness multiplier (/256) for the current trip
static time_t lastTick = 0;
static bool waitMsgShown = false, portalDrawn = false;

// ---------- Shared drawing helpers ----------
void txt(TFT_eSPI& g, const String& s, int x, int y, uint16_t col, uint8_t datum, int sh) {
  g.setTextDatum(datum);
  if (sh) {
    g.setTextColor(TFT_BLACK);
    g.drawString(s, x + sh, y + sh);
  }
  g.setTextColor(col);
  g.drawString(s, x, y);
}

String fit(TFT_eSPI& g, String s, int maxW) {
  if (g.textWidth(s) <= maxW) return s;
  while (s.length() > 1 && g.textWidth(s + "..") > maxW) s.remove(s.length() - 1);
  return s + "..";
}

struct tm homeTm(time_t t) { struct tm r; localtime_r(&t, &r); return r; }
struct tm destTm(time_t t, int32_t off) { time_t x = t + off; struct tm r; gmtime_r(&x, &r); return r; }

String hhmm(const struct tm& t, bool shortAmPm) {
  char b[16];
  if (cfg.h24) snprintf(b, sizeof b, "%02d:%02d", t.tm_hour, t.tm_min);
  else {
    int h = t.tm_hour % 12;
    if (!h) h = 12;
    if (shortAmPm) snprintf(b, sizeof b, "%d:%02d%c", h, t.tm_min, t.tm_hour < 12 ? 'a' : 'p');
    else snprintf(b, sizeof b, "%d:%02d %s", h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  }
  return b;
}

int tempTxt(float v, int x, int y, int align, uint16_t col, uint8_t nf, uint8_t uf, bool unit) {
  String s = String((int)lroundf(v));
  String u = cfg.metric ? "C" : "F";
  tft.setTextFont(nf);
  int wn = tft.textWidth(s), fh = tft.fontHeight();
  int r = max(2, fh / 14);
  int wu = 0;
  if (unit) {
    tft.setTextFont(uf);
    wu = tft.textWidth(u) + 1;
  }
  int w = wn + 2 * r + 4 + wu;
  int left = align == 0 ? x : align == 1 ? x - w / 2 : x - w;
  tft.setTextFont(nf);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(col);
  tft.drawString(s, left, y);
  int cx = left + wn + r + 2, cy = y + r + 1 + fh / 16;
  tft.drawCircle(cx, cy, r, col);
  if (r > 2) tft.drawCircle(cx, cy, r - 1, col);
  if (unit) {
    tft.setTextFont(uf);
    tft.drawString(u, left + wn + 2 * r + 5, y);
  }
  return w;
}

// ---------- Weather icons (scalable, designed at 48x48) ----------
static float K = 1;
static inline int S(float v) { return (int)lroundf(v * K); }
static inline int S1(float v) { return max(1, S(v)); }

static void cloud(int x, int y, uint16_t c) {
  tft.fillCircle(x + S(12), y + S(14), S1(8), c);
  tft.fillCircle(x + S(23), y + S(9), S1(10), c);
  tft.fillCircle(x + S(33), y + S(14), S1(8), c);
  tft.fillRoundRect(x + S(4), y + S(13), S(38), max(2, S(10)), S1(5), c);
}
static void sun(int cx, int cy, int r) {
  for (int a = 0; a < 360; a += 45) {
    float t = a * DEG_TO_RAD;
    tft.drawLine(cx + cosf(t) * (r + S(3)), cy + sinf(t) * (r + S(3)),
                 cx + cosf(t) * (r + S(7)), cy + sinf(t) * (r + S(7)), TFT_YELLOW);
  }
  tft.fillCircle(cx, cy, r, TFT_YELLOW);
}
static void moon(int cx, int cy, int r) {
  tft.fillCircle(cx, cy, r, 0xEF7B);
  tft.fillCircle(cx - S(3), cy - S(2), S1(2), 0xBDF7);
  tft.fillCircle(cx + S(3), cy + S(3), S1(3), 0xBDF7);
}

void drawIcon(int x, int y, int size, int code, bool day) {
  K = size / 48.0f;
  if (code <= 1) {
    day ? sun(x + S(24), y + S(24), S(10)) : moon(x + S(24), y + S(24), S(11));
  } else if (code == 2) {
    day ? sun(x + S(16), y + S(15), S(7)) : moon(x + S(16), y + S(15), S(8));
    cloud(x + S(4), y + S(20), TFT_WHITE);
  } else if (code == 3) {
    cloud(x + S(2), y + S(6), C_DIM);
    cloud(x + S(4), y + S(16), TFT_WHITE);
  } else if (code == 45 || code == 48) {
    for (int i = 0; i < 4; i++)
      tft.fillRoundRect(x + S(4) + (i % 2) * S(4), y + S(12) + i * S(8), S(36), max(2, S(3)), 1, C_DIM);
  } else if (code >= 95) {
    cloud(x + S(3), y + S(4), 0x7BEF);
    tft.fillTriangle(x + S(24), y + S(26), x + S(16), y + S(38), x + S(23), y + S(38), TFT_YELLOW);
    tft.fillTriangle(x + S(22), y + S(36), x + S(29), y + S(36), x + S(19), y + S(47), TFT_YELLOW);
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    cloud(x + S(3), y + S(4), TFT_WHITE);
    for (int i = 0; i < 3; i++) tft.fillCircle(x + S(12) + i * S(11), y + S(34) + (i % 2) * S(6), S1(2), TFT_WHITE);
  } else {
    cloud(x + S(3), y + S(4), TFT_WHITE);
    for (int i = 0; i < 4; i++) {
      tft.drawLine(x + S(12) + i * S(8), y + S(31), x + S(8) + i * S(8), y + S(42), C_SKYBL);
      tft.drawLine(x + S(13) + i * S(8), y + S(31), x + S(9) + i * S(8), y + S(42), C_SKYBL);
    }
  }
}

// ---------- Background pipeline ----------
static inline bool inRect(int x, int y, const Rect& r) {
  return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}
// Panel brightness for shading presets: none, light, medium, dark
static const uint16_t SHADE_MUL[4] = {256, 179, 141, 96};  // 100%, 70%, 55%, 37.5%
static inline uint16_t darken(uint16_t c) {
  uint16_t r = (((c >> 11) & 31) * shadeMul) >> 8;
  uint16_t g = (((c >> 5) & 63) * shadeMul) >> 8;
  uint16_t b = ((c & 31) * shadeMul) >> 8;
  return (r << 11) | (g << 5) | b;
}

static void processBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  for (int j = 0; j < h; j++) {
    int py = y + j;
    if (py >= 240) break;
    for (int i = 0; i < w; i++) {
      int px = x + i;
      if (px >= 320) continue;
      uint16_t& c = bmp[j * w + i];
      if (shadeMul < 256)
        for (const Rect* p : PANELS)
          if (inRect(px, py, *p)) { c = darken(c); break; }
      for (Region* rg : REGIONS)
        if (rg->buf && inRect(px, py, rg->r))
          rg->buf[(py - rg->r.y) * rg->r.w + (px - rg->r.x)] = c;
    }
  }
  tft.pushImage(x, y, w, h, bmp);
}

static bool jpgCb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bmp) {
  if (y >= 240) return false;
  processBlock(x, y, w, h, bmp);
  return true;
}

static uint16_t lineBuf[320 * 8];

static uint16_t gradAt(int x, int y) {
  struct St { int y; float r, g, b; };
  static const St st[] = {{0, 16, 24, 72}, {90, 92, 40, 120}, {160, 240, 110, 70}, {240, 252, 196, 110}};
  int i = 0;
  while (i < 2 && y > st[i + 1].y) i++;
  float t = constrain(float(y - st[i].y) / (st[i + 1].y - st[i].y), 0.0f, 1.0f);
  float r = st[i].r + t * (st[i + 1].r - st[i].r);
  float g = st[i].g + t * (st[i + 1].g - st[i].g);
  float b = st[i].b + t * (st[i + 1].b - st[i].b);
  float dx = x - 235, dy = y - 150, d = sqrtf(dx * dx + dy * dy);
  if (d < 34) { r = 255; g = 236; b = 150; }
  else if (d < 70) {
    float k = (70 - d) / 36 * 0.5f;
    r += (255 - r) * k; g += (220 - g) * k; b += (140 - b) * k;
  }
  return tft.color565((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static void drawGradient() {
  for (int y = 0; y < 240; y += 8) {
    for (int j = 0; j < 8; j++)
      for (int x = 0; x < 320; x++) lineBuf[j * 320 + x] = gradAt(x, y + j);
    processBlock(0, y, 320, 8, lineBuf);
  }
}

static void fillBlack() {
  for (int y = 0; y < 240; y += 8) {
    memset(lineBuf, 0, sizeof lineBuf);
    processBlock(0, y, 320, 8, lineBuf);
  }
}

static bool drawJpeg(const String& name) {
  String p = imgPath(name);
  if (!imgFS->exists(p)) return false;
  uint16_t w = 0, h = 0;
  if (TJpgDec.getFsJpgSize(&w, &h, p.c_str(), *imgFS) != JDR_OK || !w || !h) return false;
  uint8_t s = 1;
  while (s < 8 && (w / s > 320 || h / s > 240)) s <<= 1;
  TJpgDec.setJpgScale(s);
  int dw = w / s, dh = h / s;
  if (dw < 320 || dh < 240) fillBlack();
  return TJpgDec.drawFsJpg((320 - dw) / 2, (240 - dh) / 2, p.c_str(), *imgFS) == JDR_OK;
}

// ---------- Event selection ----------
static bool visible(const Event& e, time_t now) { return e.epoch > 0 && now < e.epoch + 86400; }

static int nearest(time_t now) {
  int best = -1;
  for (size_t i = 0; i < cfg.events.size(); i++) {
    if (!visible(cfg.events[i], now)) continue;
    if (best < 0 || cfg.events[i].epoch < cfg.events[best].epoch) best = i;
  }
  return best;
}

static int nextVisible(int from, time_t now, int dir = 1) {
  int n = cfg.events.size();
  for (int k = 1; k <= n; k++) {
    int i = ((from + dir * k) % n + n) % n;
    if (visible(cfg.events[i], now)) return i;
  }
  return -1;
}

static void selectActive(time_t now) {
  int idx = activeIdx;
  if (idx >= (int)cfg.events.size()) idx = -1;
  bool valid = idx >= 0 && visible(cfg.events[idx], now);
  if (!(manualUntil && (int32_t)(millis() - manualUntil) < 0 && valid)) {
    manualUntil = 0;
    if (cfg.mode == "rotate") {
      if (!valid || millis() - lastRotate >= cfg.rotateSec * 1000UL) {
        idx = nextVisible(valid ? idx : -1, now);
        lastRotate = millis();
      }
    } else {
      idx = nearest(now);
    }
  }
  if (!(idx >= 0 && visible(cfg.events[idx], now))) idx = nearest(now);
  if (idx != activeIdx) {
    activeIdx = idx;
    needRedraw = true;
  }
}

static void stepEvent(int dir) {
  int n = nextVisible(activeIdx, time(nullptr), dir);
  if (n >= 0 && n != activeIdx) {
    activeIdx = n;
    manualUntil = millis() + 120000;
    lastRotate = millis();
    needRedraw = true;
  }
}

int activeTrip() {
  return (activeIdx >= 0 && activeIdx < (int)cfg.events.size()) ? activeIdx : -1;
}

void showTrip(int idx) {
  if (idx < 0 || idx >= (int)cfg.events.size()) return;
  activeIdx = idx;
  manualUntil = millis() + 120000;
  lastRotate = millis();
  needRedraw = true;
}

static String bgFor(const Event* e) {
  if (!e || !e->img.length()) return "";
  return imageExists(e->img) ? e->img : "";
}

// ---------- Full redraw ----------
static void drawDots(time_t now) {
  int n = 0;
  for (const Event& e : cfg.events) if (visible(e, now)) n++;
  if (n < 2) return;
  int x0 = 160 - (n - 1) * 6, k = 0;
  for (size_t i = 0; i < cfg.events.size(); i++) {
    if (!visible(cfg.events[i], now)) continue;
    int cx = x0 + k * 12;
    if ((int)i == activeIdx) tft.fillCircle(cx, 177, 3, TFT_WHITE);
    else tft.drawCircle(cx, 177, 3, TFT_WHITE);
    k++;
  }
}

static void drawWeather(const Event& e) {
  tft.setTextFont(2);
  if (isnan(e.lat)) {
    txt(tft, "Tap here to set a location for weather", 160, 210, C_DIM, MC_DATUM);
    return;
  }
  String place = fit(tft, e.place.length() ? e.place : String("Location"), 120);
  txt(tft, place, 316, 190, TFT_WHITE, MR_DATUM);
  const Wx& w = e.wx;
  if (!w.valid) {
    txt(tft, "Loading weather...", 8, 210, C_DIM, ML_DATUM);
    return;
  }
  drawIcon(4, 186, 48, w.code, w.isDay);

  tft.setTextFont(4);
  String t = String((int)lroundf(w.temp));
  txt(tft, t, 58, 184, TFT_WHITE, TL_DATUM);
  int xe = 58 + tft.textWidth(t);
  tft.drawCircle(xe + 6, 189, 3, TFT_WHITE);
  tft.drawCircle(xe + 6, 189, 2, TFT_WHITE);
  const char* unit = cfg.metric ? "C" : "F";
  txt(tft, unit, xe + 11, 184, TFT_WHITE, TL_DATUM);
  int xe2 = xe + 11 + tft.textWidth(unit);

  tft.setTextFont(2);
  char hl[24];
  snprintf(hl, sizeof hl, "H%d L%d", (int)lroundf(w.hi), (int)lroundf(w.lo));
  if (xe2 + 8 + tft.textWidth(hl) <= 190) txt(tft, hl, xe2 + 8, 190, C_DIM, TL_DATUM);
  txt(tft, fit(tft, wxText(w.code), 132), 58, 218, TFT_WHITE, TL_DATUM);

  if (w.sunrise && w.sunset) {
    String s = "Sun " + hhmm(destTm(w.sunrise, w.utcOff), true) + "-" + hhmm(destTm(w.sunset, w.utcOff), true);
    txt(tft, fit(tft, s, 124), 316, 226, C_DIM, MR_DATUM);
  }
}

static void noTripOverlay() {
  tft.setFreeFont(&FreeSansBold12pt7b);
  txt(tft, "No upcoming events", 160, 66, TFT_WHITE, MC_DATUM);
  tft.setTextFont(2);
  txt(tft, "Tap the screen to add one, or open", 160, 100, C_DIM, MC_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  txt(tft, "http://" HOSTNAME ".local", 160, 124, C_ACCENT, MC_DATUM);
  tft.setTextFont(2);
  txt(tft, WiFi.localIP().toString(), 160, 150, C_DIM, MC_DATUM);
}

static void fullRedraw(time_t now) {
  const Event* e = haveCur ? &cur : nullptr;
  shadeMul = SHADE_MUL[e ? constrain(e->shade, 0, 3) : 1];
  String bg = bgFor(e);
  if (!(bg.length() && drawJpeg(bg))) drawGradient();

  tft.setFreeFont(&FreeSansBold9pt7b);
  txt(tft, fit(tft, e ? e->name : String(FW_NAME), 194), 6, 13, TFT_WHITE, ML_DATUM);

  if (!e) {
    noTripOverlay();
    lastBucket = -99999;
    return;
  }
  int b = dayBucket(e->epoch, now);
  lastBucket = b;
  if (b >= 0) {
    tft.setTextFont(8);
    txt(tft, String(b), 160, 77, TFT_WHITE, MC_DATUM, 2);
    tft.setFreeFont(&FreeSansBold9pt7b);
    txt(tft, b == 1 ? "DAY TO GO" : "DAYS TO GO", 160, 124, C_ACCENT, MC_DATUM);
  } else {
    tft.setFreeFont(&FreeSansBold18pt7b);
    txt(tft, "It's here!", 160, 72, TFT_WHITE, MC_DATUM, 2);
    tft.setFreeFont(&FreeSansBold9pt7b);
    txt(tft, "Have an amazing time!", 160, 112, C_ACCENT, MC_DATUM);
  }
  drawDots(now);
  drawWeather(*e);
}

// ---------- Dynamic regions ----------
static bool beginRg(Region& rg) {
  if (!rg.buf || !sprOK) return false;
  spr.pushImage(0, 0, rg.r.w, rg.r.h, rg.buf);
  return true;
}
static void endRg(Region& rg) { spr.pushSprite(rg.r.x, rg.r.y, 0, 0, rg.r.w, rg.r.h); }

static void drawClock(time_t now) {
  if (!beginRg(RG_CLOCK)) return;
  spr.setFreeFont(&FreeSansBold9pt7b);
  txt(spr, hhmm(homeTm(now), false), RG_CLOCK.r.w - 6, 13, TFT_WHITE, MR_DATUM);
  if (WiFi.status() != WL_CONNECTED) spr.fillCircle(8, 13, 4, TFT_RED);
  endRg(RG_CLOCK);
}

static void drawTime(time_t now) {
  if (!haveCur || !beginRg(RG_TIME)) return;
  long long s = (long long)cur.epoch - now;
  bool past = s < 0;
  if (past) s = -s;
  int h = past ? (int)(s / 3600) : (int)((s % 86400) / 3600);
  int m = (s % 3600) / 60, sec = s % 60;
  char b[32];
  snprintf(b, sizeof b, "%s%02dh %02dm %02ds", past ? "+" : "", h, m, sec);
  spr.setTextFont(4);
  txt(spr, b, RG_TIME.r.w / 2, RG_TIME.r.h / 2, TFT_WHITE, MC_DATUM);
  endRg(RG_TIME);
}

static void drawDest(time_t now) {
  if (!haveCur || !cur.wx.valid || !beginRg(RG_DEST)) return;
  spr.setTextFont(2);
  txt(spr, "Local " + hhmm(destTm(now, cur.wx.utcOff), false), RG_DEST.r.w - 4, RG_DEST.r.h / 2, C_DIM, MR_DATUM);
  endRg(RG_DEST);
}

// ---------- Celebrations & milestones ----------
static const int MILES[] = {100, 60, 30, 14, 7, 3, 2, 1, 0};

static void showCelebration(const String& big, const String& small) {
  drawGradient();
  for (int k = 0; k < 80; k++)
    tft.fillCircle(random(320), random(240), random(1, 4),
                   tft.color565(random(128, 256), random(64, 256), random(64, 256)));
  tft.setFreeFont(&FreeSansBold18pt7b);
  txt(tft, big, 160, 90, TFT_WHITE, MC_DATUM, 2);
  tft.setFreeFont(&FreeSansBold12pt7b);
  txt(tft, fit(tft, small, 290), 160, 132, C_ACCENT, MC_DATUM);
}

static void celebrateEvent(int idx, int which, bool test) {
  if (!test && inQuietHours()) return;
  if (uiActive()) uiClose();
  bool ok = idx >= 0 && idx < (int)cfg.events.size();
  String name = ok ? cfg.events[idx].name : String("the big day");
  if (ok && !test) {
    activeIdx = idx;
    manualUntil = millis() + 120000;
  }
  if (which == 1) {
    showCelebration("IT'S TIME!", "Enjoy " + name + "!");
    audioEvent(1, test);
    ledEffect(2, 60000, test);
    celebUntil = millis() + 20000;
  } else {
    int d = ok ? dayBucket(cfg.events[idx].epoch, time(nullptr)) : 7;
    if (d < 0) d = 0;
    String big = d == 0 ? "ALMOST!" : String(d) + (d == 1 ? " DAY" : " DAYS");
    String small = d == 0 ? "Under 24 hours to " + name : "until " + name + "!";
    showCelebration(big, small);
    audioEvent(0, test);
    ledEffect(1, 15000, test);
    celebUntil = millis() + 8000;
  }
  needRedraw = true;
}

void celebrate(int which, bool test) { celebrateEvent(activeIdx, which, test); }

static void checkMilestones(time_t now) {
  bool changed = false;
  int fireIdx = -1, fireWhich = 0;
  for (size_t i = 0; i < cfg.events.size(); i++) {
    Event& e = cfg.events[i];
    if (!e.epoch) continue;
    int d = dayBucket(e.epoch, now);
    if (d >= e.ms) continue;
    if (d < 0) {
      if (e.ms >= 0 && now - e.epoch < 3600) { fireIdx = i; fireWhich = 1; }
    } else {
      for (int m : MILES)
        if (d <= m && m < e.ms) { fireIdx = i; fireWhich = 0; break; }
    }
    e.ms = d;
    changed = true;
  }
  if (changed) saveConfig();
  if (fireIdx >= 0) celebrateEvent(fireIdx, fireWhich, false);
}

// ---------- Public ----------
void displayMessage(const String& title, const String& l1, const String& l2, const String& l3) {
  tft.fillScreen(C_BG);
  tft.fillRect(0, 0, 320, 4, C_ACCENT);
  tft.setFreeFont(&FreeSansBold12pt7b);
  txt(tft, fit(tft, title, 300), 160, 56, TFT_WHITE, MC_DATUM, 0);
  tft.setFreeFont(&FreeSans9pt7b);
  txt(tft, fit(tft, l1, 300), 160, 110, C_DIM, MC_DATUM, 0);
  txt(tft, fit(tft, l2, 300), 160, 140, C_ACCENT, MC_DATUM, 0);
  txt(tft, fit(tft, l3, 300), 160, 170, C_DIM, MC_DATUM, 0);
  needRedraw = true;
}

void displayHold(uint32_t ms) { infoUntil = millis() + ms; }

void displayProgress(const String& title, int pct) {
  static int lastPct = -2;
  if (pct < 0 || lastPct < 0 || pct < lastPct) {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, 320, 4, C_ACCENT);
    tft.setFreeFont(&FreeSansBold12pt7b);
    txt(tft, title, 160, 80, TFT_WHITE, MC_DATUM, 0);
    tft.setTextFont(2);
    txt(tft, "Don't unplug the clock", 160, 170, C_DIM, MC_DATUM, 0);
    tft.drawRoundRect(30, 116, 260, 22, 6, C_DIM);
  }
  if (pct >= 0) {
    tft.fillRoundRect(33, 119, 254 * constrain(pct, 0, 100) / 100, 16, 4, C_ACCENT);
    tft.fillRect(130, 140, 60, 18, C_BG);
    tft.setTextFont(2);
    txt(tft, String(pct) + "%", 160, 149, TFT_WHITE, MC_DATUM, 0);
  }
  lastPct = pct;
  needRedraw = true;
}

String activeName() {
  return (activeIdx >= 0 && activeIdx < (int)cfg.events.size()) ? cfg.events[activeIdx].name : String("");
}

void applyDisplaySettings() {
  tft.setRotation(cfg.flip ? 3 : 1);
  tft.invertDisplay(cfg.invert);
  needRedraw = true;
}

void displayBegin() {
  tft.init();
  applyDisplaySettings();
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);
  TJpgDec.setCallback(jpgCb);
  for (Region* rg : REGIONS) rg->buf = (uint16_t*)malloc(rg->r.w * rg->r.h * 2);
  spr.setColorDepth(16);
  sprOK = spr.createSprite(SPR_W, SPR_H) != nullptr;
  if (sprOK) spr.setSwapBytes(true);
}

// Shown while the phone setup hotspot is running (no saved WiFi, or it couldn't connect)
static void portalLoop(const TouchEvt& te) {
  if (!portalDrawn || needRedraw) {
    needRedraw = false;
    portalDrawn = true;
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, 320, 4, C_ACCENT);
    tft.setFreeFont(&FreeSansBold12pt7b);
    txt(tft, "WiFi setup", 160, 28, TFT_WHITE, MC_DATUM, 0);
    tft.setTextFont(2);
    txt(tft, "From a phone or laptop:", 160, 58, C_DIM, MC_DATUM, 0);
    txt(tft, "1. Join WiFi: " AP_NAME, 160, 80, TFT_WHITE, MC_DATUM, 0);
    txt(tft, "2. Browse to http://192.168.4.1", 160, 98, TFT_WHITE, MC_DATUM, 0);
    txt(tft, "3. Configure WiFi (2.4 GHz)", 160, 116, TFT_WHITE, MC_DATUM, 0);
    txt(tft, "- or -", 160, 144, C_DIM, MC_DATUM, 0);
    uiButton(40, 164, 240, 46, "Set up on this screen", C_ACCENT, TFT_BLACK);
  }
  if (te.type == T_TAP && te.x >= 40 && te.x < 280 && te.y >= 160 && te.y < 214) uiOpen(UI_WIFI);
  else if (te.type == T_LONG) uiOpen(UI_MENU);
}

void displayLoop() {
  uint32_t ms = millis();
  TouchEvt te = takeTouch();
  if (te.type == T_CAL) {
    runCalibration();
    te.type = T_NONE;
  }
  if (uiActive()) {
    // The home clock can stay up for days, so keep milestone chimes working there
    if (uiScreen() == UI_HOME && timeValid() && time(nullptr) != lastTick) {
      lastTick = time(nullptr);
      checkMilestones(lastTick);
    }
    if (uiActive()) uiLoop(te);
    return;
  }
  if (netState == NET_PORTAL) {
    portalLoop(te);
    return;
  }
  portalDrawn = false;

  if (infoUntil || celebUntil) {
    if (te.type != T_NONE) {
      if (celebUntil) { audioStop(); ledEffect(0, 0, false); }
      infoUntil = celebUntil = 0;
      needRedraw = true;
      te.type = T_NONE;
    }
    if (infoUntil && (int32_t)(ms - infoUntil) >= 0) { infoUntil = 0; needRedraw = true; }
    if (celebUntil && (int32_t)(ms - celebUntil) >= 0) { celebUntil = 0; needRedraw = true; }
    if (infoUntil || celebUntil) return;
  }
  if (te.type == T_LONG) {
    uiOpen(UI_MENU);
    return;
  }

  if (!timeValid()) {
    if (!waitMsgShown) {
      waitMsgShown = true;
      displayMessage(FW_NAME, "Waiting for the time from the internet...",
                     "http://" HOSTNAME ".local", WiFi.localIP().toString());
    }
    return;
  }

  time_t now = time(nullptr);
  bool newSec = now != lastTick;
  if (newSec) {
    selectActive(now);
    checkMilestones(now);
    if (celebUntil) return;
  }

  // Touch zones on the countdown screen
  switch (te.type) {
    case T_SWIPE_L: stepEvent(1); break;
    case T_SWIPE_R: stepEvent(-1); break;
    case T_TAP:
      if (activeIdx < 0) { uiOpen(UI_TRIPS); return; }
      if (te.y < 30 && te.x >= 196) { uiOpen(UI_HOME); return; }
      if (te.y >= 180) { uiOpen(UI_WX); return; }
      stepEvent(1);
      break;
    default: break;
  }

  if (!newSec && !needRedraw && !wxUpdatedMask) return;
  lastTick = now;

  haveCur = false;
  xSemaphoreTake(dataMux, portMAX_DELAY);
  if (activeIdx >= 0 && activeIdx < (int)cfg.events.size()) {
    cur = cfg.events[activeIdx];
    haveCur = true;
    if (wxUpdatedMask & (1u << activeIdx)) needRedraw = true;
  }
  wxUpdatedMask = 0;
  xSemaphoreGive(dataMux);

  if (haveCur && dayBucket(cur.epoch, now) != lastBucket) needRedraw = true;
  if (needRedraw) {
    needRedraw = false;
    fullRedraw(now);
    lastMinute = -1;
  }
  struct tm t = homeTm(now);
  if (t.tm_min != lastMinute) {
    lastMinute = t.tm_min;
    drawClock(now);
    drawDest(now);
  }
  drawTime(now);
}
