#ifndef GFX_LVGL   // the LVGL ROM has its own main + scenes (src/lvgl)
// The animation experiments for the TFT_eSPI and LovyanGFX ROMs.
//
// Every scene is written against the Display interface + the software Canvas, so the exact
// same code is measured on both libraries. Three ways of getting pixels to the panel are
// compared:
//   * direct   - library primitives straight to the panel (fillScreen / fillRect / fillCircle)
//   * strips   - the frame is rendered in 320x40 strips into RAM and pushed (blocking or DMA,
//                double-buffered so the CPU renders strip n+1 while DMA sends strip n)
//   * regions  - only a box around what changed is rendered + pushed (dirty rectangles)
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include "../common/shell.h"
#include "../common/board.h"

static Display& D() { return display(); }

// ================================================================ shared helpers
static uint16_t* sbuf[2] = {nullptr, nullptr};   // strip buffers (DMA capable)
static int8_t    sinT[256];
static uint16_t  pal[256];                        // byte-swapped palette, per scene

static bool allocStrips() {
  for (int i = 0; i < 2; i++) {
    sbuf[i] = (uint16_t*)heap_caps_malloc(SCR_W * STRIP_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!sbuf[i]) return false;
  }
  return true;
}
static void freeStrips() {
  D().waitDMA();
  for (int i = 0; i < 2; i++) { free(sbuf[i]); sbuf[i] = nullptr; }
}
static void initSin() {
  for (int i = 0; i < 256; i++) sinT[i] = (int8_t)(127.0f * sinf(i * 2 * PI / 256));
}
static uint16_t hsvBE(float h, float s, float v) {      // h 0..1
  float r, g, b;
  int i = (int)(h * 6) % 6;
  float f = h * 6 - floorf(h * 6), p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
  switch (i) { case 0: r=v; g=t; b=p; break; case 1: r=q; g=v; b=p; break; case 2: r=p; g=v; b=t; break;
               case 3: r=p; g=q; b=v; break; case 4: r=t; g=p; b=v; break; default: r=v; g=p; b=q; }
  return rgbBE((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
}
static uint16_t hsv565(float h, float s, float v) { return swap16(hsvBE(h, s, v)); }

// Render a full frame in strips. `draw` must paint every pixel of the canvas it gets.
template <typename F>
static void stripFrame(bool dma, F draw) {
  for (int y = 0, k = 0; y < SCR_H; y += STRIP_H, k ^= 1) {
    Canvas c; c.attach(sbuf[k], 0, y, SCR_W, STRIP_H);
    draw(c);
    shellOverlay(c);
    if (dma) D().pushDMA(0, y, SCR_W, STRIP_H, sbuf[k]);
    else     D().push(0, y, SCR_W, STRIP_H, sbuf[k]);
  }
}
// Render only a box (in strips of up to STRIP_H rows).
template <typename F>
static void regionFrame(int x, int y, int w, int h, F draw) {
  int sh = min(STRIP_H, (SCR_W * STRIP_H) / w);
  for (int yy = y, k = 0; yy < y + h; yy += sh, k ^= 1) {
    int hh = min(sh, y + h - yy);
    Canvas c; c.attach(sbuf[k], x, yy, w, hh);
    draw(c);
    D().pushDMA(x, yy, w, hh, sbuf[k]);
  }
}

static String mbps(float fpsFraction = 1.0f) {
  char t[16]; snprintf(t, sizeof t, "%.1f MB/s", shellFps() * SCR_W * SCR_H * 2 * fpsFraction / 1e6f);
  return t;
}

// ================================================================ scene base
class Scene {
public:
  virtual ~Scene() {}
  virtual bool begin() { return allocStrips(); }
  virtual void end() { freeStrips(); }
  virtual void frame() = 0;
  virtual bool hasParam() { return false; }
  virtual void param(int) {}
  virtual String paramLabel() { return ""; }
  virtual String metric() { return ""; }
};

// ================================================================ RAW
// Library fillScreen - the panel's raw fill rate with zero CPU work.
class FillScene : public Scene {
  uint8_t n = 0;
public:
  bool begin() override { return true; }
  void end() override {}
  void frame() override {
    D().fillScreen(hsv565((n++ & 63) / 64.0f, 1, 1));
    shellOverlayDirect(true);
  }
  String metric() override { return mbps(); }
};

// Strips with almost no rendering cost (one colour per row) - measures the push path.
class StripScene : public Scene {
  bool dma; uint8_t t = 0;
public:
  explicit StripScene(bool d) : dma(d) {}
  void frame() override {
    t += 3;
    stripFrame(dma, [this](Canvas& c) {
      for (int y = c.y0; y < c.y0 + c.h; y++) {
        uint16_t col = pal[(uint8_t)(y + t)];
        uint32_t c2 = ((uint32_t)col << 16) | col, *p = (uint32_t*)c.row(y);
        for (int i = 0; i < SCR_W / 2; i++) *p++ = c2;
      }
    });
  }
  bool begin() override {
    for (int i = 0; i < 256; i++) pal[i] = hsvBE(i / 256.0f, 0.8f, 1);
    return allocStrips();
  }
  String metric() override { return mbps(); }
};

// 100 random 32x32 fillRects per frame (small-primitive overhead: address window per call).
class RectScene : public Scene {
public:
  bool begin() override { D().fillScreen(0); return true; }
  void end() override {}
  void frame() override {
    for (int i = 0; i < 100; i++) {
      uint32_t r = esp_random();
      D().fillRect(r % (SCR_W - 32), 20 + (r >> 9) % (SCR_H - 52), 32, 32, (uint16_t)(r >> 16));
    }
    shellOverlayDirect();
  }
  String metric() override { char t[20]; snprintf(t, sizeof t, "%.0f rects/s", shellFps() * 100); return t; }
};

// LovyanGFX only: change the SPI clock at runtime. Test pattern has fine lines so corruption
// at too-high clocks is easy to see.
static const uint32_t SWEEP_HZ[] = {10000000, 20000000, 26666667, 40000000, 80000000};
class SweepScene : public Scene {
  int idx = 3; uint8_t t = 0; uint32_t restoreHz = 0;
  void apply() { D().endWrite(); D().setSpiHz(SWEEP_HZ[idx]); D().startWrite(); }
public:
  bool begin() override {
    restoreHz = D().spiHz();
    idx = 3;                                         // 40 MHz = the default clock
    for (int i = 0; i < 256; i++) pal[i] = hsvBE(i / 256.0f, 0.7f, 0.9f);
    return allocStrips();
  }
  void end() override { freeStrips(); D().endWrite(); D().setSpiHz(restoreHz); D().startWrite(); }
  void frame() override {
    t += 2;
    stripFrame(true, [this](Canvas& c) {
      for (int y = c.y0; y < c.y0 + c.h; y++) {
        uint16_t* p = c.row(y);
        uint16_t col = pal[(uint8_t)(y + t)];
        for (int x = 0; x < SCR_W; x++) p[x] = (x & 7) == 0 ? 0xFFFF : ((y & 7) == 0 ? 0 : col);
      }
      c.text(100, 100, "fine grid = no SPI errors", 0xFFFF, 1, 0);
    });
  }
  bool hasParam() override { return true; }
  void param(int d) { idx = constrain(idx + d, 0, 4); apply(); }
  String paramLabel() override { char t[16]; snprintf(t, sizeof t, "%.1f MHz", D().spiHz() / 1e6f); return t; }
  String metric() override { return mbps(); }
};

// ================================================================ SPRITES
static const int NSTEPS[] = {1, 5, 10, 25, 50, 100, 200, 400};
struct Ball { float x, y, vx, vy; int16_t r, ox, oy; uint16_t be, c; };
static Ball* balls = nullptr;
static int   nBalls = 0, nIdx = 4;          // default 50
static const uint16_t BG_A = 0x1082, BG_B = 0x18E3;  // two dark greys (normal 565)

static void spawnBalls() {
  nBalls = NSTEPS[nIdx];
  for (int i = 0; i < nBalls; i++) {
    Ball& b = balls[i];
    b.r = 6 + esp_random() % 10;
    b.x = b.r + esp_random() % (SCR_W - 2 * b.r);
    b.y = 20 + b.r + esp_random() % (SCR_H - 50 - 2 * b.r);
    float a = (esp_random() % 628) / 100.0f, s = 1.5f + (esp_random() % 300) / 100.0f;
    b.vx = cosf(a) * s; b.vy = sinf(a) * s;
    b.ox = b.x; b.oy = b.y;
    b.c = hsv565((esp_random() % 360) / 360.0f, 0.8f, 1); b.be = swap16(b.c);
  }
}
static void moveBalls() {
  for (int i = 0; i < nBalls; i++) {
    Ball& b = balls[i];
    b.ox = (int16_t)b.x; b.oy = (int16_t)b.y;
    b.x += b.vx; b.y += b.vy;
    if (b.x < b.r || b.x > SCR_W - 1 - b.r) { b.vx = -b.vx; b.x += 2 * b.vx; }
    if (b.y < 20 + b.r || b.y > SCR_H - 26 - b.r) { b.vy = -b.vy; b.y += 2 * b.vy; }
  }
}
// Checkerboard background (20 px squares), painted as runs.
static void bgPattern(Canvas& c) {
  const uint16_t a = swap16(BG_A), b = swap16(BG_B);
  for (int y = c.y0; y < c.y0 + c.h; y++) {
    uint16_t* p = c.row(y);
    for (int x = c.x0; x < c.x0 + c.w; ) {
      int end = min(c.x0 + c.w, (x / 20 + 1) * 20);
      uint16_t col = (((x / 20) + (y / 20)) & 1) ? a : b;
      for (; x < end; x++) p[x] = col;
    }
  }
}
static void drawBalls(Canvas& c) {
  for (int i = 0; i < nBalls; i++) {
    Ball& b = balls[i];
    int y = (int)b.y;
    if (y + b.r < c.y0 || y - b.r >= c.y0 + c.h) continue;
    int x = (int)b.x;
    if (x + b.r < c.x0 || x - b.r >= c.x0 + c.w) continue;
    c.fillCircle(x, y, b.r, b.be);
  }
}

class SpriteBase : public Scene {
public:
  bool begin() override {
    balls = (Ball*)malloc(sizeof(Ball) * 400);
    if (!balls) return false;
    spawnBalls();
    return allocStrips();
  }
  void end() override { freeStrips(); free(balls); balls = nullptr; }
  bool hasParam() override { return true; }
  void param(int d) override { nIdx = constrain(nIdx + d, 0, 7); spawnBalls(); onParam(); }
  virtual void onParam() {}
  String paramLabel() override { return String(nBalls) + " balls"; }
};

// Erase + redraw each ball with library calls. Fast, but flickers and can't do backgrounds.
class DirectBalls : public SpriteBase {
public:
  bool begin() override { bool ok = SpriteBase::begin(); D().fillScreen(BG_A); return ok; }
  void onParam() override { D().fillScreen(BG_A); }
  void frame() override {
    moveBalls();
    for (int i = 0; i < nBalls; i++) D().fillCircle(balls[i].ox, balls[i].oy, balls[i].r, BG_A);
    for (int i = 0; i < nBalls; i++) D().fillCircle((int)balls[i].x, (int)balls[i].y, balls[i].r, balls[i].c);
    shellOverlayDirect();
  }
};

// Whole frame (pattern + all balls) rendered in strips and DMA'd. No flicker; cost is constant.
class FrameBalls : public SpriteBase {
public:
  void frame() override {
    moveBalls();
    stripFrame(true, [](Canvas& c) { bgPattern(c); drawBalls(c); });
  }
};

// Only the box around each ball's old+new position is re-rendered (pattern + overlapping
// balls) and pushed. Cheap for few sprites, gets worse as boxes multiply.
class DirtyBalls : public SpriteBase {
public:
  bool begin() override {
    bool ok = SpriteBase::begin();
    if (ok) stripFrame(true, [](Canvas& c) { bgPattern(c); drawBalls(c); });
    return ok;
  }
  void onParam() override { stripFrame(true, [](Canvas& c) { bgPattern(c); drawBalls(c); }); }
  void frame() override {
    moveBalls();
    for (int i = 0, k = 0; i < nBalls; i++, k ^= 1) {
      Ball& b = balls[i];
      int nx = (int)b.x, ny = (int)b.y;
      int x0 = min<int>(b.ox, nx) - b.r - 1, y0 = min<int>(b.oy, ny) - b.r - 1;
      int x1 = max<int>(b.ox, nx) + b.r + 2, y1 = max<int>(b.oy, ny) + b.r + 2;
      x0 = max(0, x0); y0 = max(OV_TOP, y0); x1 = min(SCR_W, x1); y1 = min(SCR_H - 24, y1);
      if (x1 <= x0 || y1 <= y0) continue;
      Canvas c; c.attach(sbuf[k], x0, y0, x1 - x0, y1 - y0);
      bgPattern(c); drawBalls(c);
      D().pushDMA(x0, y0, x1 - x0, y1 - y0, sbuf[k]);
    }
    shellOverlayDirect();
  }
  static const int OV_TOP = 18;
};

// ================================================================ EFFECTS (full frame)
class PlasmaScene : public Scene {
  bool half; uint8_t t1 = 0, t2 = 0, t3 = 0;
public:
  explicit PlasmaScene(bool h) : half(h) {}
  bool begin() override {
    initSin();
    for (int i = 0; i < 256; i++) pal[i] = hsvBE(i / 256.0f, 0.9f, 0.5f + 0.5f * sinf(i * 2 * PI / 256 * 2) * 0.5f + 0.25f);
    return allocStrips();
  }
  void frame() override {
    t1 += 2; t2 += 3; t3 += 1;
    stripFrame(true, [this](Canvas& c) {
      int step = half ? 2 : 1;
      for (int y = c.y0; y < c.y0 + c.h; y += step) {
        uint16_t* p = c.row(y);
        int sy = sinT[(uint8_t)(y * 2 + t2)] + sinT[(uint8_t)(y + t3 * 3)];
        for (int x = 0; x < SCR_W; x += step) {
          int v = sy + sinT[(uint8_t)(x * 2 + t1)] + sinT[(uint8_t)((x + y) + t1 + t3)];
          uint16_t col = pal[(uint8_t)(v >> 2)];
          p[x] = col;
          if (half) { p[x + 1] = col; p[x + SCR_W] = col; p[x + SCR_W + 1] = col; }
        }
      }
    });
  }
};

// Classic fire, simulated at 160x120 and scaled 2x.
class FireScene : public Scene {
  uint8_t* heat = nullptr;
  static const int FW = 160, FH = 120;
public:
  bool begin() override {
    heat = (uint8_t*)calloc(FW * (FH + 1), 1);
    for (int i = 0; i < 256; i++) {
      float v = i / 255.0f;
      pal[i] = rgbBE((uint8_t)min(255.0f, v * 3 * 255), (uint8_t)constrain((v * 3 - 1) * 255, 0.0f, 255.0f),
                     (uint8_t)constrain((v * 3 - 2) * 255, 0.0f, 255.0f));
    }
    return heat && allocStrips();
  }
  void end() override { freeStrips(); free(heat); heat = nullptr; }
  void frame() override {
    uint8_t* bottom = heat + FH * FW;
    for (int x = 0; x < FW; x++) bottom[x] = (esp_random() & 3) ? 255 : 60;
    for (int y = 0; y < FH; y++) {
      uint8_t* r = heat + y * FW; const uint8_t* b = r + FW;
      const uint8_t* b2 = (y + 2 <= FH) ? b + FW : b;
      for (int x = 0; x < FW; x++) {
        int s = b[(x + FW - 1) % FW] + b[x] + b[(x + 1) % FW] + b2[x];
        int v = (s * 31) >> 7;           // /4 * 0.97
        r[x] = v > 1 ? v - 1 : 0;
      }
    }
    stripFrame(true, [this](Canvas& c) {
      for (int y = c.y0; y < c.y0 + c.h; y++) {
        uint16_t* p = c.row(y); const uint8_t* h = heat + (y >> 1) * FW;
        for (int x = 0; x < FW; x++) { uint16_t col = pal[h[x]]; p[2 * x] = col; p[2 * x + 1] = col; }
      }
    });
  }
};

class StarScene : public Scene {
  struct Star { float x, y, z; };
  Star* st = nullptr; static const int N = 600;
  int16_t* sx = nullptr; int16_t* sy = nullptr; uint8_t* sz = nullptr;
  void respawn(Star& s) { s.x = (int)(esp_random() % 2000) - 1000; s.y = (int)(esp_random() % 2000) - 1000; s.z = 200 + esp_random() % 800; }
public:
  bool begin() override {
    st = (Star*)malloc(N * sizeof(Star)); sx = (int16_t*)malloc(N * 2); sy = (int16_t*)malloc(N * 2); sz = (uint8_t*)malloc(N);
    if (!st || !sx || !sy || !sz) return false;
    for (int i = 0; i < N; i++) respawn(st[i]);
    return allocStrips();
  }
  void end() override { freeStrips(); free(st); free(sx); free(sy); free(sz); st = nullptr; sx = sy = nullptr; sz = nullptr; }
  void frame() override {
    for (int i = 0; i < N; i++) {
      Star& s = st[i];
      s.z -= 8;
      if (s.z < 10) respawn(s);
      sx[i] = SCR_W / 2 + (int)(s.x * 128 / s.z);
      sy[i] = SCR_H / 2 + (int)(s.y * 128 / s.z);
      sz[i] = s.z < 250 ? 3 : (s.z < 550 ? 2 : 1);
      if (sx[i] < 0 || sx[i] >= SCR_W || sy[i] < 0 || sy[i] >= SCR_H) respawn(s);
    }
    stripFrame(true, [this](Canvas& c) {
      c.clear(0);
      for (int i = 0; i < N; i++) {
        if (sy[i] < c.y0 - 3 || sy[i] >= c.y0 + c.h) continue;
        uint8_t v = sz[i] == 3 ? 255 : (sz[i] == 2 ? 170 : 90);
        c.fillRect(sx[i], sy[i], sz[i], sz[i], rgbBE(v, v, v));
      }
    });
  }
};

// Rotating + zooming 64x64 texture, every pixel sampled.
class RotoScene : public Scene {
  uint16_t* tex = nullptr; float a = 0;
public:
  bool begin() override {
    tex = (uint16_t*)malloc(64 * 64 * 2);
    if (!tex) return false;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
      uint8_t v = (uint8_t)((x ^ y) * 4);
      bool chk = ((x >> 4) ^ (y >> 4)) & 1;
      tex[y * 64 + x] = chk ? rgbBE(v, 80, 255 - v) : rgbBE(255, v, 40);
    }
    return allocStrips();
  }
  void end() override { freeStrips(); free(tex); tex = nullptr; }
  void frame() override {
    a += 0.03f;
    float zoom = 1.2f + 0.8f * sinf(a * 0.7f);
    int32_t du = (int32_t)(cosf(a) * zoom * 256), dv = (int32_t)(sinf(a) * zoom * 256);
    stripFrame(true, [=](Canvas& c) {
      for (int y = c.y0; y < c.y0 + c.h; y++) {
        uint16_t* p = c.row(y);
        int32_t u = (-SCR_W / 2) * du - (y - SCR_H / 2) * dv;
        int32_t v = (-SCR_W / 2) * dv + (y - SCR_H / 2) * du;
        for (int x = 0; x < SCR_W; x++) {
          p[x] = tex[(((v >> 8) & 63) << 6) | ((u >> 8) & 63)];
          u += du; v += dv;
        }
      }
    });
  }
};

// ================================================================ UI-style
// Gauge: only the dial box is redrawn each frame (partial update).
class GaugeScene : public Scene {
  float t = 0;
  static const int BX = 70, BY = 22, BW = 180, BH = 180, CX = BX + 90, CY = BY + 95, R = 84;
public:
  bool begin() override { bool ok = allocStrips(); D().fillScreen(0); return ok; }
  void frame() override {
    t += 0.04f;
    float v = 0.5f + 0.5f * sinf(t) * cosf(t * 0.37f);      // 0..1
    float ang = (-225 + 270 * v) * DEG_TO_RAD;
    int nx = CX + (int)(cosf(ang) * (R - 14)), ny = CY + (int)(sinf(ang) * (R - 14));
    char num[8]; snprintf(num, sizeof num, "%3d", (int)(v * 100));
    regionFrame(BX, BY, BW, BH, [&](Canvas& c) {
      c.clear(0);
      c.fillRing(CX, CY, R - 8, R, rgbBE(50, 56, 80));
      for (int i = 0; i <= 10; i++) {                          // ticks
        float a2 = (-225 + 27 * i) * DEG_TO_RAD;
        c.thickLine(CX + (int)(cosf(a2) * (R - 18)), CY + (int)(sinf(a2) * (R - 18)),
                    CX + (int)(cosf(a2) * (R - 10)), CY + (int)(sinf(a2) * (R - 10)), 2, rgbBE(200, 200, 210));
      }
      uint16_t nc = v > 0.8f ? rgbBE(255, 80, 60) : rgbBE(255, 203, 45);
      c.thickLine(CX, CY, nx, ny, 4, nc);
      c.fillCircle(CX, CY, 8, rgbBE(220, 220, 230));
      c.text(CX - 27, CY + 30, num, 0xFFFF, 3);
    });
    shellOverlayDirect();
  }
};

// Eye (like CYD-hal9000): pupil wanders, eyelids blink. Partial update of a 170x170 box.
class EyeScene : public Scene {
  float t = 0; uint32_t nextBlink = 0;
  static const int BX = 75, BY = 30, BW = 170, BH = 170, CX = BX + 85, CY = BY + 85;
public:
  bool begin() override { bool ok = allocStrips(); D().fillScreen(0); nextBlink = millis() + 2000; return ok; }
  void frame() override {
    t += 0.05f;
    int px = CX + (int)(28 * sinf(t * 1.3f)), py = CY + (int)(20 * sinf(t * 0.9f + 1));
    uint32_t now = millis();
    float lid = 0;                                            // 0 open .. 1 closed
    if (now > nextBlink) {
      float k = (now - nextBlink) / 250.0f;
      if (k >= 1) { nextBlink = now + 2500 + esp_random() % 3000; } else lid = k < 0.5f ? k * 2 : (1 - k) * 2;
    }
    int lidH = (int)(lid * BH / 2);
    regionFrame(BX, BY, BW, BH, [&](Canvas& c) {
      c.clear(0);
      c.fillCircle(CX, CY, 80, rgbBE(40, 40, 46));
      c.fillCircle(CX, CY, 74, rgbBE(235, 235, 240));
      c.fillCircle(px, py, 38, rgbBE(200, 30, 20));
      c.fillCircle(px, py, 30, rgbBE(255, 70, 40));
      c.fillCircle(px, py, 15, 0);
      c.fillCircle(px - 10, py - 12, 6, 0xFFFF);
      if (lidH) { c.fillRect(BX, BY, BW, lidH, 0); c.fillRect(BX, BY + BH - lidH, BW, lidH, 0); }
    });
    shellOverlayDirect();
  }
};

// Smooth-scrolling list, full frame.
class ListScene : public Scene {
  int off = 0;
  static const int ROW = 30, N = 40;
public:
  void frame() override {
    off = (off + 2) % (ROW * N);
    stripFrame(true, [this](Canvas& c) {
      int first = (c.y0 + off) / ROW;
      for (int r = first; r * ROW - off < c.y0 + c.h; r++) {
        int y = r * ROW - off, idx = r % N;
        c.fillRect(0, y, SCR_W, ROW, (idx & 1) ? rgbBE(26, 30, 48) : rgbBE(20, 24, 38));
        c.fillRect(0, y + ROW - 1, SCR_W, 1, rgbBE(50, 56, 80));
        c.fillCircle(20, y + ROW / 2, 8, hsvBE(idx / (float)N, 0.7f, 1));
        char t[32]; snprintf(t, sizeof t, "Item %02d   value %4d", idx, idx * 37 % 1000);
        c.text(40, y + 8, t, rgbBE(235, 238, 245), 2);
      }
    });
  }
  String metric() override { return mbps(); }
};

// Big counter: text rendering in a 300x64 box.
class CounterScene : public Scene {
  uint32_t n = 0;
public:
  bool begin() override { bool ok = allocStrips(); D().fillScreen(0); return ok; }
  void frame() override {
    char t[12]; snprintf(t, sizeof t, "%06u", (unsigned)(n++ % 1000000));
    regionFrame(10, 88, 300, 64, [&](Canvas& c) {
      c.clear(rgbBE(10, 12, 20));
      c.text(16, 88, t, rgbBE(255, 203, 45), 8);
    });
    shellOverlayDirect();
  }
};

// ================================================================ the experiment
enum { G_RAW, G_SPRITES, G_FX, G_UI };
static const char* GROUPS[] = {"Raw", "Balls", "FX", "UI"};

static FillScene    sFill;
static StripScene   sStrip(false), sStripDma(true);
static RectScene    sRect;
static SweepScene   sSweep;
static DirectBalls  sDirect;
static FrameBalls   sFrame;
static DirtyBalls   sDirty;
static PlasmaScene  sPlasma(false), sPlasmaHalf(true);
static FireScene    sFire;
static StarScene    sStars;
static RotoScene    sRoto;
static GaugeScene   sGauge;
static EyeScene     sEye;
static ListScene    sList;
static CounterScene sCounter;

struct Entry { uint8_t g; const char* name; Scene* s; bool needsSpi; };
static const Entry SCENES[] = {
  {G_RAW, "Fill screen", &sFill, false},
  {G_RAW, "Strips", &sStrip, false},
  {G_RAW, "Strips DMA", &sStripDma, false},
  {G_RAW, "Rects 32px", &sRect, false},
  {G_RAW, "SPI sweep", &sSweep, true},
  {G_SPRITES, "Direct", &sDirect, false},
  {G_SPRITES, "Full frame", &sFrame, false},
  {G_SPRITES, "Dirty rects", &sDirty, false},
  {G_FX, "Plasma", &sPlasma, false},
  {G_FX, "Plasma 1/2", &sPlasmaHalf, false},
  {G_FX, "Fire", &sFire, false},
  {G_FX, "Starfield", &sStars, false},
  {G_FX, "Rotozoom", &sRoto, false},
  {G_UI, "Gauge", &sGauge, false},
  {G_UI, "Eye", &sEye, false},
  {G_UI, "Scroll list", &sList, false},
  {G_UI, "Counter", &sCounter, false},
};
static const int NS = sizeof(SCENES) / sizeof(SCENES[0]);

class GfxExperiment : public Experiment {
  int map_[NS]; int n = 0; Scene* cur = nullptr;
public:
  void build() {
    n = 0;
    for (int i = 0; i < NS; i++) if (!SCENES[i].needsSpi || D().spiAdjustable()) map_[n++] = i;
  }
  int groupCount() override { return 4; }
  const char* groupName(int g) override { return GROUPS[g]; }
  int sceneCount() override { return n; }
  SceneInfo scene(int i) override { return {SCENES[map_[i]].g, SCENES[map_[i]].name}; }
  bool start(int i) override {
    cur = SCENES[map_[i]].s;
    D().startWrite();
    return cur->begin();
  }
  void stop() override {
    if (cur) cur->end();
    D().endWrite();
    cur = nullptr;
  }
  int frame() override { cur->frame(); return 1; }
  bool hasParam() override { return cur && cur->hasParam(); }
  void param(int d) override { if (cur) cur->param(d); }
  String paramLabel() override { return cur ? cur->paramLabel() : ""; }
  String metric() override { return cur ? cur->metric() : ""; }
};

static GfxExperiment gfx;

Experiment& gfxExperiment() { gfx.build(); return gfx; }
#endif
