// Settings web server at http://countdown.local
#include "app.h"
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <Update.h>
#include "webpage.h"

static WebServer server(80);
static File upFile;
static bool upOk = false;
static String upErr, upName;

static String sanitizeBase(const String& in, const char* fallback) {
  String base = in;
  int sl = base.lastIndexOf('/');
  if (sl >= 0) base = base.substring(sl + 1);
  int dot = base.lastIndexOf('.');
  if (dot > 0) base = base.substring(0, dot);
  base.toLowerCase();
  String o;
  for (unsigned i = 0; i < base.length() && o.length() < 24; i++) {
    char c = base[i];
    if (isalnum((unsigned char)c) || c == '_' || c == '-') o += c;
    else if (o.length() && o[o.length() - 1] != '-') o += '-';
  }
  while (o.endsWith("-")) o.remove(o.length() - 1);
  if (!o.length()) o = fallback;
  return o;
}

static String sanitizeName(const String& in) { return sanitizeBase(in, "photo") + ".jpg"; }

static void sendJson(JsonDocument& d) {
  String s;
  serializeJson(d, s);
  server.send(200, "application/json", s);
}

static void handleStatus() {
  JsonDocument d;
  d["fw"] = FW_VERSION;
  d["ip"] = WiFi.localIP().toString();
  d["ssid"] = WiFi.SSID();
  d["rssi"] = WiFi.RSSI();
  d["heap"] = ESP.getFreeHeap();
  d["maxBlock"] = ESP.getMaxAllocHeap();
  d["wx"] = weatherStatus();
  d["restart"] = diagLastRestart();
  d["up"] = millis() / 1000;
  d["ldr"] = ldrRaw();
  if (timeValid()) {
    time_t n = time(nullptr);
    struct tm t;
    localtime_r(&n, &t);
    char b[40];
    strftime(b, sizeof b, cfg.h24 ? "%a %b %d, %H:%M" : "%a %b %d, %I:%M %p", &t);
    d["time"] = b;
  } else {
    d["time"] = "time not set yet";
  }
  d["active"] = activeName();
  sendJson(d);
}

static void handleImages() {
  JsonDocument d;
  d["storage"] = sdOK ? "SD card" : "internal flash";
  uint64_t total = sdOK ? SD.totalBytes() : LittleFS.totalBytes();
  uint64_t used = sdOK ? SD.usedBytes() : LittleFS.usedBytes();
  d["free"] = (double)(total > used ? total - used : 0);
  JsonArray a = d["files"].to<JsonArray>();
  for (const String& n : images) a.add(n);
  sendJson(d);
}

static void handleSave() {
  Config nc;
  String err;
  if (!configFromJson(server.arg("plain"), nc, err)) {
    server.send(400, "text/plain", err);
    return;
  }

  // Dates are entered in the (possibly new) home time zone
  setenv("TZ", nc.tz.c_str(), 1);
  tzset();
  time_t now = time(nullptr);
  for (Event& e : nc.events) {
    e.epoch = parseLocal(e.target);
    if (!e.epoch) {
      applyTZ();
      server.send(400, "text/plain", "Invalid date/time for " + e.name);
      return;
    }
  }
  sortEvents(nc.events);  // keep events in date order

  bool dispChanged = nc.invert != cfg.invert || nc.flip != cfg.flip;
  // Touch calibration and desk clock state live on the clock itself; never take them from the web page
  nc.deskOn = cfg.deskOn;
  nc.calSet = cfg.calSet;
  nc.calSwap = cfg.calSwap;
  nc.calX0 = cfg.calX0;
  nc.calX1 = cfg.calX1;
  nc.calY0 = cfg.calY0;
  nc.calY1 = cfg.calY1;
  xSemaphoreTake(dataMux, portMAX_DELAY);
  for (Event& e : nc.events) {
    e.ms = dayBucket(e.epoch, now);  // new/changed trips don't chime for past milestones
    for (const Event& o : cfg.events) {
      if (o.name == e.name && o.target == e.target) { e.ms = o.ms; break; }
    }
    for (const Event& o : cfg.events) {
      if (o.wx.valid && !isnan(o.lat) && o.lat == e.lat && o.lon == e.lon && nc.metric == cfg.metric) {
        e.wx = o.wx;
        break;
      }
    }
  }
  bool homeSame = nc.metric == cfg.metric &&
                  ((isnan(nc.homeLat) && isnan(cfg.homeLat)) || (nc.homeLat == cfg.homeLat && nc.homeLon == cfg.homeLon));
  if (!homeSame) homeWx = Wx();
  cfg = nc;
  configGen++;
  xSemaphoreGive(dataMux);

  applyTZ();
  saveConfig();
  if (dispChanged) applyDisplaySettings();
  if (uiActive()) uiClose();  // on-screen editors may be showing stale data
  needRedraw = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleUploadChunk() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    upOk = true;
    upErr = "";
    upName = sanitizeName(u.filename);
    if (images.size() >= MAX_IMAGES && !imageExists(upName)) {
      upOk = false;
      upErr = "Too many images";
      return;
    }
    upFile = imgFS->open(imgPath(upName), FILE_WRITE);
    if (!upFile) { upOk = false; upErr = "Could not create file"; }
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (upOk && upFile.write(u.buf, u.currentSize) != u.currentSize) {
      upOk = false;
      upErr = "Storage is full";
    }
  } else if (u.status == UPLOAD_FILE_END) {
    if (upFile) upFile.close();
    if (!upOk) imgFS->remove(imgPath(upName));
    scanImages();
    needRedraw = true;
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (upFile) { upFile.close(); imgFS->remove(imgPath(upName)); }
    upOk = false;
    upErr = "Upload aborted";
  }
}

static void handleUploadDone() {
  if (upOk) server.send(200, "application/json", "{\"ok\":true,\"name\":\"" + upName + "\"}");
  else server.send(500, "text/plain", upErr.length() ? upErr : String("Upload failed"));
}

static void handleDelete() {
  String n = server.arg("name");
  if (!imageExists(n)) { server.send(404, "text/plain", "Not found"); return; }
  imgFS->remove(imgPath(n));
  scanImages();
  bool ch = false;
  for (Event& e : cfg.events)
    if (e.img == n) { e.img = ""; ch = true; }
  if (ch) saveConfig();
  needRedraw = true;
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---------- Sounds ----------
static String soundExt(const String& n) {
  String l = n;
  l.toLowerCase();
  int dot = l.lastIndexOf('.');
  return dot > 0 ? l.substring(dot + 1) : String("");
}

static void handleSounds() {
  JsonDocument d;
  JsonArray b = d["builtins"].to<JsonArray>();
  for (const String& id : builtinSounds()) {
    JsonObject o = b.add<JsonObject>();
    o["id"] = id;
    o["label"] = soundLabel(id);
  }
  JsonArray a = d["files"].to<JsonArray>();
  File dir = imgFS->open(soundDir());
  if (dir && dir.isDirectory()) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (f.isDirectory()) continue;
      String n = f.name();
      int sl = n.lastIndexOf('/');
      if (sl >= 0) n = n.substring(sl + 1);
      String ext = soundExt(n);
      if (ext != "mp3" && ext != "wav" && ext != "rtttl") continue;
      JsonObject o = a.add<JsonObject>();
      o["name"] = n;
      o["size"] = f.size();
    }
  }
  uint64_t total = sdOK ? SD.totalBytes() : LittleFS.totalBytes();
  uint64_t used = sdOK ? SD.usedBytes() : LittleFS.usedBytes();
  d["free"] = (double)(total > used ? total - used : 0);
  d["storage"] = sdOK ? "SD card" : "internal flash";
  sendJson(d);
}

static File sndFile;
static bool sndOk;
static String sndErr, sndName;

static void handleSoundChunk() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    sndOk = true;
    sndErr = "";
    String ext = soundExt(u.filename);
    if (ext != "mp3" && ext != "wav") {
      sndOk = false;
      sndErr = "Only MP3 and WAV files are supported";
      return;
    }
    sndName = sanitizeBase(u.filename, "sound") + "." + ext;
    sndFile = imgFS->open(String(soundDir()) + "/" + sndName, FILE_WRITE);
    if (!sndFile) { sndOk = false; sndErr = "Could not create file"; }
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (sndOk && sndFile.write(u.buf, u.currentSize) != u.currentSize) {
      sndOk = false;
      sndErr = "Storage is full";
    }
  } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
    if (sndFile) sndFile.close();
    if (u.status == UPLOAD_FILE_ABORTED) { sndOk = false; sndErr = "Upload aborted"; }
    if (!sndOk && sndName.length()) imgFS->remove(String(soundDir()) + "/" + sndName);
  }
}

static void handleSoundDone() {
  if (sndOk) server.send(200, "application/json", "{\"ok\":true,\"name\":\"" + sndName + "\"}");
  else server.send(500, "text/plain", sndErr.length() ? sndErr : String("Upload failed"));
}

static void handleRtttl() {
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain"))) { server.send(400, "text/plain", "Bad request"); return; }
  String text = d["text"] | "";
  text.trim();
  text.replace("\r", "");
  text.replace("\n", "");
  int c1 = text.indexOf(':'), c2 = c1 >= 0 ? text.indexOf(':', c1 + 1) : -1;
  if (c1 < 0 || c2 < 0 || text.length() > 4000) {
    server.send(400, "text/plain", "That doesn't look like an RTTTL tune (name:d=4,o=5,b=100:notes...)");
    return;
  }
  String name = d["name"] | "";
  if (!name.length()) name = text.substring(0, c1);
  String fn = sanitizeBase(name, "tune") + ".rtttl";
  File f = imgFS->open(String(soundDir()) + "/" + fn, FILE_WRITE);
  if (!f || f.print(text) != text.length()) {
    if (f) f.close();
    server.send(500, "text/plain", "Storage is full");
    return;
  }
  f.close();
  server.send(200, "application/json", "{\"ok\":true,\"name\":\"" + fn + "\"}");
}

static bool validSound(const String& n) {
  if (n.startsWith("builtin:")) {
    for (const String& b : builtinSounds()) if (b == n) return true;
    return false;
  }
  if (n.indexOf('/') >= 0 || n.startsWith(".")) return false;
  return imgFS->exists(String(soundDir()) + "/" + n);
}

static void handleSoundDelete() {
  String n = server.arg("name");
  if (n.startsWith("builtin:") || !validSound(n)) { server.send(404, "text/plain", "Not found"); return; }
  audioStop();
  delay(30);
  imgFS->remove(String(soundDir()) + "/" + n);
  bool ch = false;
  if (cfg.soundMile == n) { cfg.soundMile = Config().soundMile; ch = true; }
  if (cfg.soundArr == n) { cfg.soundArr = Config().soundArr; ch = true; }
  if (ch) saveConfig();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleSoundPlay() {
  String n = server.arg("name");
  if (!validSound(n)) { server.send(404, "text/plain", "Not found"); return; }
  if (server.hasArg("vol")) cfg.volume = constrain(server.arg("vol").toInt(), 0, 100);
  audioPlay(n);
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---------- Firmware update ----------
static bool fwOk;
static String fwErr;

static void handleFwChunk() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    fwOk = true;
    fwErr = "";
    audioStop();
    if (uiActive()) uiClose();
    displayProgress("Updating firmware", 0);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      fwOk = false;
      fwErr = String("Can't start update: ") + Update.errorString();
    }
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (!fwOk) return;
    if (u.totalSize == 0 && u.currentSize > 0 && u.buf[0] != 0xE9) {
      fwOk = false;
      fwErr = "That isn't an ESP32 firmware file (use firmware.bin)";
      Update.abort();
      return;
    }
    if (Update.write(u.buf, u.currentSize) != u.currentSize) {
      fwOk = false;
      fwErr = String("Write failed: ") + Update.errorString();
      Update.abort();
      return;
    }
    int len = server.clientContentLength();
    if (len > 0) displayProgress("Updating firmware", (int)((u.totalSize + u.currentSize) * 100LL / len));
  } else if (u.status == UPLOAD_FILE_END) {
    if (fwOk && !Update.end(true)) {
      fwOk = false;
      fwErr = String("Update failed: ") + Update.errorString();
    }
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    fwOk = false;
    fwErr = "Upload aborted";
  }
}

static void handleFwDone() {
  if (fwOk) {
    displayProgress("Updating firmware", 100);
    server.send(200, "application/json", "{\"ok\":true}");
    displayMessage("Update complete", "Restarting...");
    delay(1000);
    ESP.restart();
  } else {
    server.send(500, "text/plain", fwErr.length() ? fwErr : String("Update failed"));
    displayMessage("Update failed", fwErr);
    displayHold(8000);
  }
}

// Raw screen capture: 320x240 RGB565, high byte first. The settings page turns it into a PNG.
static void handleShot() {
  static uint16_t rows[320 * 4];
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(320 * 240 * 2);
  server.send(200, "application/octet-stream", "");
  for (int y = 0; y < 240; y += 4) {
    displayReadRect(0, y, 320, 4, rows);
    server.sendContent((const char*)rows, sizeof rows);
  }
}

static void handleImg() {
  String n = server.arg("name");
  if (!imageExists(n)) { server.send(404, "text/plain", "Not found"); return; }
  File f = imgFS->open(imgPath(n), "r");
  if (!f) { server.send(404, "text/plain", "Not found"); return; }
  server.sendHeader("Cache-Control", "max-age=3600");
  server.streamFile(f, "image/jpeg");
  f.close();
}

void webBegin() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/config", HTTP_GET, [] {
    if (server.hasArg("download"))
      server.sendHeader("Content-Disposition", "attachment; filename=\"countdown-backup.json\"");
    server.send(200, "application/json", configToJson());
  });
  server.on("/api/sounds", HTTP_GET, handleSounds);
  server.on("/api/sound/upload", HTTP_POST, handleSoundDone, handleSoundChunk);
  server.on("/api/sound/rtttl", HTTP_POST, handleRtttl);
  server.on("/api/sound/delete", HTTP_POST, handleSoundDelete);
  server.on("/api/sound/play", HTTP_POST, handleSoundPlay);
  server.on("/api/sound/stop", HTTP_POST, [] { audioStop(); server.send(200, "application/json", "{\"ok\":true}"); });
  server.on("/api/firmware", HTTP_POST, handleFwDone, handleFwChunk);
  server.on("/api/screenshot", HTTP_GET, handleShot);
  server.on("/api/restarts", HTTP_GET, [] { server.send(200, "text/plain", diagLog()); });
  server.on("/api/config", HTTP_POST, handleSave);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/images", HTTP_GET, handleImages);
  server.on("/api/upload", HTTP_POST, handleUploadDone, handleUploadChunk);
  server.on("/api/image/delete", HTTP_POST, handleDelete);
  server.on("/img", HTTP_GET, handleImg);
  server.on("/api/test", HTTP_POST, [] {
    int w = server.arg("w").toInt();
    server.send(200, "application/json", "{\"ok\":true}");
    celebrate(w == 1 ? 1 : 0, true);
  });
  server.on("/api/reboot", HTTP_POST, [] {
    server.send(200, "application/json", "{\"ok\":true}");
    delay(400);
    ESP.restart();
  });
  server.on("/api/wifireset", HTTP_POST, [] {
    server.send(200, "application/json", "{\"ok\":true}");
    delay(400);
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
  });
  server.onNotFound([] {
    if (server.uri().startsWith("/api/")) { server.send(404, "text/plain", "Not found"); return; }
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
}

void webLoop() { server.handleClient(); }
