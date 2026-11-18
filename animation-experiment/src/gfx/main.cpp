#ifndef GFX_LVGL   // the LVGL ROM has its own main + scenes (src/lvgl)
// Entry point for the TFT_eSPI and LovyanGFX ROMs (backend picked with -D GFX_TFT / GFX_LGFX).
#include <Arduino.h>
#include "../common/shell.h"

Experiment& gfxExperiment();

void setup() {
  Serial.begin(115200);
  display().begin();
  shellSetup(gfxExperiment());
}

void loop() { shellLoop(); }
#endif
