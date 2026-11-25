// Enough of TFT_eSPI for the simulator to compile main.cpp; it draws nothing.
#pragma once
#include "Arduino.h"
#define TFT_BLACK 0
#ifndef SPI_FREQUENCY
#define SPI_FREQUENCY 80000000
#endif
struct TFT_eSPI {
  void init() {}
  void setRotation(int) {}
  void setSwapBytes(bool) {}
  void initDMA() {}
  void fillScreen(uint16_t) {}
  void startWrite() {}
  void pushImageDMA(int32_t, int32_t, int32_t, int32_t, uint16_t*) {}
};
