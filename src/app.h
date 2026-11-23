// Shared declarations for the Countdown Clock
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <vector>
#include <math.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define FW_VERSION "1.4.2"
#define FW_NAME "Countdown Clock"
#define FW_AUTHOR "jhollin1138"
#define MAX_EVENTS 6
#define MAX_IMAGES 80
#define WX_DAYS 5
#define HOME_BIT 31
#define HOSTNAME "countdown"
#define AP_NAME "Countdown-Setup"

// Weather for one location (destination or home)
struct Wx {
  bool valid = false;
  uint32_t fetched = 0;   // millis() of last attempt
  float temp = 0, feels = 0, hi = 0, lo = 0, wind = 0, uv = 0;
  int code = 0, humidity = 0;
  bool isDay = true;
  int32_t utcOff = 0;     // location's UTC offset in seconds (DST-aware)
  time_t sunrise = 0, sunset = 0;
  uint8_t days = 0;
  time_t dDate[WX_DAYS] = {};
  int16_t dCode[WX_DAYS] = {};
  float dHi[WX_DAYS] = {}, dLo[WX_DAYS] = {};
  int16_t dPop[WX_DAYS] = {};  // chance of precipitation, %
};

struct Event {
  String name;
  String target;        // "YYYY-MM-DDTHH:MM" in HOME local time
  String place;         // display name of destination
  float lat = NAN, lon = NAN;
  String img;           // "" = built-in sunset, else photo filename
  int shade = 1;        // text panel shading: 0 none, 1 light, 2 medium, 3 dark
  int ms = 9999;        // day-bucket at last milestone check (persisted)
  time_t epoch = 0;     // computed from target
  Wx wx;                // written by the weather task under dataMux
};

struct Config {
  String tz = "EST5EDT,M3.2.0,M11.1.0";
  bool metric = false;
  bool h24 = false;
  bool invert = false;
  bool flip = false;
  String mode = "nearest";   // "nearest" or "rotate"
  int rotateSec = 20;
  bool autoBright = true;
  int manualBright = 200;
  int minBright = 15;
  int maxBright = 255;
  int ldrBright = 50;        // raw sensor reading in a bright room
  int ldrDark = 1500;        // raw sensor reading in a dark room
  bool quietOn = false;
  String quietStart = "22:30";
  String quietEnd = "06:30";
  int quietLevel = 0;        // backlight PWM during quiet hours (0 = off)
  bool sound = true;         // false = muted
  int volume = 40;           // 0-100
  String soundMile = "builtin:chime";
  String soundArr = "builtin:fanfare";
  bool led = true;
  bool deskOn = false;        // desk clock mode was left on (restored after a restart)
  bool deskWhenEmpty = true;  // show the desk clock when there are no upcoming events
  String homeName;           // home location for the desk clock's weather
  float homeLat = NAN, homeLon = NAN;
  // Touch calibration: raw reading at screen edge 0 and at 320 (x) / 240 (y)
  bool calSet = false, calSwap = false;
  int calX0 = 200, calX1 = 3700, calY0 = 240, calY1 = 3800;
  std::vector<Event> events;
};

enum NetState { NET_CONNECTING, NET_PORTAL, NET_ONLINE };

extern Config cfg;
extern Wx homeWx;
extern SemaphoreHandle_t dataMux;
extern SemaphoreHandle_t netMux;
extern volatile uint32_t configGen;
extern volatile bool needRedraw;
extern volatile uint32_t wxUpdatedMask;
extern volatile NetState netState;
extern fs::FS* imgFS;
extern bool sdOK;
extern std::vector<String> images;

inline bool timeValid() { return time(nullptr) > 1700000000; }
// Whole days remaining; -1 once the target time has passed.
inline int dayBucket(time_t epoch, time_t now) {
  long long s = (long long)epoch - (long long)now;
  return s <= 0 ? -1 : (int)(s / 86400);
}

// config.cpp
bool loadConfig();
bool saveConfig();
void commitConfig(bool resetWeather);
void applyTZ();
String configToJson();
bool configFromJson(const String& in, Config& out, String& err);
time_t parseLocal(const String& s);
void sortEvents(std::vector<Event>& v);
String formatLocal(const struct tm& t);
void scanImages();
const char* imgDir();
String imgPath(const String& name);
bool imageExists(const String& name);

// hw.cpp
enum TouchType { T_NONE, T_TAP, T_LONG, T_SWIPE_L, T_SWIPE_R, T_SWIPE_U, T_SWIPE_D, T_CAL };
struct TouchEvt {
  TouchType type = T_NONE;
  int16_t x = 0, y = 0;    // screen position where the touch started
  int16_t ra = 0, rb = 0;  // averaged raw readings (used by calibration)
};
void hwBegin();
void hwLoop();
int ldrRaw();
int backlightLevel();
bool inQuietHours();
bool quietActive();
void ledEffect(int which, uint32_t durMs, bool force);  // 0 off, 1 rainbow, 2 party
TouchEvt takeTouch();

// audio.cpp
void audioBegin();
void audioPlay(const String& name);   // "builtin:..." or a file in soundDir()
void audioStop();
bool audioPlaying();
void audioEvent(int which, bool force);  // 0 milestone, 1 arrival
const char* soundDir();
String soundLabel(const String& id);
std::vector<String> builtinSounds();

// display.cpp
void displayBegin();
void applyDisplaySettings();
void displayMessage(const String& title, const String& l1, const String& l2 = "", const String& l3 = "");
void displayHold(uint32_t ms);
void displayProgress(const String& title, int pct);
void displayReadRect(int x, int y, int w, int h, uint16_t* buf);  // RGB565, high byte first
int nextEventIdx();  // soonest visible event, or -1
void displayLoop();
void celebrate(int which, bool test);
String activeName();
int activeTrip();
void showTrip(int idx);

// ui.cpp
enum UiScreen { UI_NONE, UI_MENU, UI_WX, UI_HOME, UI_SETTINGS, UI_WIFI, UI_TRIPS, UI_TRIP,
                UI_DATE, UI_GEO, UI_KB, UI_INFO };
void uiOpen(UiScreen s);
void uiOpenDeskAuto();  // desk clock shown because there are no upcoming events
void uiClose();
bool uiActive();
UiScreen uiScreen();
void uiLoop(const TouchEvt& e);
bool runCalibration();

// weather.cpp
struct GeoHit { String label, shortName; float lat, lon; };
void weatherBegin();
const char* wxText(int code);
bool geocode(const String& q, std::vector<GeoHit>& out, String& err);
String weatherStatus();  // "OK at 8:15 PM via HTTP" or the last error

// diag.cpp
void diagBegin();
void diagLoop();
String diagLastRestart();
String diagLog();

// web.cpp
void webBegin();
void webLoop();
