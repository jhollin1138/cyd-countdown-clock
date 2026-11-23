// Settings storage (LittleFS /config.json) and image library
#include "app.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <algorithm>

Config cfg;
Wx homeWx;
static const char* CFG_PATH = "/config.json";
static const char* CFG_TMP = "/config.tmp";

void applyTZ() {
  setenv("TZ", cfg.tz.c_str(), 1);
  tzset();
}

// Parse "YYYY-MM-DDTHH:MM" as local time in the current TZ.
time_t parseLocal(const String& s) {
  int Y = 0, M = 0, D = 0, h = 0, m = 0;
  int n = sscanf(s.c_str(), "%d-%d-%dT%d:%d", &Y, &M, &D, &h, &m);
  if (n < 3 || Y < 2020 || M < 1 || M > 12 || D < 1 || D > 31) return 0;
  struct tm t = {};
  t.tm_year = Y - 1900;
  t.tm_mon = M - 1;
  t.tm_mday = D;
  t.tm_hour = h;
  t.tm_min = m;
  t.tm_isdst = -1;
  return mktime(&t);
}

// Events are kept in date order (soonest first); events without a valid date go last.
void sortEvents(std::vector<Event>& v) {
  std::stable_sort(v.begin(), v.end(), [](const Event& a, const Event& b) {
    if (!a.epoch || !b.epoch) return a.epoch != 0 && b.epoch == 0;
    return a.epoch < b.epoch;
  });
}

String formatLocal(const struct tm& t) {
  char b[40];
  snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
  return b;
}

static String jstr(JsonVariantConst v, const String& def) {
  return v.is<const char*>() ? String(v.as<const char*>()) : def;
}
static int jint(JsonVariantConst v, int def, int lo, int hi) {
  return (v.is<int>() || v.is<float>()) ? constrain(v.as<int>(), lo, hi) : def;
}
static bool jbool(JsonVariantConst v, bool def) {
  return v.is<bool>() ? v.as<bool>() : def;
}
static String hhmmOr(JsonVariantConst v, const String& def) {
  String s = jstr(v, def);
  int h, m;
  return (sscanf(s.c_str(), "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) ? s : def;
}

bool configFromJson(const String& in, Config& c, String& err) {
  JsonDocument d;
  DeserializationError e = deserializeJson(d, in);
  if (e) { err = String("Bad JSON: ") + e.c_str(); return false; }
  c = Config();
  c.tz = jstr(d["tz"], c.tz); c.tz.trim();
  if (!c.tz.length()) c.tz = Config().tz;
  c.metric = jbool(d["metric"], c.metric);
  c.h24 = jbool(d["h24"], c.h24);
  c.invert = jbool(d["invert"], c.invert);
  c.flip = jbool(d["flip"], c.flip);
  c.mode = jstr(d["mode"], c.mode) == "rotate" ? "rotate" : "nearest";
  c.rotateSec = jint(d["rotateSec"], c.rotateSec, 5, 3600);
  c.autoBright = jbool(d["autoBright"], c.autoBright);
  c.manualBright = jint(d["manualBright"], c.manualBright, 1, 255);
  c.minBright = jint(d["minBright"], c.minBright, 0, 255);
  c.maxBright = jint(d["maxBright"], c.maxBright, 0, 255);
  c.ldrBright = jint(d["ldrBright"], c.ldrBright, 0, 4095);
  c.ldrDark = jint(d["ldrDark"], c.ldrDark, 0, 4095);
  c.quietOn = jbool(d["quietOn"], c.quietOn);
  c.quietStart = hhmmOr(d["quietStart"], c.quietStart);
  c.quietEnd = hhmmOr(d["quietEnd"], c.quietEnd);
  c.quietLevel = jint(d["quietLevel"], c.quietLevel, 0, 255);
  c.sound = jbool(d["sound"], c.sound);
  c.volume = jint(d["volume"], c.volume, 0, 100);
  c.soundMile = jstr(d["soundMile"], c.soundMile);
  c.soundArr = jstr(d["soundArr"], c.soundArr);
  if (!c.soundMile.length()) c.soundMile = Config().soundMile;
  if (!c.soundArr.length()) c.soundArr = Config().soundArr;
  c.led = jbool(d["led"], c.led);
  c.deskOn = jbool(d["deskOn"], false);
  c.deskWhenEmpty = jbool(d["deskWhenEmpty"], true);
  c.homeName = jstr(d["homeName"], "");
  if (c.homeName.length() > 48) c.homeName = c.homeName.substring(0, 48);
  if (d["homeLat"].is<float>() && d["homeLon"].is<float>()) {
    c.homeLat = d["homeLat"].as<float>();
    c.homeLon = d["homeLon"].as<float>();
  }
  c.calSet = jbool(d["calSet"], false);
  c.calSwap = jbool(d["calSwap"], false);
  c.calX0 = jint(d["calX0"], c.calX0, -30000, 30000);
  c.calX1 = jint(d["calX1"], c.calX1, -30000, 30000);
  c.calY0 = jint(d["calY0"], c.calY0, -30000, 30000);
  c.calY1 = jint(d["calY1"], c.calY1, -30000, 30000);
  if (c.calX0 == c.calX1 || c.calY0 == c.calY1) c.calSet = false;

  for (JsonObjectConst o : d["events"].as<JsonArrayConst>()) {
    if (c.events.size() >= MAX_EVENTS) break;
    Event ev;
    ev.name = jstr(o["name"], "Event");
    ev.name.trim();
    if (!ev.name.length()) ev.name = "Event";
    if (ev.name.length() > 32) ev.name = ev.name.substring(0, 32);
    ev.target = jstr(o["target"], "");
    ev.place = jstr(o["place"], "");
    if (ev.place.length() > 48) ev.place = ev.place.substring(0, 48);
    if (o["lat"].is<float>() && o["lon"].is<float>()) {
      ev.lat = o["lat"].as<float>();
      ev.lon = o["lon"].as<float>();
    }
    ev.img = jstr(o["img"], "");
    if (ev.img == "*") ev.img = "";  // slideshow was removed in v1.2
    ev.shade = jint(o["shade"], 1, 0, 3);
    ev.ms = jint(o["ms"], 9999, -1, 99999);
    c.events.push_back(ev);
  }
  return true;
}

String configToJson() {
  JsonDocument d;
  d["tz"] = cfg.tz;
  d["metric"] = cfg.metric;
  d["h24"] = cfg.h24;
  d["invert"] = cfg.invert;
  d["flip"] = cfg.flip;
  d["mode"] = cfg.mode;
  d["rotateSec"] = cfg.rotateSec;
  d["autoBright"] = cfg.autoBright;
  d["manualBright"] = cfg.manualBright;
  d["minBright"] = cfg.minBright;
  d["maxBright"] = cfg.maxBright;
  d["ldrBright"] = cfg.ldrBright;
  d["ldrDark"] = cfg.ldrDark;
  d["quietOn"] = cfg.quietOn;
  d["quietStart"] = cfg.quietStart;
  d["quietEnd"] = cfg.quietEnd;
  d["quietLevel"] = cfg.quietLevel;
  d["sound"] = cfg.sound;
  d["volume"] = cfg.volume;
  d["soundMile"] = cfg.soundMile;
  d["soundArr"] = cfg.soundArr;
  d["led"] = cfg.led;
  d["deskOn"] = cfg.deskOn;
  d["deskWhenEmpty"] = cfg.deskWhenEmpty;
  d["homeName"] = cfg.homeName;
  if (!isnan(cfg.homeLat)) { d["homeLat"] = cfg.homeLat; d["homeLon"] = cfg.homeLon; }
  d["calSet"] = cfg.calSet;
  d["calSwap"] = cfg.calSwap;
  d["calX0"] = cfg.calX0;
  d["calX1"] = cfg.calX1;
  d["calY0"] = cfg.calY0;
  d["calY1"] = cfg.calY1;
  JsonArray a = d["events"].to<JsonArray>();
  for (const Event& e : cfg.events) {
    JsonObject o = a.add<JsonObject>();
    o["name"] = e.name;
    o["target"] = e.target;
    o["place"] = e.place;
    if (!isnan(e.lat)) { o["lat"] = e.lat; o["lon"] = e.lon; }
    o["img"] = e.img;
    o["shade"] = e.shade;
    o["ms"] = e.ms;
  }
  String out;
  serializeJson(d, out);
  return out;
}

bool loadConfig() {
  if (LittleFS.exists(CFG_PATH)) {
    File f = LittleFS.open(CFG_PATH, "r");
    String s = f.readString();
    f.close();
    String err;
    Config c;
    if (configFromJson(s, c, err)) cfg = c;
    else Serial.printf("Config parse failed: %s\n", err.c_str());
  }
  applyTZ();
  for (Event& e : cfg.events) e.epoch = parseLocal(e.target);
  sortEvents(cfg.events);
  return true;
}

bool saveConfig() {
  File f = LittleFS.open(CFG_TMP, "w");
  if (!f) return false;
  f.print(configToJson());
  f.close();
  LittleFS.remove(CFG_PATH);
  return LittleFS.rename(CFG_TMP, CFG_PATH);
}

// Save a change made on the clock itself. resetWeather refetches all weather (e.g. units changed).
void commitConfig(bool resetWeather) {
  xSemaphoreTake(dataMux, portMAX_DELAY);
  if (resetWeather) {
    for (Event& e : cfg.events) e.wx = Wx();
    homeWx = Wx();
  }
  configGen++;
  xSemaphoreGive(dataMux);
  saveConfig();
  needRedraw = true;
}

const char* imgDir() { return sdOK ? "/countdown" : "/img"; }
String imgPath(const String& n) { return String(imgDir()) + "/" + n; }

bool imageExists(const String& n) {
  for (const String& s : images) if (s == n) return true;
  return false;
}

void scanImages() {
  images.clear();
  File d = imgFS->open(imgDir());
  if (!d || !d.isDirectory()) {
    imgFS->mkdir(imgDir());
    return;
  }
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (f.isDirectory()) continue;
    String n = f.name();
    int sl = n.lastIndexOf('/');
    if (sl >= 0) n = n.substring(sl + 1);
    String l = n;
    l.toLowerCase();
    if ((l.endsWith(".jpg") || l.endsWith(".jpeg")) && !n.startsWith(".") && images.size() < MAX_IMAGES)
      images.push_back(n);
  }
  std::sort(images.begin(), images.end(), [](const String& a, const String& b) { return a < b; });
}
