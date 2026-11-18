// Library-agnostic display interface. Each ROM implements it with one graphics library
// (TFT_eSPI or LovyanGFX). The shell and the software-rendered scenes only use this, so the
// exact same scene code is measured on every library.
#pragma once
#include <stdint.h>

class Display {
public:
  virtual ~Display() {}
  virtual void        begin() = 0;
  virtual const char* libName() const = 0;

  // Native library primitives (colour = normal RGB565, NOT swapped).
  virtual void fillScreen(uint16_t c) = 0;
  virtual void fillRect(int x, int y, int w, int h, uint16_t c) = 0;
  virtual void fillCircle(int x, int y, int r, uint16_t c) = 0;

  // Push a block of byte-swapped RGB565 pixels (see board.h).
  virtual void push(int x, int y, int w, int h, const uint16_t* be) = 0;     // blocking
  virtual void pushDMA(int x, int y, int w, int h, const uint16_t* be) = 0;  // returns at once
  virtual void waitDMA() = 0;
  virtual bool hasDMA() const = 0;

  // Hold the SPI bus / chip select across many calls (needed for DMA, faster in general).
  virtual void startWrite() = 0;
  virtual void endWrite() = 0;

  // SPI write clock. setSpiHz returns false if the library can't change it at runtime.
  virtual uint32_t spiHz() const = 0;
  virtual bool     spiAdjustable() const { return false; }
  virtual bool     setSpiHz(uint32_t hz) { (void)hz; return false; }
};

Display& display();   // provided by the ROM
