// Board hardware: backlight, light sensor, touch, speaker, RGB LED
#include "app.h"

// ESP32-2432S028R pin map
#define PIN_BL     21
#define PIN_LDR    34
// GPIO26 (speaker amplifier) is driven by the built-in DAC in audio.cpp
#define PIN_LED_R  4     // RGB LED is active LOW
#define PIN_LED_G  16
#define PIN_LED_B  17
#define T_CLK      25    // XPT2046 touch (bit-banged so the SD card keeps the VSPI bus)
#define T_MOSI     32
#define T_MISO     39
#define T_CS       33
#define T_IRQ      36

// LEDC channels (backlight on its own timer)
#define CH_BL   0   // timer 0
#define CH_R    4   // timer 2
#define CH_G    5   // timer 2
#define CH_B    6   // timer 3

#define PRESS_MIN 400   // touch pressure threshold

static int ldrSmooth = -1;
static int curBL = 0;
static uint32_t wakeUntil = 0;
static TouchEvt pending;

// ---------- Light sensor / quiet hours / backlight ----------
int ldrRaw() { return ldrSmooth < 0 ? 0 : ldrSmooth; }
int backlightLevel() { return curBL; }

static int hm(const String& s) {
  int h = 0, m = 0;
  sscanf(s.c_str(), "%d:%d", &h, &m);
  return h * 60 + m;
}

bool inQuietHours() {
  if (!cfg.quietOn || !timeValid()) return false;
  time_t n = time(nullptr);
  struct tm t;
  localtime_r(&n, &t);
  int now = t.tm_hour * 60 + t.tm_min;
  int a = hm(cfg.quietStart), b = hm(cfg.quietEnd);
  if (a == b) return false;
  return a < b ? (now >= a && now < b) : (now >= a || now < b);
}

bool quietActive() { return inQuietHours() && (int32_t)(millis() - wakeUntil) >= 0; }

static void backlightLoop() {
  static uint32_t last = 0, lastLdr = 0;
  uint32_t ms = millis();
  if (ms - lastLdr >= 250) {
    lastLdr = ms;
    int r = analogRead(PIN_LDR);
    ldrSmooth = ldrSmooth < 0 ? r : (ldrSmooth * 7 + r) / 8;
  }
  if (ms - last < 20) return;
  last = ms;

  int target;
  if (quietActive()) {
    target = cfg.quietLevel;
  } else if (cfg.autoBright) {
    float span = float(cfg.ldrBright - cfg.ldrDark);
    float f = span == 0 ? 1.0f : (ldrSmooth - cfg.ldrDark) / span;
    f = constrain(f, 0.0f, 1.0f);
    target = cfg.minBright + int(f * (cfg.maxBright - cfg.minBright));
  } else {
    target = cfg.manualBright;
  }
  target = constrain(target, 0, 255);
  if (curBL != target) {
    int d = target - curBL;
    int step = max(1, abs(d) / 10);
    curBL += d > 0 ? min(step, d) : max(-step, d);
    ledcWrite(CH_BL, curBL);
  }
}

// ---------- Touch (XPT2046, bit-banged) ----------
static uint16_t xptRead(uint8_t cmd) {
  digitalWrite(T_CS, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(T_MOSI, (cmd >> i) & 1);
    digitalWrite(T_CLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(T_CLK, LOW);
    delayMicroseconds(1);
  }
  uint16_t r = 0;
  for (int i = 15; i >= 0; i--) {
    digitalWrite(T_CLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(T_CLK, LOW);
    delayMicroseconds(1);
    r |= digitalRead(T_MISO) << i;
  }
  digitalWrite(T_CS, HIGH);
  return r >> 4;
}

static int touchPressure() {
  int z1 = xptRead(0xB1);
  int z2 = xptRead(0xC1);
  xptRead(0x90);  // PD=00: leave PENIRQ enabled
  return z1 + 4095 - z2;
}

static int med3(int a, int b, int c) { return max(min(a, b), min(max(a, b), c)); }

// a = channel 0x91, b = channel 0xD1 (which one is screen X depends on the panel)
static void readRaw(int& a, int& b) {
  int as[3], bs[3];
  for (int i = 0; i < 3; i++) {
    as[i] = xptRead(0x91);
    bs[i] = xptRead(0xD1);
  }
  xptRead(0x90);
  a = med3(as[0], as[1], as[2]);
  b = med3(bs[0], bs[1], bs[2]);
}

static void mapXY(int a, int b, int& x, int& y) {
  int rx = cfg.calSwap ? b : a, ry = cfg.calSwap ? a : b;
  long dx = cfg.calX1 - cfg.calX0, dy = cfg.calY1 - cfg.calY0;
  x = dx ? (int)((long)(rx - cfg.calX0) * 320 / dx) : 0;
  y = dy ? (int)((long)(ry - cfg.calY0) * 240 / dy) : 0;
  x = constrain(x, 0, 319);
  y = constrain(y, 0, 239);
  if (cfg.flip) {
    x = 319 - x;
    y = 239 - y;
  }
}

static void emit(TouchType t, int x, int y, int ra, int rb) {
  bool wasDark = quietActive();
  wakeUntil = millis() + 30000;
  if (wasDark && t != T_CAL) return;  // first touch in quiet hours only wakes the screen
  pending.type = t;
  pending.x = x;
  pending.y = y;
  pending.ra = ra;
  pending.rb = rb;
}

TouchEvt takeTouch() {
  TouchEvt e = pending;
  pending.type = T_NONE;
  return e;
}

static void touchLoop() {
  static uint32_t last = 0, downAt = 0;
  static bool down = false, longFired = false, calFired = false;
  static uint8_t cnt = 0;
  static int sx, sy, lx, ly, n;
  static long sumA, sumB;
  if (millis() - last < 20) return;
  last = millis();

  bool raw = touchPressure() > PRESS_MIN;
  cnt = raw ? (cnt < 3 ? cnt + 1 : 3) : 0;

  if (cnt >= 2) {
    int a, b, x, y;
    readRaw(a, b);
    if (touchPressure() <= PRESS_MIN) return;  // finger lifted mid-read: discard sample
    mapXY(a, b, x, y);
    if (!down) {
      down = true;
      downAt = millis();
      longFired = calFired = false;
      sx = lx = x;
      sy = ly = y;
      sumA = sumB = 0;
      n = 0;
    } else {
      lx = x;
      ly = y;
    }
    sumA += a;
    sumB += b;
    n++;
    bool still = abs(lx - sx) < 25 && abs(ly - sy) < 25;
    uint32_t held = millis() - downAt;
    if (!longFired && still && held > 900) {
      longFired = true;
      emit(T_LONG, sx, sy, sumA / n, sumB / n);
    }
    if (!calFired && still && held > 8000) {
      calFired = true;
      emit(T_CAL, sx, sy, 0, 0);
    }
  } else if (down && !raw) {
    down = false;
    if (longFired || !n) return;
    int dx = lx - sx, dy = ly - sy;
    if (abs(dx) > 50 && abs(dx) * 2 > abs(dy) * 3) emit(dx < 0 ? T_SWIPE_L : T_SWIPE_R, sx, sy, 0, 0);
    else if (abs(dy) > 45 && abs(dy) * 2 > abs(dx) * 3) emit(dy < 0 ? T_SWIPE_U : T_SWIPE_D, sx, sy, 0, 0);
    else emit(T_TAP, sx, sy, sumA / n, sumB / n);
  }
}

// ---------- RGB LED ----------
static int ledMode = 0;
static uint32_t ledUntil = 0;

static void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  ledcWrite(CH_R, 255 - r);
  ledcWrite(CH_G, 255 - g);
  ledcWrite(CH_B, 255 - b);
}

static void hsvRGB(int h, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t x = (h % 60) * v / 60;
  switch (h / 60) {
    case 0: r = v; g = x; b = 0; break;
    case 1: r = v - x; g = v; b = 0; break;
    case 2: r = 0; g = v; b = x; break;
    case 3: r = 0; g = v - x; b = v; break;
    case 4: r = x; g = 0; b = v; break;
    default: r = v; g = 0; b = v - x; break;
  }
}

void ledEffect(int which, uint32_t durMs, bool force) {
  if (which && !cfg.led && !force) return;
  ledMode = which;
  ledUntil = millis() + durMs;
  if (!which) setRGB(0, 0, 0);
}

static void ledLoop() {
  static uint32_t last = 0;
  if (!ledMode || millis() - last < 20) return;
  last = millis();
  if ((int32_t)(millis() - ledUntil) >= 0) {
    ledMode = 0;
    setRGB(0, 0, 0);
    return;
  }
  if (ledMode == 1) {
    uint8_t r, g, b;
    hsvRGB((millis() / 8) % 360, 90, r, g, b);
    setRGB(r, g, b);
  } else {
    static const uint8_t cols[6][3] = {{255, 0, 0}, {0, 0, 0}, {0, 255, 0}, {0, 0, 0}, {0, 0, 255}, {255, 255, 255}};
    const uint8_t* c = cols[(millis() / 120) % 6];
    setRGB(c[0], c[1], c[2]);
  }
}

// ---------- Setup / loop ----------
void hwBegin() {
  ledcSetup(CH_BL, 5000, 8);
  ledcAttachPin(PIN_BL, CH_BL);
  curBL = cfg.autoBright ? 180 : cfg.manualBright;
  ledcWrite(CH_BL, curBL);

  ledcSetup(CH_R, 5000, 8); ledcAttachPin(PIN_LED_R, CH_R);
  ledcSetup(CH_G, 5000, 8); ledcAttachPin(PIN_LED_G, CH_G);
  ledcSetup(CH_B, 5000, 8); ledcAttachPin(PIN_LED_B, CH_B);
  setRGB(0, 0, 0);


  pinMode(PIN_LDR, INPUT);
  analogSetPinAttenuation(PIN_LDR, ADC_0db);  // the CYD's LDR circuit gives a small voltage

  pinMode(T_CLK, OUTPUT);
  pinMode(T_MOSI, OUTPUT);
  pinMode(T_CS, OUTPUT);
  digitalWrite(T_CS, HIGH);
  digitalWrite(T_CLK, LOW);
  pinMode(T_MISO, INPUT);
  pinMode(T_IRQ, INPUT);
}

void hwLoop() {
  touchLoop();
  backlightLoop();
  ledLoop();
}
