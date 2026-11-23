// Countdown Clock — ESP32-2432S028R ("Cheap Yellow Display")
#include "app.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

SemaphoreHandle_t dataMux;
volatile uint32_t configGen = 0;
volatile bool needRedraw = true;
volatile NetState netState = NET_CONNECTING;
fs::FS* imgFS = &LittleFS;
bool sdOK = false;
std::vector<String> images;

static SPIClass sdSPI(VSPI);  // display is on HSPI, touch is bit-banged
static WiFiManager wm;
static bool servicesStarted = false;

// Called once WiFi is connected (at boot, from the phone portal, or from the on-screen WiFi tool)
static void goOnline() {
  netState = NET_ONLINE;
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  Serial.printf("Connected to %s, IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  if (!servicesStarted) {
    servicesStarted = true;
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.nist.gov");
    webBegin();
    weatherBegin();
  }
  if (!uiActive()) {
    displayMessage("Connected!", "Settings: http://" HOSTNAME ".local",
                   "http://" + WiFi.localIP().toString(), "Hold the screen for the menu");
    displayHold(6000);
  }
}

void setup() {
  Serial.begin(115200);
  dataMux = xSemaphoreCreateMutex();
  netMux = xSemaphoreCreateMutex();

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  diagBegin();
  loadConfig();
  hwBegin();
  displayBegin();

  sdSPI.begin(18, 19, 23, 5);
  sdOK = SD.begin(5, sdSPI, 20000000);
  imgFS = sdOK ? (fs::FS*)&SD : (fs::FS*)&LittleFS;
  scanImages();
  Serial.printf("Images on %s: %u\n", sdOK ? "SD" : "flash", (unsigned)images.size());
  imgFS->mkdir(soundDir());
  audioBegin();

  if (!cfg.calSet) runCalibration();  // first boot: line up the touch screen

  displayMessage(FW_NAME, "Connecting to WiFi...", "", "by " FW_AUTHOR);
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  wm.setConfigPortalBlocking(false);  // keep the touch screen usable while the phone portal runs
  wm.setConfigPortalTimeout(0);
  wm.setConnectTimeout(20);
  if (wm.autoConnect(AP_NAME)) {
    goOnline();
  } else {
    netState = NET_PORTAL;
    needRedraw = true;
  }
}

void loop() {
  if (netState == NET_PORTAL) {
    wm.process();
    if (WiFi.status() == WL_CONNECTED) {
      if (wm.getConfigPortalActive()) wm.stopConfigPortal();
      goOnline();
    }
  } else if (netState == NET_ONLINE) {
    webLoop();
  }
  hwLoop();
  displayLoop();
  diagLoop();
  delay(2);
}
