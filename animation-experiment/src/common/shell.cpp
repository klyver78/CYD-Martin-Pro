// Shared shell - see shell.h.
#include "shell.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Update.h>
#include <Preferences.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <XPT2046_Touchscreen.h>
#include <ImprovWiFiLibrary.h>
#include <esp_heap_caps.h>
#include "ota_certs.h"

// ---------------------------------------------------------------- colours (byte-swapped)
static const uint16_t C_BG     = rgbBE(14, 18, 32);
static const uint16_t C_CARD   = rgbBE(28, 34, 56);
static const uint16_t C_LINE   = rgbBE(52, 60, 90);
static const uint16_t C_TEXT   = rgbBE(235, 238, 245);
static const uint16_t C_DIM    = rgbBE(150, 160, 180);
static const uint16_t C_ACC    = rgbBE(255, 203, 45);
static const uint16_t C_ACC_FG = rgbBE(26, 21, 0);
static const uint16_t C_OK     = rgbBE(90, 210, 120);
static const uint16_t C_BAD    = rgbBE(240, 90, 80);
static const uint16_t C_OV_BG  = rgbBE(0, 0, 0);

// ---------------------------------------------------------------- state
enum Screen { SCR_MENU, SCR_RUN, SCR_RESULTS, SCR_INFO, SCR_ROMS, SCR_CONFIRM, SCR_MSG };
static Screen       screen = SCR_MENU;
static Experiment*  ex = nullptr;
static int          curScene = -1;
static int          menuGroup = 0;

static SPIClass            touchSPI(VSPI);
static XPT2046_Touchscreen touch(XPT2046_CS, XPT2046_IRQ);
static ImprovWiFi          improv(&Serial);
static Preferences         prefs;
static String              wifiSsid, wifiPass;
static bool                wifiEnabled = true;

// FPS bookkeeping
static uint32_t fpsT0 = 0, fpsFrames = 0;
static float    fpsNow = 0;
static char     fpsTxt[16] = "-- fps";
static bool     overlayDirty = true;

// Bench (run every scene in turn)
static const uint32_t BENCH_WARMUP_MS = 700, BENCH_MEASURE_MS = 3000;
static int      benchIdx = -1;               // -1 = not benchmarking
static uint32_t benchT0 = 0, benchFrames = 0;
static bool     benchMeasuring = false;
static float    benchFps[32];
static String   benchMetric[32];
static bool     haveResults = false;

// ROM list
struct RomEntry { String id, name, desc, bin, version; };
static RomEntry roms[8];
static int      nRoms = 0, romPick = -1;
static String   romsVersion, msgLine1, msgLine2;

// ---------------------------------------------------------------- forward decls
static void drawMenu();
static void drawResults();
static void drawInfo();
static void drawRoms();
static void drawConfirm();
static void drawMsg(const String& a, const String& b = "");
static void startScene(int i);
static void stopScene();

// ================================================================ UI rendering
// UI screens are rendered in horizontal strips through a temporary buffer, using the same
// software renderer as the scenes - so the shell works with every graphics library.
template <typename F>
static void renderRegion(int x, int y, int w, int h, F draw) {
  int sh = min(h, STRIP_H);
  uint16_t* buf = (uint16_t*)heap_caps_malloc((size_t)w * sh * 2, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!buf) return;
  Display& d = display();
  d.waitDMA();
  d.startWrite();
  for (int yy = y; yy < y + h; yy += sh) {
    int hh = min(sh, y + h - yy);
    Canvas c; c.attach(buf, x, yy, w, hh);
    c.clear(C_BG);
    draw(c);
    d.push(x, yy, w, hh, buf);
  }
  d.endWrite();
  free(buf);
}
template <typename F> static void renderScreen(F draw) { renderRegion(0, 0, SCR_W, SCR_H, draw); }

static void ctext(Canvas& c, int cx, int y, const char* s, uint16_t fg, int size) {
  c.text(cx - Canvas::textW(s, size) / 2, y, s, fg, size);
}
static void button(Canvas& c, int x, int y, int w, int h, const char* label, int size, bool on) {
  c.fillRect(x, y, w, h, on ? C_ACC : C_CARD);
  c.rect(x, y, w, h, on ? C_ACC : C_LINE);
  ctext(c, x + w / 2, y + (h - 8 * size) / 2, label, on ? C_ACC_FG : C_TEXT, size);
}
static bool inBox(int tx, int ty, int x, int y, int w, int h) {
  return tx >= x && tx < x + w && ty >= y && ty < y + h;
}
static void header(Canvas& c, const char* title, bool back) {
  c.fillRect(0, 0, SCR_W, 26, C_CARD);
  c.text(8, 5, title, C_ACC, 2);
  if (back) button(c, SCR_W - 70, 2, 66, 22, "Back", 2, false);
}

// ================================================================ touch
bool readTap(int& tx, int& ty) {
  static bool     down = false;
  static uint32_t upSince = 0, lastTap = 0;
  bool t = touch.tirqTouched() && touch.touched();
  if (!t) {
    if (down) {
      if (!upSince) upSince = millis();
      else if (millis() - upSince > 60) down = false;     // released
    }
    return false;
  }
  upSince = 0;
  if (down) return false;
  down = true;
  if (millis() - lastTap < 150) return false;
  delay(10);                                  // let the reading settle
  TS_Point p = touch.getPoint();
  if (p.z < 200) { down = false; return false; }
  lastTap = millis();
  tx = map(p.x, TOUCH_MINX, TOUCH_MAXX, 0, SCR_W - 1);
  ty = map(p.y, TOUCH_MINY, TOUCH_MAXY, 0, SCR_H - 1);
  if (TOUCH_FLIP_X) tx = SCR_W - 1 - tx;
  if (TOUCH_FLIP_Y) ty = SCR_H - 1 - ty;
  tx = constrain(tx, 0, SCR_W - 1);
  ty = constrain(ty, 0, SCR_H - 1);
  return true;
}

// ================================================================ overlay
// Top-left: FPS (+ metric). Bottom: [-] param [+] for scenes with a tunable.
static const int OV_H = 18, PB_Y = SCR_H - 24, PB_H = 24;

float shellFps() { return fpsNow; }
const char* shellFpsText() { return fpsTxt; }

bool shellOverlayHits(int y0, int h) {
  if (y0 < OV_H) return true;
  return ex && ex->hasParam() && y0 + h > PB_Y;
}

static void overlayLine(char* line, size_t n) {
  String m = ex->metric();
  if (benchIdx >= 0) snprintf(line, n, " %s  bench %d/%d ", fpsTxt, benchIdx + 1, ex->sceneCount());
  else if (m.length()) snprintf(line, n, " %s  %s ", fpsTxt, m.c_str());
  else snprintf(line, n, " %s ", fpsTxt);
}

void shellOverlay(Canvas& c) {
  if (!ex) return;
  if (c.y0 < OV_H) {
    char line[48];
    overlayLine(line, sizeof line);
    c.fillRect(0, 0, Canvas::textW(line, 2), OV_H, C_OV_BG);
    c.text(0, 1, line, C_ACC, 2);
  }
  if (ex->hasParam() && c.y0 + c.h > PB_Y) {
    c.fillRect(0, PB_Y, SCR_W, PB_H, C_OV_BG);
    c.fillRect(2, PB_Y + 2, 60, PB_H - 4, C_CARD);
    c.fillRect(SCR_W - 62, PB_Y + 2, 60, PB_H - 4, C_CARD);
    ctext(c, 32, PB_Y + 5, "-", C_TEXT, 2);
    ctext(c, SCR_W - 32, PB_Y + 5, "+", C_TEXT, 2);
    String l = ex->paramLabel();
    ctext(c, SCR_W / 2, PB_Y + 5, l.c_str(), C_TEXT, 2);
  }
}

// For scenes that draw straight to the panel. Only re-sent when the text changed, so it
// doesn't eat into what's being measured.
void shellOverlayDirect(bool force) {
  if ((!overlayDirty && !force) || !ex) return;
  overlayDirty = false;
  static uint16_t ovBuf[SCR_W * PB_H];        // static: no heap churn while measuring
  Display& d = display();
  char line[48];
  overlayLine(line, sizeof line);
  int w = min(SCR_W, Canvas::textW(line, 2));
  Canvas c;
  c.attach(ovBuf, 0, 0, w, OV_H);
  shellOverlay(c);
  d.waitDMA();
  d.push(0, 0, w, OV_H, ovBuf);
  if (ex->hasParam()) {
    c.attach(ovBuf, 0, PB_Y, SCR_W, PB_H);
    shellOverlay(c);
    d.push(0, PB_Y, SCR_W, PB_H, ovBuf);
  }
}

static void updateFps(int frames) {
  fpsFrames += frames;
  uint32_t now = millis();
  if (now - fpsT0 >= 500) {
    fpsNow = fpsFrames * 1000.0f / (now - fpsT0);
    snprintf(fpsTxt, sizeof fpsTxt, "%.1f fps", fpsNow);
    fpsFrames = 0;
    fpsT0 = now;
    overlayDirty = true;
  }
}

// ================================================================ Wi-Fi / Improv
static void loadWiFi() {
  prefs.begin("wifi", true);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  prefs.end();
  prefs.begin("anim", true);
  wifiEnabled = prefs.getBool("wifi_on", true);
  prefs.end();
}
static void saveWiFi(const char* s, const char* p) {
  wifiSsid = s; wifiPass = p;
  prefs.begin("wifi", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.end();
}
static void wifiStart() {
  if (!wifiSsid.length()) return;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}
static void wifiStop() { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }
static void setWifiEnabled(bool on) {
  wifiEnabled = on;
  prefs.begin("anim", false);
  prefs.putBool("wifi_on", on);
  prefs.end();
  if (on) wifiStart(); else wifiStop();
}
static void onImprovConnected(const char* ssid, const char* password) {
  saveWiFi(ssid, password);
  if (!wifiEnabled) setWifiEnabled(true);
}
// Make sure we're online (temporarily turning Wi-Fi on if it was switched off for tests).
static bool ensureOnline(uint32_t timeoutMs = 12000) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (!wifiSsid.length()) return false;
  if (WiFi.getMode() == WIFI_OFF) wifiStart();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    improv.handleSerial();
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}
static bool ensureTime() {
  if (time(nullptr) > 1700000000) return true;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < 8000) delay(100);
  return time(nullptr) > 1700000000;
}

// ================================================================ scenes
static void startScene(int i) {
  if (i < 0 || i >= ex->sceneCount()) return;
  curScene = i;
  screen = SCR_RUN;
  fpsT0 = millis(); fpsFrames = 0; fpsNow = 0;
  strcpy(fpsTxt, "-- fps");
  overlayDirty = true;
  display().fillScreen(0);
  if (!ex->start(i)) {
    ex->stop();
    drawMsg("Not enough memory", String("for ") + ex->scene(i).name);
    benchIdx = -1;
    return;
  }
  Serial.printf("[scene] %s / %s  heap free %u, largest %u\n", ex->groupName(ex->scene(i).group),
                ex->scene(i).name, (unsigned)ESP.getFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
static void stopScene() {
  if (curScene >= 0) ex->stop();
  display().waitDMA();
  curScene = -1;
}

static void benchStart() {
  haveResults = false;
  for (int i = 0; i < 32; i++) { benchFps[i] = -1; benchMetric[i] = ""; }
  benchIdx = 0;
  Serial.println("\n[bench] start");
  startScene(0);
  benchT0 = millis(); benchFrames = 0; benchMeasuring = false;
}
static void benchFinish() {
  stopScene();
  benchIdx = -1;
  haveResults = true;
  Serial.printf("\n| ROM | Library | Group | Scene | FPS | Metric |\n|---|---|---|---|---|---|\n");
  for (int i = 0; i < ex->sceneCount() && i < 32; i++) {
    SceneInfo s = ex->scene(i);
    Serial.printf("| %s | %s | %s | %s | %.1f | %s |\n", ROM_ID, display().libName(), ex->groupName(s.group),
                  s.name, benchFps[i], benchMetric[i].c_str());
  }
  drawResults();
}
static void benchStep(int frames) {
  uint32_t now = millis();
  if (!benchMeasuring) {
    if (now - benchT0 >= BENCH_WARMUP_MS) { benchMeasuring = true; benchT0 = now; benchFrames = 0; }
    return;
  }
  benchFrames += frames;
  if (now - benchT0 < BENCH_MEASURE_MS) return;
  benchFps[benchIdx] = benchFrames * 1000.0f / (now - benchT0);
  benchMetric[benchIdx] = ex->metric();
  Serial.printf("[bench] %-12s %6.1f fps %s\n", ex->scene(benchIdx).name, benchFps[benchIdx], benchMetric[benchIdx].c_str());
  stopScene();
  if (++benchIdx >= ex->sceneCount() || benchIdx >= 32) { benchFinish(); return; }
  startScene(benchIdx);
  if (screen != SCR_RUN) { benchIdx = -1; return; }      // start failed
  benchT0 = millis(); benchFrames = 0; benchMeasuring = false;
}

// ================================================================ screens
// Menu: tabs (groups) / scene buttons (2 x 4) / bottom row.
static const int TAB_Y = 30, TAB_H = 28, SC_Y = 64, SC_H = 32, SC_GAP = 4, BOT_Y = 206, BOT_H = 30;

static int groupScenes(int g, int* out, int max) {
  int n = 0;
  for (int i = 0; i < ex->sceneCount() && n < max; i++) if (ex->scene(i).group == g) out[n++] = i;
  return n;
}

static void drawMenu() {
  screen = SCR_MENU;
  if (!wifiEnabled && WiFi.getMode() != WIFI_OFF) wifiStop();   // was only on for the ROM list
  renderScreen([](Canvas& c) {
    char t[40]; snprintf(t, sizeof t, "%s", ROM_NAME);
    c.fillRect(0, 0, SCR_W, 26, C_CARD);
    c.text(8, 5, t, C_ACC, 2);
    char v[24]; snprintf(v, sizeof v, "v%s", FW_VERSION_SEM);
    c.text(SCR_W - Canvas::textW(v, 1) - 6, 10, v, C_DIM, 1);
    int ng = ex->groupCount(), tw = SCR_W / ng;
    for (int g = 0; g < ng; g++) {
      const char* gn = ex->groupName(g);
      button(c, g * tw + 2, TAB_Y, tw - 4, TAB_H, gn, Canvas::textW(gn, 2) <= tw - 10 ? 2 : 1, g == menuGroup);
    }
    int idx[8]; int n = groupScenes(menuGroup, idx, 8);
    for (int k = 0; k < n; k++) {
      int col = k & 1, r = k >> 1;
      int x = 4 + col * 158, y = SC_Y + r * (SC_H + SC_GAP);
      button(c, x, y, 154, SC_H, ex->scene(idx[k]).name, 2, false);
    }
    const char* bl[4] = {"Bench", "ROMs", "Info", "Results"};
    for (int b = 0; b < 4; b++) {
      if (b == 3 && !haveResults) continue;
      button(c, 4 + b * 79, BOT_Y, 75, BOT_H, bl[b], 2, b == 0);
    }
  });
}
static void menuTap(int tx, int ty) {
  int ng = ex->groupCount(), tw = SCR_W / ng;
  if (ty >= TAB_Y && ty < TAB_Y + TAB_H) { menuGroup = constrain(tx / tw, 0, ng - 1); drawMenu(); return; }
  if (ty >= BOT_Y) {
    int b = (tx - 4) / 79;
    if (b == 0) benchStart();
    else if (b == 1) drawRoms();
    else if (b == 2) drawInfo();
    else if (b == 3 && haveResults) drawResults();
    return;
  }
  int idx[8]; int n = groupScenes(menuGroup, idx, 8);
  for (int k = 0; k < n; k++) {
    int col = k & 1, r = k >> 1;
    if (inBox(tx, ty, 4 + col * 158, SC_Y + r * (SC_H + SC_GAP), 154, SC_H)) { startScene(idx[k]); return; }
  }
}

static void drawResults() {
  screen = SCR_RESULTS;
  renderScreen([](Canvas& c) {
    header(c, "Bench results", true);
    char l[64];
    snprintf(l, sizeof l, "%s / %s / SPI %.1f MHz", ROM_ID, display().libName(), display().spiHz() / 1e6);
    c.text(8, 30, l, C_DIM, 1);
    int n = min(ex->sceneCount(), 32), y = 44;
    for (int i = 0; i < n; i++) {
      SceneInfo s = ex->scene(i);
      int col = (n > 18 && i >= (n + 1) / 2) ? 1 : 0;
      int row = col ? i - (n + 1) / 2 : i;
      int x = 8 + col * 158;
      int yy = y + row * 10;
      if (benchFps[i] < 0) snprintf(l, sizeof l, "%-12s   --", s.name);
      else snprintf(l, sizeof l, "%-12s %6.1f", s.name, benchFps[i]);
      c.text(x, yy, l, benchFps[i] >= 30 ? C_OK : (benchFps[i] >= 15 ? C_TEXT : C_BAD), 1);
      if (col == 0 && n <= 18 && benchMetric[i].length()) c.text(x + 132, yy, benchMetric[i].c_str(), C_DIM, 1);
    }
    c.text(8, SCR_H - 12, "Also printed to serial (115200) as a table.", C_DIM, 1);
  });
}

static void drawInfo() {
  screen = SCR_INFO;
  renderScreen([](Canvas& c) {
    header(c, "Info", true);
    char l[64]; int y = 34;
    auto line = [&](const char* k, const String& v) {
      c.text(8, y, k, C_DIM, 1); c.text(110, y, v.c_str(), C_TEXT, 1); y += 13;
    };
    float hz = display().spiHz();
    line("ROM", String(ROM_ID) + "  v" + FW_VERSION_SEM);
    line("Library", display().libName());
    snprintf(l, sizeof l, "%.2f MHz", hz / 1e6); line("SPI write clock", l);
    snprintf(l, sizeof l, "%.1f fps (320x240x16 bit)", hz / (SCR_W * SCR_H * 16.0f)); line("Wire limit", l);
    line("DMA", display().hasDMA() ? "yes" : "no");
    snprintf(l, sizeof l, "%u MHz", (unsigned)getCpuFrequencyMhz()); line("CPU", l);
    snprintf(l, sizeof l, "%u KB free, %u KB largest", (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));
    line("Heap", l);
    line("PSRAM", psramFound() ? String(ESP.getPsramSize() / 1024) + " KB" : String("none"));
    if (!wifiSsid.length()) line("Wi-Fi", "not set - use web flasher");
    else if (WiFi.status() == WL_CONNECTED)
      line("Wi-Fi", WiFi.localIP().toString() + "  " + String(WiFi.RSSI()) + " dBm");
    else line("Wi-Fi", wifiEnabled ? "connecting..." : "off");
    c.text(8, BOT_Y - 12, "Wi-Fi off = no radio activity during tests", C_DIM, 1);
    button(c, 4, BOT_Y, 152, BOT_H, wifiEnabled ? "Wi-Fi: on" : "Wi-Fi: off", 2, wifiEnabled);
  });
}
static void infoTap(int tx, int ty) {
  if (ty < 26 && tx > SCR_W - 76) { drawMenu(); return; }
  if (inBox(tx, ty, 4, BOT_Y, 152, BOT_H)) { setWifiEnabled(!wifiEnabled); drawInfo(); }
}

static void drawMsg(const String& a, const String& b) {
  screen = SCR_MSG;
  msgLine1 = a; msgLine2 = b;
  renderScreen([](Canvas& c) {
    ctext(c, SCR_W / 2, 92, msgLine1.c_str(), C_TEXT, 2);
    ctext(c, SCR_W / 2, 118, msgLine2.c_str(), C_DIM, 1);
    ctext(c, SCR_W / 2, SCR_H - 20, "tap to continue", C_DIM, 1);
  });
}

// ---------------------------------------------------------------- ROM switcher
static bool fetchRoms() {
  nRoms = 0;
  if (!ensureOnline()) return false;
  ensureTime();                              // certificate validation needs the real time
  WiFiClientSecure sc;
  sc.setCACert(OTA_ROOT_CAS);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  String body;
  String url = String(ROMS_URL) + "?t=" + String(millis());   // dodge the Pages CDN cache
  if (http.begin(sc, url)) {
    int code = http.GET();
    if (code == HTTP_CODE_OK) body = http.getString();
    else Serial.printf("[roms] HTTP %d\n", code);
    http.end();
  }
  if (!body.length()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  romsVersion = doc["version"] | "";
  for (JsonObject r : doc["roms"].as<JsonArray>()) {
    if (nRoms >= 8) break;
    RomEntry& e = roms[nRoms++];
    e.id = r["id"] | ""; e.name = r["name"] | ""; e.desc = r["desc"] | "";
    e.bin = r["bin"] | ""; e.version = doc["version"] | "";
  }
  return nRoms > 0;
}

static const int ROW_Y = 30, ROW_H = 40;
static void drawRoms() {
  screen = SCR_ROMS;
  renderScreen([](Canvas& c) {
    header(c, "Switch ROM", true);
    ctext(c, SCR_W / 2, 110, "Loading ROM list...", C_DIM, 2);
  });
  bool ok = fetchRoms();
  if (!ok) {
    if (!wifiSsid.length()) drawMsg("No Wi-Fi set up", "Use the web flasher -> Connect to Wi-Fi");
    else drawMsg("Could not load ROM list", WiFi.status() == WL_CONNECTED ? "roms.json not reachable" : "Wi-Fi not connected");
    return;
  }
  renderScreen([](Canvas& c) {
    header(c, "Switch ROM", true);
    for (int i = 0; i < nRoms && i < 5; i++) {
      int y = ROW_Y + i * ROW_H;
      bool cur = roms[i].id == ROM_ID;
      c.fillRect(4, y, SCR_W - 8, ROW_H - 4, cur ? C_LINE : C_CARD);
      c.text(10, y + 4, roms[i].name.c_str(), cur ? C_ACC : C_TEXT, 2);
      if (cur) c.text(SCR_W - 70, y + 8, "running", C_ACC, 1);
      c.text(10, y + 24, roms[i].desc.c_str(), C_DIM, 1);
    }
    char v[48]; snprintf(v, sizeof v, "latest v%s - running v%s", romsVersion.c_str(), FW_VERSION_SEM);
    c.text(8, SCR_H - 12, v, C_DIM, 1);
  });
}
static void romsTap(int tx, int ty) {
  if (ty < 26 && tx > SCR_W - 76) { drawMenu(); return; }
  int i = (ty - ROW_Y) / ROW_H;
  if (ty >= ROW_Y && i >= 0 && i < nRoms && i < 5) { romPick = i; drawConfirm(); }
}

static void drawConfirm() {
  screen = SCR_CONFIRM;
  renderScreen([](Canvas& c) {
    header(c, "Install ROM", false);
    RomEntry& r = roms[romPick];
    ctext(c, SCR_W / 2, 60, r.name.c_str(), C_ACC, 2);
    ctext(c, SCR_W / 2, 84, r.desc.c_str(), C_DIM, 1);
    String v = "v" + r.version + (r.id == ROM_ID ? "  (reinstall / update)" : "");
    ctext(c, SCR_W / 2, 100, v.c_str(), C_TEXT, 1);
    ctext(c, SCR_W / 2, 124, "Downloads over Wi-Fi and restarts.", C_DIM, 1);
    ctext(c, SCR_W / 2, 136, "Wi-Fi settings are kept.", C_DIM, 1);
    button(c, 20, 170, 130, 40, "Install", 2, true);
    button(c, 170, 170, 130, 40, "Cancel", 2, false);
  });
}

static void drawOtaProgress(size_t done, size_t total) {
  static int lastPct = -1;
  int pct = total ? (int)((done * 100ULL) / total) : 0;
  if (pct == lastPct) return;
  lastPct = pct;
  renderRegion(30, 120, SCR_W - 60, 40, [pct](Canvas& c) {
    c.rect(30, 120, SCR_W - 60, 14, C_DIM);
    c.fillRect(32, 122, (SCR_W - 64) * pct / 100, 10, C_ACC);
    char t[8]; snprintf(t, sizeof t, "%d%%", pct);
    ctext(c, SCR_W / 2, 142, t, C_TEXT, 2);
  });
}

static void runOta() {
  RomEntry r = roms[romPick];
  renderScreen([&r](Canvas& c) {
    ctext(c, SCR_W / 2, 60, "Installing", C_TEXT, 2);
    ctext(c, SCR_W / 2, 84, r.name.c_str(), C_ACC, 2);
    ctext(c, SCR_W / 2, 180, "Don't unplug - restarts when done", C_DIM, 1);
  });
  if (!ensureOnline()) { drawMsg("Wi-Fi not connected"); return; }
  ensureTime();
  String url = String(PAGES_BASE) + r.bin;
  Serial.printf("[ota] %s\n", url.c_str());
  Update.onProgress(drawOtaProgress);
  WiFiClientSecure sc;
  sc.setCACert(OTA_ROOT_CAS);
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return res = httpUpdate.update(sc, url);
  if (res == HTTP_UPDATE_NO_UPDATES) drawMsg("No update");
  else drawMsg("Update failed", httpUpdate.getLastErrorString());
}
static void confirmTap(int tx, int ty) {
  if (inBox(tx, ty, 20, 170, 130, 40)) runOta();
  else if (inBox(tx, ty, 170, 170, 130, 40)) drawRoms();
}

// ================================================================ setup / loop
void shellSetup(Experiment& e) {
  ex = &e;
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touch.begin(touchSPI);
  touch.setRotation(1);                        // matches the landscape screen

  loadWiFi();
  improv.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32, "cyd-anim-" ROM_ID, FW_VERSION_SEM, ROM_NAME);
  improv.onImprovConnected(onImprovConnected);
  if (wifiEnabled) wifiStart();

  Serial.printf("\n%s (%s) v%s - %s, SPI %.2f MHz\n", ROM_NAME, ROM_ID, FW_VERSION_SEM,
                display().libName(), display().spiHz() / 1e6);
  drawMenu();
}

void shellLoop() {
  improv.handleSerial();
  int tx, ty;
  bool tap = readTap(tx, ty);

  if (screen == SCR_RUN) {
    if (tap) {
      if (benchIdx >= 0) { stopScene(); benchIdx = -1; drawMenu(); return; }   // abort bench
      if (ex->hasParam() && ty >= PB_Y - 10) {
        if (tx < 90)            { ex->param(-1); overlayDirty = true; fpsT0 = millis(); fpsFrames = 0; }
        else if (tx > SCR_W - 90) { ex->param(+1); overlayDirty = true; fpsT0 = millis(); fpsFrames = 0; }
        else { stopScene(); drawMenu(); return; }
      } else { stopScene(); drawMenu(); return; }
    }
    int f = ex->frame();
    updateFps(f);
    if (benchIdx >= 0) benchStep(f);
    return;
  }

  if (!tap) { delay(5); return; }
  switch (screen) {
    case SCR_MENU:    menuTap(tx, ty); break;
    case SCR_RESULTS: drawMenu(); break;
    case SCR_INFO:    infoTap(tx, ty); break;
    case SCR_ROMS:    romsTap(tx, ty); break;
    case SCR_CONFIRM: confirmTap(tx, ty); break;
    case SCR_MSG:     drawMenu(); break;
    default: break;
  }
}
