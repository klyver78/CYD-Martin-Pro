// All drawing and interaction. Hardware-free: main.cpp feeds a debounced touch position and
// pushes the strips that uiRender() fills, so tools/sim can render the same screens on a PC.
#pragma once
#include "canvas.h"
#include "portfolio.h"

enum UiView { UV_LOADING, UV_LIST, UV_DETAIL, UV_INFO };

// Things the UI asks main.cpp to do (one at a time, picked up with uiTakeAction()).
enum UiAction { UA_NONE, UA_REFRESH, UA_UPDATE };

void     uiInit();
void     uiTouch(bool down, int x, int y, uint32_t nowMs);  // debounced touch, every frame
void     uiUpdate(float dt);                                // scroll momentum
void     uiRender(Canvas& c);                               // fill one strip (every pixel)

UiView   uiView();
void     uiSetView(UiView v);
int      uiSelected();
UiAction uiTakeAction();

void     uiSetStatus(const char* s);      // status / error line
void     uiSetProgress(int pct);          // 0..100 on the loading screen, -1 = spinner only
void     uiSetAge(int seconds);           // age of the price data, -1 = unknown
void     uiSetBusy(bool busy);            // a fetch is running

extern const char* gVersion;
