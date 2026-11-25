// CYD (ESP32-2432S028R) constants - same panel/touch setup as the other experiments here.
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
#define TOUCH_MINY   240
#define TOUCH_MAXY   3800
#define TOUCH_FLIP_X 0       // set to 1 if left/right are mirrored
#define TOUCH_FLIP_Y 0

// ---- GitHub Pages: portfolio definition + OTA ----
#define PAGES_BASE     "https://martindamp.github.io/CYD-experiments/portfolio-tracker/"
#define ROMS_URL       PAGES_BASE "roms.json"
#define PORTFOLIO_URL  PAGES_BASE "portfolio.json"
#define OTA_BIN_PATH   "roms/tracker/firmware.bin"

// ---- price API (Yahoo Finance, no API key) ----
// One spark call returns daily closes for every symbol at once; chart/search are only used
// once per holding to resolve an ISIN and learn the quote currency (both cached in NVS).
#define API_HOST       "query1.finance.yahoo.com"
#define API_SPARK_PATH "/v8/finance/spark?interval=1d&range=1mo&symbols="
#define API_CHART_PATH "/v8/finance/chart/"
#define API_SEARCH_PATH "/v1/finance/search?quotesCount=4&newsCount=0&q="

#define REFRESH_MS     (5 * 60 * 1000UL)   // re-fetch prices this often

// Byte-swapped RGB565 (wire order), so strips can be DMA'd as-is.
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static inline uint16_t swap16(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }
static inline uint16_t rgbBE(uint8_t r, uint8_t g, uint8_t b) { return swap16(rgb565(r, g, b)); }
