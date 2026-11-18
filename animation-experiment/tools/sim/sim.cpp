// Host-side renderer: compiles the real shell + scene code against mock headers and writes
// screenshots of the menu and of every scene (tools/sim/out/*.ppm -> docs/*.png).
#include <cstdio>
#include <vector>
#include "Arduino.h"
uint32_t g_millis = 100000;
#include "WiFi.h"
#include "HTTPUpdate.h"
#include "Update.h"
HardwareSerial Serial; WiFiClass WiFi; EspClass ESP; HTTPUpdateClass httpUpdate; UpdateClass Update;
#define FW_VERSION_SEM "1.0.0"
#define ROM_ID "tftespi"
#define ROM_NAME "TFT_eSPI"
#define GFX_TFT 1
#include "../../src/common/display.h"
#include "../../src/common/board.h"

static uint16_t fb[SCR_W * SCR_H];
class SimDisplay : public Display {
public:
  void begin() override {}
  const char* libName() const override { return "TFT_eSPI"; }
  void fillScreen(uint16_t c) override { for (auto& p : fb) p = c; }
  void fillRect(int x, int y, int w, int h, uint16_t c) override {
    for (int yy = std::max(0, y); yy < std::min(SCR_H, y + h); yy++)
      for (int xx = std::max(0, x); xx < std::min(SCR_W, x + w); xx++) fb[yy * SCR_W + xx] = c;
  }
  void fillCircle(int cx, int cy, int r, uint16_t c) override {
    for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++)
      if (x * x + y * y <= r * r) fillRect(cx + x, cy + y, 1, 1, c);
  }
  void push(int x, int y, int w, int h, const uint16_t* be) override {
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) {
      int X = x + xx, Y = y + yy;
      if (X >= 0 && X < SCR_W && Y >= 0 && Y < SCR_H) fb[Y * SCR_W + X] = swap16(be[yy * w + xx]);
    }
  }
  void pushDMA(int x, int y, int w, int h, const uint16_t* be) override { push(x, y, w, h, be); }
  void waitDMA() override {}
  bool hasDMA() const override { return true; }
  void startWrite() override {}
  void endWrite() override {}
  uint32_t spiHz() const override { return 40000000; }
};
static SimDisplay sd;
Display& display() { return sd; }

#include "../../src/common/shell.cpp"
#include "../../src/gfx/scenes.cpp"

static void save(const char* name) {
  char path[128]; snprintf(path, sizeof path, "out/%s.ppm", name);
  FILE* f = fopen(path, "wb");
  fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H);
  for (uint16_t c : fb) {
    uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}
static void slug(const char* in, char* out) {
  for (; *in; in++) *out++ = isalnum((unsigned char)*in) ? tolower(*in) : '-';
  *out = 0;
}

int main() {
  srand(42);
  Experiment& e = gfxExperiment();
  shellSetup(e);
  save("menu_raw");
  for (int g = 1; g < e.groupCount(); g++) {
    menuGroup = g; drawMenu();
    char n[32]; snprintf(n, sizeof n, "menu_%d", g); save(n);
  }
  menuGroup = 0;
  for (int i = 0; i < e.sceneCount(); i++) {
    startScene(i);
    for (int f = 0; f < 90; f++) { g_millis += 22; ex->frame(); updateFps(1); }
    char n[48], s[32]; slug(e.scene(i).name, s); snprintf(n, sizeof n, "scene_%s", s); save(n);
    benchFps[i] = 10 + (i * 7) % 50;
    stopScene();
  }
  haveResults = true;
  drawResults(); save("results");
  drawInfo(); save("info");
  printf("rendered %d scenes\n", e.sceneCount());
}
