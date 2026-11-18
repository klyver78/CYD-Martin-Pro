// LVGL 8.3 configuration for the "lvgl" ROM. Anything not set here uses LVGL's defaults
// (lv_conf_internal.h).
#if 1
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH     16
#define LV_COLOR_16_SWAP   1          // render in wire byte order, so DMA sends buffers as-is

#define LV_MEM_CUSTOM      0
#define LV_MEM_SIZE        (48U * 1024U)

// Redraw as often as possible - the default (30 ms) would cap everything at ~33 fps.
#define LV_DISP_DEF_REFR_PERIOD 1
#define LV_INDEV_DEF_READ_PERIOD 30

#define LV_TICK_CUSTOM     1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_DPI_DEF         130
#define LV_USE_LOG         0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_METER   1
#define LV_USE_SPINNER 1
#define LV_USE_LIST    1
#define LV_USE_ARC     1
#define LV_USE_BAR     1
#define LV_USE_SWITCH  1

#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

#endif
#endif
