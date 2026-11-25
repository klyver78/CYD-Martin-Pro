// Wi-Fi (Improv from the web flasher), the price API, and OTA from GitHub Pages.
// Same NVS keys as the other CYD projects (namespace "wifi": ssid / pass).
//
// Every fetch here is blocking: the responses are a few KB and the UI shows a blinking
// indicator while one is running, which is simpler than threading an async client through
// the whole app. `redraw` is called on every status change so the screen keeps up.
#include "net.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ImprovWiFiLibrary.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "api_certs.h"
#include "board.h"
#include "jsonlite.h"
#include "portfolio.h"
#include "ui.h"

static ImprovWiFi  improv(&Serial);
static Preferences prefs;
static String      ssid, pass;
static void (*redraw)() = nullptr;

static void status(const char* s) {
  uiSetStatus(s);
  Serial.printf("[net] %s\n", s);
  if (redraw) redraw();
}
static void statusf(const char* fmt, ...) {
  char buf[72];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  status(buf);
}

static void onImprov(const char* s, const char* p) {
  ssid = s;
  pass = p;
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
  improv.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32, fwName, version, "CYD Portfolio Tracker");
  improv.onImprovConnected(onImprov);
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    uiSetStatus("No Wi-Fi - set it in the web flasher");
  }
}

void netLoop() { improv.handleSerial(); }

bool netOnline() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (!ssid.length()) {
    status("No Wi-Fi - set it in the web flasher");
    return false;
  }
  status("Connecting to Wi-Fi...");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    improv.handleSerial();
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    status("Wi-Fi not connected");
    return false;
  }
  return true;
}

// Certificate validation needs a plausible wall clock.
static void ensureTime() {
  if (time(nullptr) > 1700000000) return;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < 8000) delay(100);
}

// One HTTPS GET. Returns an empty String on any failure (the callers all tolerate that).
static String httpsGet(const String& url) {
  ensureTime();
  WiFiClientSecure sc;
  sc.setCACert(API_ROOT_CAS);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(12000);
  http.setUserAgent("cyd-portfolio-tracker");
  if (!http.begin(sc, url)) return String();
  int code = http.GET();
  String body;
  if (code == HTTP_CODE_OK) body = http.getString();
  else Serial.printf("[net] HTTP %d for %s\n", code, url.c_str());
  http.end();
  return body;
}

// ===================================================================== portfolio.json
bool netLoadPortfolio() {
  if (!netOnline()) return false;
  status("Loading portfolio.json...");
  String body = httpsGet(String(PORTFOLIO_URL) + "?t=" + millis());
  if (!body.length()) {
    status("Could not load portfolio.json");
    return false;
  }
  if (!pfParsePortfolio(body.c_str())) {
    status("portfolio.json has no usable holdings");
    return false;
  }
  statusf("%d holdings, base %s", pf.nh, pf.base);
  return true;
}

// ===================================================================== symbol / currency
// Both lookups are cached in NVS, so this is a first-boot cost only. NVS keys are capped at
// 15 characters, hence the truncation.
static void cacheKey(char* out, char tag, const char* value) {
  out[0] = tag;
  size_t i = 0;
  for (; value[i] && i < 14; ++i) out[i + 1] = value[i];
  out[i + 1] = 0;
}

bool netResolveSymbols() {
  bool allOk = true;
  prefs.begin("pftrack", false);

  for (int i; (i = pfFirstUnresolved()) >= 0;) {
    Holding& h = pf.h[i];
    char key[16];
    cacheKey(key, 's', h.key);
    String cached = prefs.getString(key, "");
    if (cached.length()) {
      snprintf(h.symbol, sizeof h.symbol, "%s", cached.c_str());
      continue;
    }
    if (!netOnline()) { allOk = false; break; }
    statusf("Looking up %s...", h.key);
    String body = httpsGet(String("https://" API_HOST API_SEARCH_PATH) + h.key);
    char sym[20] = {0};
    if (!body.length() || !pfParseSearch(body.c_str(), sym, sizeof sym)) {
      statusf("No symbol found for %s", h.key);
      snprintf(h.symbol, sizeof h.symbol, "%s", h.key);   // stop retrying this one
      allOk = false;
      continue;
    }
    snprintf(h.symbol, sizeof h.symbol, "%s", sym);
    prefs.putString(key, sym);
    delay(400);                                           // the search endpoint rate-limits
  }

  for (int i; (i = pfFirstUnknownCurrency()) >= 0;) {
    Holding& h = pf.h[i];
    char ck[16], nk[16];
    cacheKey(ck, 'c', h.symbol);
    cacheKey(nk, 'n', h.symbol);
    String cached = prefs.getString(ck, "");
    if (cached.length()) {
      snprintf(h.cur, sizeof h.cur, "%s", cached.c_str());
      if (!h.name[0] || !strcmp(h.name, h.key)) {
        String n = prefs.getString(nk, "");
        if (n.length()) snprintf(h.name, sizeof h.name, "%s", n.c_str());
      }
      continue;
    }
    if (!netOnline()) { allOk = false; break; }
    statusf("Currency for %s...", h.symbol);
    String body = httpsGet(String("https://" API_HOST API_CHART_PATH) + h.symbol + "?range=1d&interval=1d");
    char cur[8] = {0}, name[24] = {0};
    if (!body.length() || !pfParseChartMeta(body.c_str(), cur, sizeof cur, name, sizeof name)) {
      statusf("No currency for %s", h.symbol);
      snprintf(h.cur, sizeof h.cur, "%s", pf.base);       // assume the base, stop retrying
      allOk = false;
      continue;
    }
    snprintf(h.cur, sizeof h.cur, "%s", cur);
    prefs.putString(ck, cur);
    if (name[0]) {
      prefs.putString(nk, name);
      if (!h.name[0] || !strcmp(h.name, h.key)) snprintf(h.name, sizeof h.name, "%s", name);
    }
  }

  prefs.end();
  return allOk;
}

// ===================================================================== prices
bool netFetchPrices() {
  if (!pf.nh) return false;
  if (!netOnline()) return false;
  pfPrepareFx();

  char symbols[320];
  int n = pfSymbolList(symbols, sizeof symbols);
  if (!n) {
    status("No symbols to look up");
    return false;
  }
  statusf("Fetching %d quotes...", n);
  String body = httpsGet(String("https://" API_HOST API_SPARK_PATH) + symbols);
  if (!body.length()) {
    status("Price API did not answer");
    return false;
  }
  if (!pfParseSpark(body.c_str())) {
    status("Could not read the price data");
    return false;
  }
  pfCompute();
  statusf("%d of %d holdings priced", pf.nPriced, pf.nh);
  return pf.nPriced > 0;
}

// ===================================================================== OTA
static long semverNum(const char* v) {
  int a = 0, b = 0, c = 0;
  sscanf(v, "%d.%d.%d", &a, &b, &c);
  return (long)a * 1000000L + (long)b * 1000L + c;
}

static void progressCb(size_t done, size_t total) {
  static int last = -1;
  int pct = total ? (int)(done * 100ULL / total) : 0;
  if (pct / 5 == last) return;
  last = pct / 5;
  uiSetProgress(pct);
  statusf("Updating... %d%% - don't unplug", pct);
}

void netCheckAndUpdate(const char* runningVersion) {
  if (!netOnline()) return;
  status("Checking for update...");
  String body = httpsGet(String(ROMS_URL) + "?t=" + millis());
  char latest[16] = {0};
  if (!body.length() || !jlStr(jlFind(body.c_str(), nullptr, "version"), latest, sizeof latest)) {
    status("Could not reach GitHub Pages");
    return;
  }
  if (semverNum(latest) <= semverNum(runningVersion)) {
    statusf("Up to date (v%s)", runningVersion);
    return;
  }
  statusf("Installing v%s...", latest);
  UiView before = uiView();
  uiSetView(UV_LOADING);
  uiSetProgress(0);
  Update.onProgress(progressCb);
  ensureTime();
  WiFiClientSecure sc;
  sc.setCACert(API_ROOT_CAS);
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = httpUpdate.update(sc, String(PAGES_BASE) + OTA_BIN_PATH);
  uiSetProgress(-1);
  if (r != HTTP_UPDATE_OK) {                   // only reached when nothing was installed
    uiSetView(before);
    statusf("Update failed: %s", httpUpdate.getLastErrorString().c_str());
  }
}
