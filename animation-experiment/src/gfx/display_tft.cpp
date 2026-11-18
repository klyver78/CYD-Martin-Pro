// Display backend: TFT_eSPI. Panel setup comes from build flags in platformio.ini.
// The SPI clock is fixed at compile time (SPI_FREQUENCY) - that's why there are two TFT_eSPI
// ROMs (default clock and 80 MHz).
#ifdef GFX_TFT
#include <TFT_eSPI.h>
#include "../common/display.h"
#include "../common/board.h"

static TFT_eSPI tft;
TFT_eSPI& tftRef() { return tft; }   // used by the LVGL ROM's flush callback

class TftDisplay : public Display {
public:
  void begin() override {
    tft.init();
    tft.setRotation(1);                 // landscape 320 x 240
    tft.setSwapBytes(false);            // our buffers are already in wire byte order
    tft.initDMA();
    tft.fillScreen(TFT_BLACK);
  }
  const char* libName() const override { return "TFT_eSPI"; }

  void fillScreen(uint16_t c) override { tft.dmaWait(); tft.fillScreen(c); }
  void fillRect(int x, int y, int w, int h, uint16_t c) override { tft.dmaWait(); tft.fillRect(x, y, w, h, c); }
  void fillCircle(int x, int y, int r, uint16_t c) override { tft.dmaWait(); tft.fillCircle(x, y, r, c); }

  void push(int x, int y, int w, int h, const uint16_t* be) override {
    tft.dmaWait();
    tft.pushImage(x, y, w, h, be);
  }
  void pushDMA(int x, int y, int w, int h, const uint16_t* be) override {
    // waits for the previous DMA transfer itself, then queues this one and returns
    tft.pushImageDMA(x, y, w, h, const_cast<uint16_t*>(be));
  }
  void waitDMA() override { tft.dmaWait(); }
  bool hasDMA() const override { return true; }

  void startWrite() override { tft.startWrite(); }
  void endWrite() override { tft.dmaWait(); tft.endWrite(); }

  uint32_t spiHz() const override { return spiClockDivToFrequency(spiFrequencyToClockDiv(SPI_FREQUENCY)); }
};

static TftDisplay disp;
Display& display() { return disp; }
#endif
