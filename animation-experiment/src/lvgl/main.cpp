// The LVGL ROM: realistic "UI framework" frame rates. LVGL 8.3 renders into two 320x40
// buffers that are sent to the panel with TFT_eSPI DMA (double buffered). FPS here = how many
// screen refreshes LVGL completes per second while the scene animates.
#ifdef GFX_LVGL
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <esp_random.h>
#include "../common/shell.h"
#include "../common/board.h"

TFT_eSPI& tftRef();

// ---------------------------------------------------------------- display driver
static lv_disp_draw_buf_t drawBuf;
static lv_disp_drv_t      dispDrv;
static lv_color_t*        lvBuf[2];
static volatile uint32_t  lvFrames = 0;

static void flushCb(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* px) {
  int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  // pushImageDMA waits for the previous transfer, so with two buffers LVGL never renders into
  // the one that is still being sent.
  tftRef().pushImageDMA(a->x1, a->y1, w, h, (uint16_t*)px);
  lv_disp_flush_ready(d);
}
static void monitorCb(lv_disp_drv_t*, uint32_t, uint32_t) { lvFrames++; }   // one per refresh

static void lvglInit() {
  lv_init();
  for (int i = 0; i < 2; i++)
    lvBuf[i] = (lv_color_t*)heap_caps_malloc(SCR_W * STRIP_H * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  lv_disp_draw_buf_init(&drawBuf, lvBuf[0], lvBuf[1], SCR_W * STRIP_H);
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res    = SCR_W;
  dispDrv.ver_res    = SCR_H;
  dispDrv.flush_cb   = flushCb;
  dispDrv.monitor_cb = monitorCb;
  dispDrv.draw_buf   = &drawBuf;
  lv_disp_drv_register(&dispDrv);
}

// ---------------------------------------------------------------- helpers
static lv_obj_t* scr() { return lv_scr_act(); }
static lv_obj_t *fpsLbl = nullptr, *paramLbl = nullptr;
static char      fpsShown[24] = "";

static void baseScreen() {
  lv_obj_clean(scr());
  lv_obj_set_style_bg_color(scr(), lv_color_hex(0x0e1220), 0);
  lv_obj_set_style_bg_opa(scr(), LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr(), LV_OBJ_FLAG_SCROLLABLE);
  fpsShown[0] = 0;
}
static void topOverlay(bool withParam) {
  fpsLbl = lv_label_create(lv_layer_top());
  lv_obj_set_style_bg_color(fpsLbl, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(fpsLbl, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(fpsLbl, lv_color_hex(0xffcb2d), 0);
  lv_obj_set_style_text_font(fpsLbl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_pad_hor(fpsLbl, 4, 0);
  lv_label_set_text(fpsLbl, "-- fps");
  lv_obj_set_pos(fpsLbl, 0, 0);
  paramLbl = nullptr;
  if (withParam) {
    paramLbl = lv_label_create(lv_layer_top());
    lv_obj_set_size(paramLbl, SCR_W, 24);
    lv_obj_set_style_bg_color(paramLbl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(paramLbl, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(paramLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(paramLbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(paramLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(paramLbl, LV_ALIGN_BOTTOM_MID, 0, 0);
  }
}
static void anim(lv_obj_t* o, lv_anim_exec_xcb_t cb, int32_t a, int32_t b, uint32_t ms) {
  lv_anim_t an;
  lv_anim_init(&an);
  lv_anim_set_var(&an, o);
  lv_anim_set_exec_cb(&an, cb);
  lv_anim_set_values(&an, a, b);
  lv_anim_set_time(&an, ms);
  lv_anim_set_playback_time(&an, ms);
  lv_anim_set_repeat_count(&an, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&an, lv_anim_path_ease_in_out);
  lv_anim_start(&an);
}

// ---------------------------------------------------------------- scenes
enum { S_WIDGETS, S_GAUGE, S_LIST, S_FULL, S_OBJS, S_TEXT, S_COUNT };
static const char* NAMES[S_COUNT] = {"Widgets", "Meter", "Scroll list", "Full redraw", "Moving objs", "Big text"};
static const uint8_t GRP[S_COUNT] = {0, 0, 0, 1, 1, 1};
static const char* GROUPS[] = {"LVGL widgets", "LVGL stress"};

static int cur = -1;
static lv_obj_t* fullObj = nullptr;
static lv_obj_t* bigLbl = nullptr;
static uint16_t  hue = 0;
static uint32_t  counter = 0;

static const int OBJ_STEPS[] = {5, 10, 25, 50, 100};
static int       objIdx = 2, nObjs = 0;
struct Mover { lv_obj_t* o; int16_t x, y, vx, vy; };
static Mover     movers[100];

static void arcCb(void* o, int32_t v)  { lv_arc_set_value((lv_obj_t*)o, v); }
static void barCb(void* o, int32_t v)  { lv_bar_set_value((lv_obj_t*)o, v, LV_ANIM_OFF); }
static void lblCb(void* o, int32_t v)  { lv_label_set_text_fmt((lv_obj_t*)o, "%d %%", (int)v); }
static lv_meter_indicator_t* needle = nullptr;
static lv_obj_t* meterObj = nullptr;
static void needleCb(void* o, int32_t v) { lv_meter_set_indicator_value((lv_obj_t*)o, needle, v); }
static void scrollCb(void* o, int32_t v) { lv_obj_scroll_to_y((lv_obj_t*)o, v, LV_ANIM_OFF); }

static void buildWidgets() {
  lv_obj_t* arc = lv_arc_create(scr());
  lv_obj_set_size(arc, 130, 130);
  lv_obj_align(arc, LV_ALIGN_LEFT_MID, 14, 8);
  lv_arc_set_range(arc, 0, 100);
  anim(arc, arcCb, 0, 100, 1500);
  lv_obj_t* l = lv_label_create(arc);
  lv_obj_center(l);
  anim(l, lblCb, 0, 100, 1500);
  for (int i = 0; i < 3; i++) {
    lv_obj_t* b = lv_bar_create(scr());
    lv_obj_set_size(b, 140, 16);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -14, 50 + i * 36);
    lv_bar_set_range(b, 0, 100);
    anim(b, barCb, 0, 100, 900 + i * 400);
  }
  lv_obj_t* sw = lv_switch_create(scr());
  lv_obj_align(sw, LV_ALIGN_BOTTOM_RIGHT, -60, -30);
  lv_obj_t* sp = lv_spinner_create(scr(), 1000, 60);
  lv_obj_set_size(sp, 44, 44);
  lv_obj_align(sp, LV_ALIGN_BOTTOM_RIGHT, -8, -24);
}

static void buildMeter() {
  meterObj = lv_meter_create(scr());
  lv_obj_set_size(meterObj, 210, 210);
  lv_obj_align(meterObj, LV_ALIGN_CENTER, 0, 10);
  lv_meter_scale_t* s = lv_meter_add_scale(meterObj);
  lv_meter_set_scale_ticks(meterObj, s, 41, 2, 10, lv_palette_main(LV_PALETTE_GREY));
  lv_meter_set_scale_major_ticks(meterObj, s, 8, 4, 15, lv_color_white(), 10);
  lv_meter_indicator_t* red = lv_meter_add_arc(meterObj, s, 3, lv_palette_main(LV_PALETTE_RED), 0);
  lv_meter_set_indicator_start_value(meterObj, red, 80);
  lv_meter_set_indicator_end_value(meterObj, red, 100);
  needle = lv_meter_add_needle_line(meterObj, s, 4, lv_palette_main(LV_PALETTE_AMBER), -10);
  anim(meterObj, needleCb, 0, 100, 2000);
}

static void buildList() {
  lv_obj_t* list = lv_list_create(scr());
  lv_obj_set_size(list, SCR_W, SCR_H);
  lv_obj_set_pos(list, 0, 0);
  for (int i = 0; i < 40; i++) {
    char t[32]; snprintf(t, sizeof t, "Item %02d   value %d", i, i * 37 % 1000);
    lv_list_add_btn(list, LV_SYMBOL_FILE, t);
  }
  lv_obj_update_layout(list);
  int32_t maxY = lv_obj_get_scroll_bottom(list);
  anim(list, scrollCb, 0, maxY, 6000);
}

static void buildFull() {
  fullObj = lv_obj_create(scr());
  lv_obj_set_size(fullObj, SCR_W, SCR_H);
  lv_obj_set_pos(fullObj, 0, 0);
  lv_obj_set_style_radius(fullObj, 0, 0);
  lv_obj_set_style_border_width(fullObj, 0, 0);
  lv_obj_set_style_bg_grad_dir(fullObj, LV_GRAD_DIR_VER, 0);
  lv_obj_t* l = lv_label_create(fullObj);
  lv_label_set_text(l, "whole screen invalidated\nevery frame (worst case)");
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
  lv_obj_center(l);
}

static void spawnObjs() {
  for (int i = 0; i < nObjs; i++) lv_obj_del(movers[i].o);
  nObjs = OBJ_STEPS[objIdx];
  for (int i = 0; i < nObjs; i++) {
    Mover& m = movers[i];
    m.o = lv_obj_create(scr());
    lv_obj_set_size(m.o, 24, 24);
    lv_obj_set_style_radius(m.o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(m.o, 0, 0);
    lv_obj_set_style_bg_color(m.o, lv_color_hsv_to_rgb(esp_random() % 360, 80, 100), 0);
    lv_obj_clear_flag(m.o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    m.x = esp_random() % (SCR_W - 24); m.y = 20 + esp_random() % (SCR_H - 70);
    m.vx = (esp_random() & 1) ? 3 : -3; m.vy = (esp_random() & 1) ? 2 : -2;
  }
}
static void moveObjs() {
  for (int i = 0; i < nObjs; i++) {
    Mover& m = movers[i];
    m.x += m.vx; m.y += m.vy;
    if (m.x < 0 || m.x > SCR_W - 24) { m.vx = -m.vx; m.x += 2 * m.vx; }
    if (m.y < 20 || m.y > SCR_H - 50) { m.vy = -m.vy; m.y += 2 * m.vy; }
    lv_obj_set_pos(m.o, m.x, m.y);
  }
}

static void buildText() {
  bigLbl = lv_label_create(scr());
  lv_obj_set_style_text_font(bigLbl, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(bigLbl, lv_color_hex(0xffcb2d), 0);
  lv_obj_align(bigLbl, LV_ALIGN_CENTER, 0, -10);
  lv_label_set_text(bigLbl, "000000");
  lv_obj_t* sp = lv_spinner_create(scr(), 1000, 60);
  lv_obj_set_size(sp, 50, 50);
  lv_obj_align(sp, LV_ALIGN_BOTTOM_MID, 0, -14);
}

// ---------------------------------------------------------------- the experiment
class LvglExperiment : public Experiment {
public:
  int groupCount() override { return 2; }
  const char* groupName(int g) override { return GROUPS[g]; }
  int sceneCount() override { return S_COUNT; }
  SceneInfo scene(int i) override { return {GRP[i], NAMES[i]}; }
  bool ownOverlay() override { return true; }

  bool start(int i) override {
    cur = i;
    nObjs = 0;
    tftRef().startWrite();
    baseScreen();
    switch (i) {
      case S_WIDGETS: buildWidgets(); break;
      case S_GAUGE:   buildMeter(); break;
      case S_LIST:    buildList(); break;
      case S_FULL:    buildFull(); break;
      case S_OBJS:    spawnObjs(); break;
      case S_TEXT:    buildText(); break;
    }
    topOverlay(i == S_OBJS);
    if (paramLbl) lv_label_set_text(paramLbl, paramLabel().c_str());
    lv_obj_invalidate(scr());
    return true;
  }
  void stop() override {
    lv_anim_del_all();
    lv_obj_clean(lv_layer_top());
    lv_obj_clean(scr());
    fpsLbl = paramLbl = nullptr; fullObj = bigLbl = meterObj = nullptr; nObjs = 0;
    tftRef().dmaWait();
    tftRef().endWrite();
    cur = -1;
  }
  int frame() override {
    if (cur == S_FULL && fullObj) {
      hue = (hue + 3) % 360;
      lv_obj_set_style_bg_color(fullObj, lv_color_hsv_to_rgb(hue, 70, 60), 0);
      lv_obj_set_style_bg_grad_color(fullObj, lv_color_hsv_to_rgb((hue + 120) % 360, 70, 30), 0);
    } else if (cur == S_OBJS) {
      moveObjs();
    } else if (cur == S_TEXT && bigLbl) {
      lv_label_set_text_fmt(bigLbl, "%06u", (unsigned)(counter++ % 1000000));
    }
    if (fpsLbl && strcmp(fpsShown, shellFpsText())) {
      strlcpy(fpsShown, shellFpsText(), sizeof fpsShown);
      lv_label_set_text(fpsLbl, fpsShown);
    }
    uint32_t before = lvFrames;
    lv_timer_handler();
    return (int)(lvFrames - before);
  }
  bool hasParam() override { return cur == S_OBJS; }
  void param(int d) override {
    objIdx = constrain(objIdx + d, 0, 4);
    spawnObjs();
    if (paramLbl) lv_label_set_text(paramLbl, paramLabel().c_str());
  }
  String paramLabel() override { return String("-      ") + nObjs + " objects      +"; }
};

static LvglExperiment lvx;

// Shell display: TFT_eSPI (display_tft.cpp, compiled with GFX_TFT in this ROM too).
void setup() {
  Serial.begin(115200);
  display().begin();
  lvglInit();
  shellSetup(lvx);
}

void loop() { shellLoop(); }
#endif
