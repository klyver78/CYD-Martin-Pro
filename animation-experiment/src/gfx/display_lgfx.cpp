// Display backend: LovyanGFX. Configured to send exactly the same panel setup as the
// TFT_eSPI builds (ILI9341_2 init, BGR, inversion on, MADCTL = MV for landscape).
// Unlike TFT_eSPI, the SPI clock can be changed at runtime - used by the "SPI sweep" scene.
#ifdef GFX_LGFX
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "../common/display.h"
#include "../common/board.h"

#ifndef LGFX_SPI_HZ
#define LGFX_SPI_HZ 40000000
#endif

class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Panel_ILI9341_2 panel;
  lgfx::Bus_SPI         bus;
  LGFX() {
    auto b = bus.config();
    b.spi_host    = HSPI_HOST;
    b.spi_mode    = 0;
    b.freq_write  = LGFX_SPI_HZ;
    b.freq_read   = 16000000;
    b.spi_3wire   = false;
    b.use_lock    = true;
    b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk    = PIN_TFT_SCLK;
    b.pin_mosi    = PIN_TFT_MOSI;
    b.pin_miso    = PIN_TFT_MISO;
    b.pin_dc      = PIN_TFT_DC;
    bus.config(b);
    panel.setBus(&bus);

    auto p = panel.config();
    p.pin_cs          = PIN_TFT_CS;
    p.pin_rst         = -1;
    p.pin_busy        = -1;
    p.panel_width     = 240;
    p.panel_height    = 320;
    p.offset_x        = 0;
    p.offset_y        = 0;
    p.offset_rotation = 0;
    p.readable        = true;
    p.invert          = true;     // = TFT_INVERSION_ON
    p.rgb_order       = false;    // false = BGR (= TFT_RGB_ORDER=TFT_BGR)
    p.dlen_16bit      = false;
    p.bus_shared      = false;
    panel.config(p);
    setPanel(&panel);
  }
};

static LGFX lcd;

// ESP32 SPI clock = 80 MHz / ((pre+1) * (n+1)) - same maths LovyanGFX uses (FreqToClockDiv).
static uint32_t actualHz(uint32_t hz) {
  const uint32_t apb = 80000000;
  if (hz >= apb) return apb;
  uint32_t div = apb / (1 + hz);
  uint32_t pre = div / 64u;
  div = div / (pre + 1);
  return apb / ((pre + 1) * (div + 1));
}

class LgfxDisplay : public Display {
  uint32_t hz_ = LGFX_SPI_HZ;
public:
  void begin() override {
    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);
    lcd.init();
    lcd.setRotation(5);       // internal rotation 5 = MADCTL MV only = TFT_eSPI rotation 1
    lcd.fillScreen((uint16_t)0);
  }
  const char* libName() const override { return "LovyanGFX"; }

  void fillScreen(uint16_t c) override { lcd.fillScreen(c); }
  void fillRect(int x, int y, int w, int h, uint16_t c) override { lcd.fillRect(x, y, w, h, c); }
  void fillCircle(int x, int y, int r, uint16_t c) override { lcd.fillCircle(x, y, r, c); }

  void push(int x, int y, int w, int h, const uint16_t* be) override {
    lcd.waitDMA();
    lcd.pushImage(x, y, w, h, (const lgfx::swap565_t*)be);
  }
  void pushDMA(int x, int y, int w, int h, const uint16_t* be) override {
    lcd.pushImageDMA(x, y, w, h, (const lgfx::swap565_t*)be);
  }
  void waitDMA() override { lcd.waitDMA(); }
  bool hasDMA() const override { return true; }

  void startWrite() override { lcd.startWrite(); }
  void endWrite() override { lcd.waitDMA(); lcd.endWrite(); }

  uint32_t spiHz() const override { return actualHz(hz_); }
  bool spiAdjustable() const override { return true; }
  bool setSpiHz(uint32_t hz) override {
    lcd.waitDMA();
    auto b = lcd.bus.config();
    b.freq_write = hz;
    lcd.bus.config(b);        // takes effect at the next transaction
    hz_ = hz;
    return true;
  }
};

static LgfxDisplay disp;
Display& display() { return disp; }
#endif
