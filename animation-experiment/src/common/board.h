// Board constants for the CYD (ESP32-2432S028R) as used in Martin's other CYD projects:
// ILI9341_2-compatible panel, BGR, inversion ON, landscape 320x240.
#pragma once
#include <stdint.h>

#define SCR_W 320
#define SCR_H 240

// ---- display (HSPI, hardwired on the CYD) ----
#define PIN_TFT_MISO 12
#define PIN_TFT_MOSI 13
#define PIN_TFT_SCLK 14
#define PIN_TFT_CS   15
#define PIN_TFT_DC    2
#define PIN_TFT_BL   21

// ---- resistive touch (XPT2046) on its own SPI bus (VSPI) ----
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define TOUCH_MINX   200
#define TOUCH_MAXX   3700
#define TOUCH_MINY   240
#define TOUCH_MAXY   3800
#define TOUCH_FLIP_X 0     // set to 1 if left/right taps are mirrored
#define TOUCH_FLIP_Y 0     // set to 1 if up/down taps are mirrored

// ---- GitHub Pages: ROM list + OTA images ----
#define PAGES_BASE   "https://martindamp.github.io/CYD-experiments/animation-experiment/"
#define ROMS_URL     PAGES_BASE "roms.json"

// Strip height used for banded full-frame rendering. 320 x 40 x 2 bytes = 25.6 KB per buffer;
// two buffers (double-buffered DMA) = 51.2 KB. A full 320x240 16-bit frame (153.6 KB) does not
// fit in one contiguous block of internal RAM on an ESP32 without PSRAM - that's the core
// constraint all the "full-frame" experiments work around.
#define STRIP_H      40

// RGB565 helpers. Pixel buffers in this project are stored byte-swapped ("big-endian"), which
// is the byte order the panel wants on the wire, so DMA can send them without conversion.
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint16_t swap16(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }
static inline uint16_t rgbBE(uint8_t r, uint8_t g, uint8_t b) { return swap16(rgb565(r, g, b)); }
