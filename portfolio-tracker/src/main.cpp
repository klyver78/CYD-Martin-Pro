// PORTFOLIO TRACKER on the CYD: TFT_eSPI at 80 MHz SPI, the whole frame rendered in 320x40
// strips and sent with double-buffered DMA (the CPU renders strip n+1 while DMA sends strip n).
//
// Holdings come from portfolio.json on GitHub Pages; prices come from one call to the Yahoo
// Finance spark endpoint. All drawing lives in ui.cpp, all maths in portfolio.cpp.
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

#include "board.h"
#include "config.h"
#include "net.h"
#include "portfolio.h"
#include "ui.h"

#if __has_include("build_version.h")
#include "build_version.h"            // generated in CI from VERSION
#endif
#ifndef FW_VERSION_SEM
#define FW_VERSION_SEM "0.0.0-dev"
#endif

static TFT_eSPI            tft;
static SPIClass            touchSPI(VSPI);
static XPT2046_Touchscreen touch(XPT2046_CS, XPT2046_IRQ);

static uint16_t stripBuf[2][SCR_W * STRIP_H];   // 2 x 25.6 KB, static = DMA capable

static void renderFrame() {
  for (int y = 0, k = 0; y < SCR_H; y += STRIP_H, k ^= 1) {
    Canvas c;
    c.attach(stripBuf[k], 0, y, SCR_W, STRIP_H);
    uiRender(c);
    tft.pushImageDMA(0, y, SCR_W, STRIP_H, stripBuf[k]);   // waits for the previous strip first
  }
}

// ---- touch: resistive panels drop out for a frame or two, so a release only counts after a gap
static bool     touchDown = false;
static uint32_t lastTouchMs = 0;
static int      heldX = 0, heldY = 0;

static void readTouch() {
  uint32_t now = millis();
  if (touch.touched()) {
    TS_Point p = touch.getPoint();
    if (p.z >= 200) {
      int x = map(p.x, TOUCH_MINX, TOUCH_MAXX, 0, SCR_W - 1);
      int y = map(p.y, TOUCH_MINY, TOUCH_MAXY, 0, SCR_H - 1);
      if (TOUCH_FLIP_X) x = SCR_W - 1 - x;
      if (TOUCH_FLIP_Y) y = SCR_H - 1 - y;
      heldX = constrain(x, 0, SCR_W - 1);
      heldY = constrain(y, 0, SCR_H - 1);
      touchDown = true;
      lastTouchMs = now;
    }
  } else if (touchDown && now - lastTouchMs > 60) {
    touchDown = false;
  }
  uiTouch(touchDown, heldX, heldY, now);
}

// ---- data
static uint32_t lastFetchMs = 0;        // 0 = never
static uint32_t nextTryMs = 0;
static bool     havePortfolio = false;

// Re-reads portfolio.json every time (so edits show up without a reflash), then the prices.
// If portfolio.json cannot be fetched, the holdings we already have are kept.
static void refresh() {
  uiSetBusy(true);
  renderFrame();
  if (netLoadPortfolio()) havePortfolio = true;
  if (havePortfolio) {
    netResolveSymbols();
    if (netFetchPrices()) {
      lastFetchMs = millis();
      if (uiView() == UV_LOADING) uiSetView(UV_LIST);
    }
  }
  uiSetBusy(false);
  // Retry sooner than the normal interval while we still have nothing to show.
  nextTryMs = millis() + (lastFetchMs ? REFRESH_MS : 30000UL);
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);                      // landscape 320 x 240
  tft.setSwapBytes(false);                 // strips are already in wire byte order
  tft.initDMA();
  tft.fillScreen(TFT_BLACK);
  tft.startWrite();                        // keep CS low for DMA; held for the whole run

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touch.begin(touchSPI);
  touch.setRotation(1);

  pfReset();
  gVersion = FW_VERSION_SEM;
  uiInit();
  uiSetStatus("Starting...");
  renderFrame();

  netSetup("cyd-portfolio-tracker", FW_VERSION_SEM, renderFrame);
  Serial.printf("PORTFOLIO TRACKER v%s, SPI %u Hz\n", FW_VERSION_SEM, (unsigned)SPI_FREQUENCY);
  refresh();
}

void loop() {
  static uint32_t lastUs = micros();
  netLoop();
  readTouch();

  uint32_t now = micros();
  float dt = (now - lastUs) / 1e6f;
  lastUs = now;
  uiUpdate(dt);
  uiSetAge(lastFetchMs ? (int)((millis() - lastFetchMs) / 1000) : -1);

  switch (uiTakeAction()) {
    case UA_REFRESH: refresh(); lastUs = micros(); break;
    case UA_UPDATE:  netCheckAndUpdate(FW_VERSION_SEM); lastUs = micros(); break;
    default: break;
  }

  if ((int32_t)(millis() - nextTryMs) >= 0) {
    refresh();
    lastUs = micros();
  }

  renderFrame();
}
