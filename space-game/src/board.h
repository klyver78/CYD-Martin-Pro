// CYD (ESP32-2432S028R) constants - same panel/touch setup as animation-experiment.
#pragma once
#include <stdint.h>

#define SCR_W 320
#define SCR_H 240
#define STRIP_H 40           // frame is rendered in 6 strips of 320x40, double-buffered DMA

// ---- resistive touch (XPT2046) on VSPI ----
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33
#define TOUCH_MINX   200
#define TOUCH_MAXX   3700
#define TOUCH_FLIP_X 0       // set to 1 if left/right are mirrored

// ---- GitHub Pages: OTA ----
#define PAGES_BASE   "https://martindamp.github.io/CYD-experiments/space-game/"
#define ROMS_URL     PAGES_BASE "roms.json"
#define OTA_BIN_PATH "roms/game/firmware.bin"

// Byte-swapped RGB565 (wire order), so strips can be DMA'd as-is.
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint16_t swap16(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }
static inline uint16_t rgbBE(uint8_t r, uint8_t g, uint8_t b) { return swap16(rgb565(r, g, b)); }
