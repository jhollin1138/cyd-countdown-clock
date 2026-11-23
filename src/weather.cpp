// Weather and place search from Open-Meteo (free, no API key). Weather is fetched on core 0.
#include "app.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

volatile uint32_t wxUpdatedMask = 0;
SemaphoreHandle_t netMux;

// Open-Meteo refreshes current conditions every 15 minutes, so polling faster gains nothing
static const uint32_t WX_REFRESH_MS = 15UL * 60 * 1000;
static const uint32_t WX_RETRY_MS = 2UL * 60 * 1000;

const char* wxText(int c) {
  switch (c) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Heavy drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorms";
    case 96: case 99: return "T-storms, hail";
    default: return "-";
  }
}

static void fillFilter(JsonObject f) {
  f["utc_offset_seconds"] = true;
  JsonObject fc = f["current"].to<JsonObject>();
  fc["temperature_2m"] = true;
  fc["apparent_temperature"] = true;
  fc["relative_humidity_2m"] = true;
  fc["weather_code"] = true;
  fc["wind_speed_10m"] = true;
  fc["is_day"] = true;
  JsonObject fd = f["daily"].to<JsonObject>();
  fd["time"] = true;
  fd["weather_code"] = true;
  fd["temperature_2m_max"] = true;
  fd["temperature_2m_min"] = true;
  fd["precipitation_probability_max"] = true;
  fd["uv_index_max"] = true;
  fd["sunrise"] = true;
  fd["sunset"] = true;
}

static bool parseWx(JsonObject o, Wx& out) {
  JsonObject cur = o["current"];
  JsonObject dly = o["daily"];
  int32_t off = o["utc_offset_seconds"] | 0;
  if (!cur["temperature_2m"].is<float>() || off < -16 * 3600 || off > 16 * 3600) return false;
  out.utcOff = off;
  out.temp = cur["temperature_2m"] | 0.0f;
  out.feels = cur["apparent_temperature"] | out.temp;
  out.humidity = cur["relative_humidity_2m"] | 0;
  out.code = cur["weather_code"] | 0;
  out.wind = cur["wind_speed_10m"] | 0.0f;
  out.isDay = (cur["is_day"] | 1) != 0;
  out.hi = dly["temperature_2m_max"][0] | out.temp;
  out.lo = dly["temperature_2m_min"][0] | out.temp;
  out.uv = dly["uv_index_max"][0] | 0.0f;
  out.sunrise = (time_t)(dly["sunrise"][0] | 0LL);
  out.sunset = (time_t)(dly["sunset"][0] | 0LL);
  JsonArray days = dly["time"];
  out.days = min((int)days.size(), WX_DAYS);
  for (int i = 0; i < out.days; i++) {
    out.dDate[i] = (time_t)(dly["time"][i] | 0LL);
    out.dCode[i] = dly["weather_code"][i] | 0;
    out.dHi[i] = dly["temperature_2m_max"][i] | 0.0f;
    out.dLo[i] = dly["temperature_2m_min"][i] | 0.0f;
    out.dPop[i] = dly["precipitation_probability_max"][i] | -1;
  }
  return true;
}

struct Loc { float lat, lon; int slot; };  // slot = event index, or HOME_BIT

// ---------- Network helper and status ----------
static String lastErr, lastVia;
static time_t lastOkAt = 0, lastErrAt = 0;

// GET a JSON document from Open-Meteo. Tries plain HTTP first: the data is public, and skipping
// TLS saves ~40 KB of memory and a slow handshake. Falls back to HTTPS if HTTP fails.
// Call with netMux held.
static bool getJson(const char* host, const String& path, JsonDocument& filter, JsonDocument& doc, String& err) {
  for (int secure = 0; secure < 2; secure++) {
    WiFiClient plain;
    WiFiClientSecure tls;
    if (secure) tls.setInsecure();  // public, read-only data; skips certificate bundle management
    HTTPClient http;
    http.useHTTP10(true);  // avoid chunked encoding so the JSON can be streamed
    http.setTimeout(12000);
    String url = String(secure ? "https://" : "http://") + host + path;
    bool begun = secure ? http.begin(tls, url) : http.begin(plain, url);
    if (!begun) {
      err = "couldn't start request";
      continue;
    }
    int code = http.GET();
    if (code != 200) {
      err = code < 0 ? HTTPClient::errorToString(code) : "HTTP " + String(code);
      http.end();
      continue;
    }
    // Read the whole body first, pausing whenever no data is waiting. Parsing straight from the
    // network stream busy-waits (Stream::timedRead never yields), which starves the idle task on
    // this core and trips the 5-second task watchdog when the server is slow.
    std::vector<char> body;
    int len = http.getSize();  // -1 when the server doesn't say
    body.reserve(len > 0 ? len : 8192);
    WiFiClient* st = http.getStreamPtr();
    uint32_t last = millis();
    bool tooBig = false;
    char buf[512];
    while (millis() - last < 12000) {
      int avail = st->available();
      if (avail > 0) {
        int n = st->read((uint8_t*)buf, min(avail, (int)sizeof buf));
        if (n > 0) {
          body.insert(body.end(), buf, buf + n);
          last = millis();
          if (body.size() > 60000) { tooBig = true; break; }
          if (len > 0 && (int)body.size() >= len) break;
        }
      } else if (!st->connected()) {
        break;
      } else {
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    http.end();
    if (tooBig || body.empty()) {
      err = tooBig ? "response too large" : "no data received";
      continue;
    }
    DeserializationError de = deserializeJson(doc, body.data(), body.size(), DeserializationOption::Filter(filter));
    if (de) {
      err = String("bad data: ") + de.c_str();
      continue;
    }
    lastVia = secure ? "HTTPS" : "HTTP";
    return true;
  }
  err += " (free " + String(ESP.getFreeHeap() / 1024) + " KB, largest " + String(ESP.getMaxAllocHeap() / 1024) + " KB)";
  return false;
}

String weatherStatus() {
  xSemaphoreTake(dataMux, portMAX_DELAY);
  time_t okAt = lastOkAt, errAt = lastErrAt;
  String err = lastErr, via = lastVia;
  xSemaphoreGive(dataMux);
  auto at = [](time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    char b[16];
    strftime(b, sizeof b, cfg.h24 ? "%H:%M" : "%I:%M %p", &tm);
    return String(b[0] == '0' ? b + 1 : b);
  };
  if (!okAt && !errAt) return "not fetched yet";
  if (errAt > okAt) return "failed at " + at(errAt) + ": " + err;
  return "OK at " + at(okAt) + " via " + via;
}

// Fetch weather for several places in one request (Open-Meteo returns a list).
// ok[i] says whether out[i] was filled.
static void fetchMany(const std::vector<Loc>& locs, bool metric, std::vector<Wx>& out, std::vector<bool>& ok) {
  size_t n = locs.size();
  out.assign(n, Wx());
  ok.assign(n, false);
  if (!n) return;
  String lats, lons, tzs;
  char b[16];
  for (size_t i = 0; i < n; i++) {
    if (i) { lats += ','; lons += ','; tzs += ','; }
    snprintf(b, sizeof b, "%.4f", locs[i].lat); lats += b;
    snprintf(b, sizeof b, "%.4f", locs[i].lon); lons += b;
    tzs += "auto";  // each place gets its own time zone
  }
  String path = "/v1/forecast?latitude=" + lats + "&longitude=" + lons +
               "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,uv_index_max,sunrise,sunset"
               "&timezone=" + tzs + "&forecast_days=" + String(WX_DAYS) + "&timeformat=unixtime" +
               (metric ? "" : "&temperature_unit=fahrenheit&wind_speed_unit=mph");

  JsonDocument filter;
  fillFilter(n > 1 ? filter[0].to<JsonObject>() : filter.to<JsonObject>());  // [0] applies to every list item
  JsonDocument doc;
  String err;
  xSemaphoreTake(netMux, portMAX_DELAY);
  bool got = getJson("api.open-meteo.com", path, filter, doc, err);
  xSemaphoreGive(netMux);
  if (got) {
    if (n == 1) ok[0] = parseWx(doc.as<JsonObject>(), out[0]);
    else {
      JsonArray arr = doc.as<JsonArray>();
      for (size_t i = 0; i < n && i < arr.size(); i++) ok[i] = parseWx(arr[i], out[i]);
    }
    if (!ok[0]) err = "unexpected response";
  }
  xSemaphoreTake(dataMux, portMAX_DELAY);
  if (got && ok[0]) lastOkAt = time(nullptr);
  else { lastErr = err; lastErrAt = time(nullptr); }
  xSemaphoreGive(dataMux);
}

static void wxTask(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (WiFi.status() != WL_CONNECTED) continue;

    std::vector<Loc> locs;
    bool metric = false, anyStale = false;
    uint32_t gen;
    xSemaphoreTake(dataMux, portMAX_DELAY);
    gen = configGen;
    metric = cfg.metric;
    uint32_t now = millis();
    auto stale = [now](const Wx& w) {
      return w.fetched == 0 || (now - w.fetched) > (w.valid ? WX_REFRESH_MS : WX_RETRY_MS);
    };
    for (size_t i = 0; i < cfg.events.size(); i++) {
      const Event& e = cfg.events[i];
      if (isnan(e.lat)) continue;
      locs.push_back({e.lat, e.lon, (int)i});
      anyStale |= stale(e.wx);
    }
    if (!isnan(cfg.homeLat)) {
      locs.push_back({cfg.homeLat, cfg.homeLon, HOME_BIT});
      anyStale |= stale(homeWx);
    }
    xSemaphoreGive(dataMux);
    if (!anyStale) continue;  // when anything is due, refresh everything in one request

    std::vector<Wx> res;
    std::vector<bool> ok;
    fetchMany(locs, metric, res, ok);
    // If the combined request failed, fall back to one request per place
    bool none = true;
    for (bool k : ok) none &= !k;
    if (none && locs.size() > 1) {
      for (size_t i = 0; i < locs.size(); i++) {
        std::vector<Wx> r1;
        std::vector<bool> ok1;
        fetchMany({locs[i]}, metric, r1, ok1);
        if (ok1[0]) { res[i] = r1[0]; ok[i] = true; }
      }
    }

    xSemaphoreTake(dataMux, portMAX_DELAY);
    if (gen == configGen) {
      uint32_t t = millis();
      if (!t) t = 1;
      for (size_t i = 0; i < locs.size(); i++) {
        int slot = locs[i].slot;
        if (slot != HOME_BIT && slot >= (int)cfg.events.size()) continue;
        Wx& w = slot == HOME_BIT ? homeWx : cfg.events[slot].wx;
        if (ok[i]) {
          res[i].valid = true;
          res[i].fetched = t;
          w = res[i];
          wxUpdatedMask |= (1u << slot);
        } else {
          // retry in ~2 minutes whether or not we already had data
          w.fetched = w.valid ? (t - WX_REFRESH_MS + WX_RETRY_MS) : t;
          if (!w.fetched) w.fetched = 1;
        }
      }
    }
    xSemaphoreGive(dataMux);
  }
}

void weatherBegin() {
  xTaskCreatePinnedToCore(wxTask, "wx", 12288, nullptr, 1, nullptr, 0);
}

static String urlEncode(const String& s) {
  String o;
  char b[4];
  for (unsigned i = 0; i < s.length(); i++) {
    unsigned char c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else {
      snprintf(b, sizeof b, "%%%02X", c);
      o += b;
    }
  }
  return o;
}

// Place-name search used by the on-screen trip editor and home location setting.
bool geocode(const String& q, std::vector<GeoHit>& out, String& err) {
  out.clear();
  if (WiFi.status() != WL_CONNECTED) { err = "Not connected to WiFi"; return false; }
  JsonDocument filter;
  JsonObject f = filter["results"][0].to<JsonObject>();
  f["name"] = true;
  f["admin1"] = true;
  f["country"] = true;
  f["latitude"] = true;
  f["longitude"] = true;
  JsonDocument doc;
  String e;
  xSemaphoreTake(netMux, portMAX_DELAY);
  bool got = getJson("geocoding-api.open-meteo.com",
                     "/v1/search?count=5&language=en&format=json&name=" + urlEncode(q), filter, doc, e);
  xSemaphoreGive(netMux);
  if (!got) {
    err = "Search failed: " + e;
    return false;
  }
  for (JsonObject r : doc["results"].as<JsonArray>()) {
    GeoHit h;
    String name = r["name"] | "";
    String a1 = r["admin1"] | "";
    String ctry = r["country"] | "";
    h.shortName = a1.length() ? name + ", " + a1 : name;
    h.label = h.shortName + (ctry.length() ? ", " + ctry : String(""));
    h.lat = r["latitude"] | 0.0f;
    h.lon = r["longitude"] | 0.0f;
    if (name.length()) out.push_back(h);
  }
  return true;
}
