// STAR RING on the CYD: TFT_eSPI at 80 MHz SPI, whole frame rendered in 320x40 strips and sent
// with double-buffered DMA (the CPU renders strip n+1 while DMA sends strip n).
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include "board.h"
#include "game.h"
#include "net.h"

#if __has_include("build_version.h")
#include "build_version.h"            // generated in CI from VERSION
#endif
#ifndef FW_VERSION_SEM
#define FW_VERSION_SEM "0.0.0-dev"
#endif

static TFT_eSPI            tft;
static SPIClass            touchSPI(VSPI);
static XPT2046_Touchscreen touch(XPT2046_CS, XPT2046_IRQ);
static Preferences         prefs;

static uint16_t stripBuf[2][SCR_W * STRIP_H];   // 2 x 25.6 KB, static = DMA capable

static void renderFrame() {
  for (int y = 0, k = 0; y < SCR_H; y += STRIP_H, k ^= 1) {
    Canvas c; c.attach(stripBuf[k], 0, y, SCR_W, STRIP_H);
    gameRender(c);
    tft.pushImageDMA(0, y, SCR_W, STRIP_H, stripBuf[k]);   // waits for the previous strip first
  }
}

// ---- touch: held side -> turn direction; new press -> tap
static bool     touchDown = false;
static uint32_t lastTouchMs = 0;
static int      heldX = 0, heldY = 0;
static void readTouch(GameInput& in) {
  bool t = touch.touched();
  uint32_t now = millis();
  if (t) {
    TS_Point p = touch.getPoint();
    int x = map(p.x, TOUCH_MINX, TOUCH_MAXX, 0, SCR_W - 1);
    if (TOUCH_FLIP_X) x = SCR_W - 1 - x;
    heldX = constrain(x, 0, SCR_W - 1);
    heldY = constrain((int)map(p.y, 240, 3800, 0, SCR_H - 1), 0, SCR_H - 1);
    if (!touchDown) { in.tap = true; in.tapX = heldX; in.tapY = heldY; }
    touchDown = true;
    lastTouchMs = now;
  } else if (touchDown && now - lastTouchMs > 80) {
    touchDown = false;                     // released (short gaps are ignored - resistive touch)
  }
  if (touchDown) in.turn = heldX < SCR_W / 2 ? -1 : 1;
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

  prefs.begin("game", true);
  gameSetBest(prefs.getUInt("best", 0));
  prefs.end();

  gVersion = FW_VERSION_SEM;
  gameInit(esp_random());
  netSetup("cyd-star-ring", FW_VERSION_SEM, renderFrame);
  Serial.printf("STAR RING v%s, SPI %u Hz\n", FW_VERSION_SEM, (unsigned)SPI_FREQUENCY);
}

void loop() {
  static uint32_t lastUs = micros(), fpsT0 = millis(), frames = 0;
  static GameState prev = GS_TITLE;
  netLoop();

  GameInput in;
  readTouch(in);
  if (in.tap && gameTitleUpdateHit(in.tapX, in.tapY)) {
    netCheckAndUpdate(FW_VERSION_SEM);     // only returns if nothing was installed
    in.tap = false;
  }

  uint32_t now = micros();
  float dt = (now - lastUs) / 1e6f;
  lastUs = now;
  gameUpdate(in, dt);

  GameState st = gameState();
  if (st == GS_OVER && prev != GS_OVER) {  // save best score
    prefs.begin("game", false);
    prefs.putUInt("best", gameBest());
    prefs.end();
  }
  prev = st;

  renderFrame();

  frames++;
  if (millis() - fpsT0 >= 500) {
    snprintf(gFpsText, sizeof gFpsText, "%.0f fps", frames * 1000.0f / (millis() - fpsT0));
    frames = 0;
    fpsT0 = millis();
  }
}
