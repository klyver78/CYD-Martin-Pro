// The shell is shared by every ROM: touch, Wi-Fi (Improv), the menu, the FPS overlay, the
// "Bench" run-all, the info page and the ROM switcher (fetches roms.json from GitHub Pages and
// installs the chosen ROM over the air).
#pragma once
#include "experiment.h"
#include "canvas.h"
#include "display.h"

#ifndef ROM_ID
#define ROM_ID "dev"
#endif
#ifndef ROM_NAME
#define ROM_NAME "Animation experiment"
#endif
#if __has_include("build_version.h")
#include "build_version.h"            // generated in CI from VERSION
#endif
#ifndef FW_VERSION_SEM
#define FW_VERSION_SEM "0.0.0-dev"
#endif

void  shellSetup(Experiment& e);
void  shellLoop();

// FPS overlay. Strip-rendered scenes call shellOverlay() on every strip they render (it is
// clipped to the canvas, so it costs nothing for strips it doesn't touch). Scenes that draw
// straight to the panel call shellOverlayDirect() once per frame instead.
void  shellOverlay(Canvas& c);
void  shellOverlayDirect(bool force = false);   // force: scene overwrites it every frame
bool  shellOverlayHits(int y0, int h);   // does a strip intersect the overlay areas?

float shellFps();
const char* shellFpsText();               // "42.3 fps"

// Touch: true once per press, with the tap in screen pixels.
bool  readTap(int& tx, int& ty);
