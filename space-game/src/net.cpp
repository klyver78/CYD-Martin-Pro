// Wi-Fi (Improv from the web flasher) + OTA update from GitHub Pages.
// Same NVS keys as the other CYD projects (namespace "wifi": ssid / pass).
#include "net.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ImprovWiFiLibrary.h>
#include "board.h"
#include "game.h"
#include "ota_certs.h"

static ImprovWiFi  improv(&Serial);
static Preferences prefs;
static String      ssid, pass;
static void (*redraw)() = nullptr;

static void status(const char* s) {
  strlcpy(gStatusText, s, sizeof gStatusText);
  Serial.printf("[net] %s\n", s);
  if (redraw) redraw();
}

static void onImprov(const char* s, const char* p) {
  ssid = s; pass = p;
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  status("Wi-Fi saved");
}

void netSetup(const char* fwName, const char* version, void (*redrawFn)()) {
  redraw = redrawFn;
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  improv.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32, fwName, version, "CYD Star Ring");
  improv.onImprovConnected(onImprov);
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    strlcpy(gStatusText, "No Wi-Fi - set it in the web flasher", sizeof gStatusText);
  }
}

void netLoop() { improv.handleSerial(); }

static long semverNum(const String& v) {
  int a = 0, b = 0, c = 0;
  sscanf(v.c_str(), "%d.%d.%d", &a, &b, &c);
  return (long)a * 1000000L + (long)b * 1000L + c;
}

static bool ensureOnline() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (!ssid.length()) return false;
  status("Connecting to Wi-Fi...");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) { improv.handleSerial(); delay(100); }
  return WiFi.status() == WL_CONNECTED;
}

static void ensureTime() {
  if (time(nullptr) > 1700000000) return;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < 8000) delay(100);
}

static void progress(size_t done, size_t total) {
  static int last = -1;
  int pct = total ? (int)(done * 100ULL / total) : 0;
  if (pct / 5 == last) return;
  last = pct / 5;
  char t[40]; snprintf(t, sizeof t, "Updating... %d%% - don't unplug", pct);
  status(t);
}

void netCheckAndUpdate(const char* runningVersion) {
  if (!ssid.length()) { status("No Wi-Fi - set it in the web flasher"); return; }
  if (!ensureOnline()) { status("Wi-Fi not connected"); return; }
  status("Checking for update...");
  ensureTime();                                   // cert validation needs the real time
  String body;
  {
    WiFiClientSecure sc;
    sc.setCACert(OTA_ROOT_CAS);
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(10000);
    if (http.begin(sc, String(ROMS_URL) + "?t=" + millis())) {
      if (http.GET() == HTTP_CODE_OK) body = http.getString();
      http.end();
    }
  }
  JsonDocument doc;
  if (!body.length() || deserializeJson(doc, body)) { status("Could not reach GitHub Pages"); return; }
  String latest = doc["version"] | "";
  if (semverNum(latest) <= semverNum(runningVersion)) {
    char t[48]; snprintf(t, sizeof t, "Up to date (v%s)", runningVersion);
    status(t);
    return;
  }
  char t[48]; snprintf(t, sizeof t, "Installing v%s...", latest.c_str());
  status(t);
  Update.onProgress(progress);
  WiFiClientSecure sc;
  sc.setCACert(OTA_ROOT_CAS);
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = httpUpdate.update(sc, String(PAGES_BASE) + OTA_BIN_PATH);
  if (r != HTTP_UPDATE_OK) {
    snprintf(t, sizeof t, "Update failed: %s", httpUpdate.getLastErrorString().c_str());
    status(t);
  }
}
