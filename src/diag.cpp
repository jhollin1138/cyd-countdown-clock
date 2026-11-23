// Restart diagnostics: why the clock last restarted, and where it crashed if it did.
// The ESP32 core saves a crash dump to the "coredump" flash partition; we summarize it at boot,
// keep a short log in /restarts.txt (LittleFS), then erase the dump so the next crash is fresh.
#include "app.h"
#include <LittleFS.h>
#include <esp_system.h>
#include <esp_core_dump.h>

static const char* LOG_PATH = "/restarts.txt";
static const int LOG_KEEP = 10;
static String reasonShort, entry;  // entry is written to the log once the clock knows the time
static bool logged = false;

static const char* reasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart (update, settings or WiFi change)";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "froze (interrupt watchdog)";
    case ESP_RST_TASK_WDT: return "froze (task watchdog)";
    case ESP_RST_WDT: return "froze (watchdog)";
    case ESP_RST_BROWNOUT: return "power dip (brownout)";
    case ESP_RST_EXT: return "reset button";
    case ESP_RST_DEEPSLEEP: return "wake from sleep";
    default: return "unknown";
  }
}

static const char* causeText(uint32_t c) {
  switch (c) {
    case 0: return "IllegalInstruction";
    case 2: return "InstructionFetchError";
    case 3: return "LoadStoreError";
    case 6: return "IntegerDivideByZero";
    case 9: return "LoadStoreAlignment";
    case 20: return "InstFetchProhibited";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    default: return "other";
  }
}

void diagBegin() {
  esp_reset_reason_t r = esp_reset_reason();
  reasonShort = reasonText(r);
  entry = reasonShort;

  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) == ESP_OK) {
    esp_core_dump_summary_t* sm = (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
    if (sm && esp_core_dump_get_summary(sm) == ESP_OK) {
      char b[64];
      String t = " | task " + String(sm->exc_task);
      snprintf(b, sizeof b, " | %s at 0x%08x", causeText(sm->ex_info.exc_cause), (unsigned)sm->exc_pc);
      t += b;
      snprintf(b, sizeof b, " addr 0x%08x", (unsigned)sm->ex_info.exc_vaddr);
      t += b;
      t += " | backtrace";
      for (uint32_t i = 0; i < sm->exc_bt_info.depth && i < 16; i++) {
        snprintf(b, sizeof b, " 0x%08x", (unsigned)sm->exc_bt_info.bt[i]);
        t += b;
      }
      if (sm->exc_bt_info.corrupted) t += " (corrupted)";
      t += " | elf ";
      t += (const char*)sm->app_elf_sha256;
      entry += t;
    }
    free(sm);
    esp_core_dump_image_erase();
  }
  entry = "v" FW_VERSION " | " + entry;
  Serial.printf("Restart reason: %s\n", entry.c_str());
}

// Once the time is known, log the restart with an approximate timestamp
void diagLoop() {
  if (logged || !timeValid()) return;
  logged = true;
  time_t boot = time(nullptr) - millis() / 1000;
  struct tm t;
  localtime_r(&boot, &t);
  char ts[24];
  strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", &t);
  String line = String(ts) + " | " + entry;

  std::vector<String> lines;
  File f = LittleFS.open(LOG_PATH, "r");
  if (f) {
    while (f.available()) {
      String l = f.readStringUntil('\n');
      l.trim();
      if (l.length()) lines.push_back(l);
    }
    f.close();
  }
  lines.push_back(line);
  while ((int)lines.size() > LOG_KEEP) lines.erase(lines.begin());
  f = LittleFS.open(LOG_PATH, "w");
  if (f) {
    for (const String& l : lines) f.println(l);
    f.close();
  }
}

String diagLastRestart() { return reasonShort; }

String diagLog() {
  File f = LittleFS.open(LOG_PATH, "r");
  if (!f) return "No restarts logged yet.\n";
  String s = f.readString();
  f.close();
  return s;
}
