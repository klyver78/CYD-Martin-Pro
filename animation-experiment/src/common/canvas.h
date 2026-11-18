// Tiny software renderer that draws into a pixel buffer covering part of the screen (usually
// a horizontal strip). Coordinates are SCREEN coordinates; everything is clipped to the
// canvas. Colours are byte-swapped RGB565 (rgbBE / swap16, see board.h).
#pragma once
#include <Arduino.h>
#include "board.h"
#include "font5x7.h"

struct Canvas {
  uint16_t* buf = nullptr;
  int x0 = 0, y0 = 0, w = 0, h = 0;

  void attach(uint16_t* b, int x, int y, int ww, int hh) { buf = b; x0 = x; y0 = y; w = ww; h = hh; }
  inline uint16_t* row(int y) { return buf + (y - y0) * w - x0; }   // index with screen x

  void clear(uint16_t c) {
    if (c == 0) { memset(buf, 0, (size_t)w * h * 2); return; }
    uint32_t c2 = ((uint32_t)c << 16) | c, *p = (uint32_t*)buf;
    for (int i = (w * h) >> 1; i > 0; --i) *p++ = c2;
    if ((w * h) & 1) buf[w * h - 1] = c;
  }

  void fillRect(int x, int y, int ww, int hh, uint16_t c) {
    int xa = max(x, x0), xb = min(x + ww, x0 + w);
    int ya = max(y, y0), yb = min(y + hh, y0 + h);
    if (xa >= xb || ya >= yb) return;
    for (int yy = ya; yy < yb; ++yy) {
      uint16_t* p = row(yy) + xa;
      for (int n = xb - xa; n > 0; --n) *p++ = c;
    }
  }

  void hline(int x, int y, int len, uint16_t c) { fillRect(x, y, len, 1, c); }

  void rect(int x, int y, int ww, int hh, uint16_t c) {
    fillRect(x, y, ww, 1, c); fillRect(x, y + hh - 1, ww, 1, c);
    fillRect(x, y, 1, hh, c); fillRect(x + ww - 1, y, 1, hh, c);
  }

  void fillCircle(int cx, int cy, int r, uint16_t c) {
    int ya = max(cy - r, y0), yb = min(cy + r, y0 + h - 1);
    for (int yy = ya; yy <= yb; ++yy) {
      int dy = yy - cy;
      int dx = (int)sqrtf((float)(r * r - dy * dy));
      hline(cx - dx, yy, 2 * dx + 1, c);
    }
  }

  // Ring between radius r0 and r1 (used for gauge dials / iris).
  void fillRing(int cx, int cy, int r0, int r1, uint16_t c) {
    int ya = max(cy - r1, y0), yb = min(cy + r1, y0 + h - 1);
    for (int yy = ya; yy <= yb; ++yy) {
      int dy = yy - cy;
      int o = (int)sqrtf((float)(r1 * r1 - dy * dy));
      if (abs(dy) >= r0) { hline(cx - o, yy, 2 * o + 1, c); continue; }
      int i = (int)sqrtf((float)(r0 * r0 - dy * dy));
      hline(cx - o, yy, o - i, c);
      hline(cx + i + 1, yy, o - i, c);
    }
  }

  // Thick line as a series of filled squares (cheap, good enough for needles).
  void thickLine(int xa, int ya, int xb, int yb, int t, uint16_t c) {
    int dx = abs(xb - xa), dy = abs(yb - ya);
    int n = max(dx, dy); if (n == 0) n = 1;
    for (int i = 0; i <= n; ++i) {
      int x = xa + (xb - xa) * i / n, y = ya + (yb - ya) * i / n;
      fillRect(x - t / 2, y - t / 2, t, t, c);
    }
  }

  // 5x7 font, 6x8 cell per character, scaled by `size`. bg == fg means transparent.
  int text(int x, int y, const char* s, uint16_t fg, int size = 1, int bg = -1) {
    for (; *s; ++s, x += 6 * size) {
      if (x >= x0 + w) break;
      if (x + 6 * size <= x0 || y >= y0 + h || y + 8 * size <= y0) continue;
      if (bg >= 0) fillRect(x, y, 6 * size, 8 * size, (uint16_t)bg);
      const uint8_t* g = FONT5X7 + (uint8_t)*s * 5;
      for (int col = 0; col < 5; ++col) {
        uint8_t bits = g[col];
        for (int r = 0; r < 8; ++r)
          if (bits & (1 << r)) fillRect(x + col * size, y + r * size, size, size, fg);
      }
    }
    return x;
  }
  static int textW(const char* s, int size) { return (int)strlen(s) * 6 * size; }
};
