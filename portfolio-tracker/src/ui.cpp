#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const char* gVersion = "0.0.0";

// ------------------------------------------------------------------ palette
static const uint16_t C_BG    = rgbBE(12, 14, 22);
static const uint16_t C_CARD  = rgbBE(23, 28, 46);
static const uint16_t C_ROW   = rgbBE(18, 22, 36);
static const uint16_t C_LINE  = rgbBE(42, 48, 72);
static const uint16_t C_FG    = rgbBE(233, 236, 242);
static const uint16_t C_MUTE  = rgbBE(138, 146, 166);
static const uint16_t C_ACC   = rgbBE(255, 203, 45);
static const uint16_t C_UP    = rgbBE(54, 200, 118);
static const uint16_t C_UPD   = rgbBE(16, 52, 36);        // area fill under a rising chart
static const uint16_t C_DOWN  = rgbBE(238, 88, 88);
static const uint16_t C_DOWND = rgbBE(58, 22, 26);

// ------------------------------------------------------------------ layout
static const int UI_HEAD_H  = 42;
static const int UI_CHART_Y = UI_HEAD_H, UI_CHART_H = 84;
static const int UI_LIST_Y  = UI_CHART_Y + UI_CHART_H + 2;          // 128
static const int UI_LIST_H  = SCR_H - UI_LIST_Y;                 // 112
static const int UI_ROW_H   = 34;
static const int UI_BAR_H   = 26;                             // top bar of the sub-views

// ------------------------------------------------------------------ state
static UiView   view = UV_LOADING;
static UiAction pending = UA_NONE;
static char     status[72] = "Starting...";
static int      progress = -1;
static int      ageSec = -1;
static bool     busy = false;
static int      sel = 0;
static int      detDays = 14;                              // 14 = 2W, 31 = 1M

static float    scroll = 0, vel = 0;
static bool     down = false, dragging = false;
static int      downX = 0, downY = 0, lastY = 0;
static uint32_t lastMs = 0;
static uint32_t anim = 0;

void uiInit() {
  view = UV_LOADING;
  pending = UA_NONE;
  scroll = vel = 0;
  sel = 0;
}
UiView uiView() { return view; }
int uiSelected() { return sel; }
void uiSetView(UiView v) { view = v; if (v == UV_LIST) { scroll = 0; vel = 0; } }
UiAction uiTakeAction() { UiAction a = pending; pending = UA_NONE; return a; }
void uiSetStatus(const char* s) { snprintf(status, sizeof status, "%s", s ? s : ""); }
void uiSetProgress(int pct) { progress = pct; }
void uiSetAge(int seconds) { ageSec = seconds; }
void uiSetBusy(bool b) { busy = b; }

// ------------------------------------------------------------------ formatting
// Thousands are grouped with a space: locale-neutral and unambiguous on a 5x7 font.
static void fmtNum(char* out, size_t cap, float v, int dec) {
  if (!isfinite(v)) { snprintf(out, cap, "-"); return; }
  bool neg = v < 0;
  if (neg) v = -v;
  char raw[32];
  snprintf(raw, sizeof raw, "%.*f", dec, (double)v);
  char* frac = strchr(raw, '.');
  int ilen = frac ? (int)(frac - raw) : (int)strlen(raw);
  char grouped[40];
  int g = 0;
  if (neg && g + 1 < (int)sizeof grouped) grouped[g++] = '-';
  for (int i = 0; i < ilen && g + 1 < (int)sizeof grouped; ++i) {
    grouped[g++] = raw[i];
    int left = ilen - 1 - i;
    if (left > 0 && left % 3 == 0 && g + 1 < (int)sizeof grouped) grouped[g++] = ' ';
  }
  if (frac)
    for (const char* p = frac; *p && g + 1 < (int)sizeof grouped; ++p) grouped[g++] = *p;
  grouped[g] = 0;
  snprintf(out, cap, "%s", grouped);
}
// Money: no decimals once the number is big enough that they are noise.
static void fmtMoney(char* out, size_t cap, float v) {
  fmtNum(out, cap, v, fabsf(v) >= 1000 ? 0 : 2);
}
static void fmtSigned(char* out, size_t cap, float v) {
  char n[24];
  fmtMoney(n, sizeof n, fabsf(v));
  snprintf(out, cap, "%c%s", v < 0 ? '-' : '+', n);
}
static void fmtPct(char* out, size_t cap, float p) {
  if (!isfinite(p)) { snprintf(out, cap, "-"); return; }
  snprintf(out, cap, "%+.2f%%", (double)p);
}
static uint16_t signCol(float v) { return v < -0.0001f ? C_DOWN : (v > 0.0001f ? C_UP : C_MUTE); }

// ------------------------------------------------------------------ small widgets
static int textR(Canvas& c, int xRight, int y, const char* s, uint16_t col, int size) {
  int x = xRight - Canvas::textW(s, size);
  c.text(x, y, s, col, size);
  return x;
}
static void textClip(Canvas& c, int x, int y, const char* s, uint16_t col, int size, int maxW) {
  char t[32];
  size_t fit = (size_t)(maxW / (6 * size));
  if (fit == 0) return;
  if (fit > sizeof t - 1) fit = sizeof t - 1;
  strncpy(t, s, fit);
  t[fit] = 0;
  c.text(x, y, t, col, size);
}
static bool hit(int x, int y, int bx, int by, int bw, int bh) {
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}
static void button(Canvas& c, int x, int y, int w, int h, const char* label, bool on) {
  c.fillRect(x, y, w, h, on ? C_ACC : C_CARD);
  c.rect(x, y, w, h, on ? C_ACC : C_LINE);
  int size = h >= 24 ? 2 : 1;
  c.text(x + (w - Canvas::textW(label, size)) / 2, y + (h - 8 * size) / 2 + 1, label,
         on ? rgbBE(26, 21, 0) : C_FG, size);
}

// ------------------------------------------------------------------ chart
// Area chart of v[from..to], drawn column by column so it works inside a strip canvas.
static void chart(Canvas& c, int x, int y, int w, int h, const float* v, int from, int to,
                  bool showScale) {
  c.fillRect(x, y, w, h, C_BG);
  int n = to - from + 1;
  if (n < 2) {
    c.text(x + (w - Canvas::textW("no data yet", 1)) / 2, y + h / 2 - 4, "no data yet", C_MUTE, 1);
    return;
  }
  float lo = v[from], hi = v[from];
  for (int i = from; i <= to; ++i) {
    if (v[i] < lo) lo = v[i];
    if (v[i] > hi) hi = v[i];
  }
  float pad = (hi - lo) * 0.08f;
  if (pad <= 0) pad = (fabsf(hi) > 0 ? fabsf(hi) : 1) * 0.01f;
  lo -= pad;
  hi += pad;

  bool rising = v[to] >= v[from];
  uint16_t line = rising ? C_UP : C_DOWN;
  uint16_t fill = rising ? C_UPD : C_DOWND;

  int plotW = showScale ? w - 52 : w;                        // room for the min/max labels
  int baseY = y + h - 1 - (int)((v[from] - lo) / (hi - lo) * (h - 1));
  for (int px = 0; px < plotW; ++px) {
    float t = plotW > 1 ? (float)px / (plotW - 1) : 0;
    float fi = from + t * (to - from);
    int i0 = (int)fi;
    if (i0 > to - 1) i0 = to - 1;
    float fr = fi - i0;
    float val = v[i0] + (v[i0 + 1] - v[i0]) * fr;
    int yy = y + h - 1 - (int)((val - lo) / (hi - lo) * (h - 1));
    c.fillRect(x + px, yy + 1, 1, y + h - yy - 1, fill);
    c.fillRect(x + px, yy - 1, 1, 2, line);
    if ((px & 3) == 0 && abs(yy - baseY) > 2) c.fillRect(x + px, baseY, 1, 1, C_LINE);
  }
  if (showScale) {
    char t[24];
    fmtMoney(t, sizeof t, hi - pad);
    c.text(x + plotW + 4, y, t, C_MUTE, 1);
    fmtMoney(t, sizeof t, lo + pad);
    c.text(x + plotW + 4, y + h - 8, t, C_MUTE, 1);
  }
}

// ------------------------------------------------------------------ overview
static float listMaxScroll() {
  float m = (float)pf.nh * UI_ROW_H - UI_LIST_H;
  return m > 0 ? m : 0;
}

static void drawHeader(Canvas& c) {
  c.fillRect(0, 0, SCR_W, UI_HEAD_H, C_CARD);
  c.fillRect(0, UI_HEAD_H - 1, SCR_W, 1, C_LINE);

  char label[24];
  snprintf(label, sizeof label, "TOTAL %s", pf.base);
  c.text(8, 4, label, C_MUTE, 1);

  char t[32];
  fmtMoney(t, sizeof t, pf.value);
  c.text(8, 14, t, C_FG, 3);

  int right = SCR_W - 30;
  fmtPct(t, sizeof t, pf.dayPct);
  textR(c, right, 8, t, ageSec > 3600 ? C_MUTE : signCol(pf.dayDelta), 2);
  fmtSigned(t, sizeof t, pf.dayDelta);
  textR(c, right, 26, t, C_MUTE, 1);

  // info button; it blinks while a fetch is running
  bool blink = busy && ((anim / 20) & 1);
  c.rect(SCR_W - 24, 5, 19, 19, blink ? C_ACC : C_LINE);
  c.text(SCR_W - 17, 10, "i", blink ? C_ACC : C_MUTE, 1);
}

static void drawRow(Canvas& c, int i, int y) {
  const Holding& h = pf.h[i];
  c.fillRect(0, y, SCR_W, UI_ROW_H, (i & 1) ? C_ROW : C_BG);
  c.fillRect(0, y + UI_ROW_H - 1, SCR_W, 1, C_LINE);
  c.fillRect(0, y + 2, 3, UI_ROW_H - 4, h.priced ? signCol(h.dayPct) : C_LINE);

  textClip(c, 10, y + 4, h.name[0] ? h.name : h.symbol, C_FG, 2, 150);
  char sub[40];
  char q[24];
  fmtNum(q, sizeof q, h.qty, h.qty == (int)h.qty ? 0 : 2);
  snprintf(sub, sizeof sub, "%s  x%s", h.symbol[0] ? h.symbol : h.key, q);
  textClip(c, 10, y + 21, sub, C_MUTE, 1, 180);

  if (!h.priced) {
    textR(c, SCR_W - 8, y + 11, "no price", C_MUTE, 1);
    return;
  }
  char t[32];
  fmtMoney(t, sizeof t, h.value);
  textR(c, SCR_W - 8, y + 4, t, C_FG, 2);
  fmtPct(t, sizeof t, h.dayPct);
  textR(c, SCR_W - 8, y + 21, t, signCol(h.dayPct), 1);
}

static void drawList(Canvas& c) {
  // Rows first: a row scrolled past the top of the list is then painted over by the chart,
  // the divider and the header, which together cover every pixel above UI_LIST_Y.
  int off = (int)scroll;
  int first = off / UI_ROW_H;
  if (first < 0) first = 0;
  for (int i = first; i < pf.nh; ++i) {
    int y = UI_LIST_Y + i * UI_ROW_H - off;
    if (y >= c.y0 + c.h) break;
    if (y + UI_ROW_H <= c.y0) continue;
    drawRow(c, i, y);
  }

  int from, to;
  bool twoWeeks = pfWindow(14, from, to);
  if (!twoWeeks && pf.ngrid >= 2) { from = 0; to = pf.ngrid - 1; }
  if (twoWeeks || pf.ngrid >= 2) {
    chart(c, 0, UI_CHART_Y, SCR_W, UI_CHART_H, pf.total, from, to, true);
    c.text(6, UI_CHART_Y + 4, twoWeeks ? "2 WEEKS" : "ALL DATA", C_MUTE, 1, C_BG);
    char t[24];
    float a = pf.total[from];
    if (a > 0) {
      fmtPct(t, sizeof t, (pf.total[to] / a - 1) * 100.0f);
      c.text(6, UI_CHART_Y + UI_CHART_H - 12, t, signCol(pf.total[to] - a), 1, C_BG);
    }
  } else {
    chart(c, 0, UI_CHART_Y, SCR_W, UI_CHART_H, pf.total, 0, 0, false);
  }
  if (!pf.nh)
    c.text((SCR_W - Canvas::textW("No holdings in portfolio.json", 1)) / 2, UI_LIST_Y + 30,
           "No holdings in portfolio.json", C_MUTE, 1);
  c.fillRect(0, UI_LIST_Y - 2, SCR_W, 2, C_LINE);
  drawHeader(c);

  if (pf.nh * UI_ROW_H > UI_LIST_H) {                              // scrollbar
    int th = UI_LIST_H * UI_LIST_H / (pf.nh * UI_ROW_H);
    int ty = UI_LIST_Y + (int)(scroll / listMaxScroll() * (UI_LIST_H - th));
    c.fillRect(SCR_W - 3, UI_LIST_Y, 3, UI_LIST_H, C_ROW);
    c.fillRect(SCR_W - 3, ty, 3, th, C_LINE);
  }
}

// ------------------------------------------------------------------ one holding
static void drawDetail(Canvas& c) {
  const Holding& h = pf.h[sel];
  c.clear(C_BG);
  c.fillRect(0, 0, SCR_W, UI_BAR_H, C_CARD);
  c.fillRect(0, UI_BAR_H - 1, SCR_W, 1, C_LINE);
  button(c, 2, 2, 52, UI_BAR_H - 5, "BACK", false);
  c.text(62, 6, h.symbol[0] ? h.symbol : h.key, C_ACC, 2);
  button(c, SCR_W - 76, 2, 36, UI_BAR_H - 5, "2W", detDays == 14);
  button(c, SCR_W - 38, 2, 36, UI_BAR_H - 5, "1M", detDays != 14);

  textClip(c, 8, UI_BAR_H + 4, h.name[0] ? h.name : h.symbol, C_FG, 2, SCR_W - 16);

  int cy = UI_BAR_H + 24, ch = 94;
  if (h.priced && h.n >= 2) {
    long cutoff = h.day[h.n - 1] - (detDays - 1);
    int from = h.n - 1;
    while (from > 0 && h.day[from - 1] >= cutoff) --from;
    if (from > h.n - 2) from = h.n - 2;
    chart(c, 0, cy, SCR_W, ch, h.close, from, h.n - 1, true);
    char t[24];
    snprintf(t, sizeof t, "%s %s", h.cur[0] ? h.cur : "?", detDays == 14 ? "2W" : "1M");
    c.text(6, cy + 4, t, C_MUTE, 1, C_BG);
    float a = h.close[from];
    if (a > 0) {
      fmtPct(t, sizeof t, (h.price / a - 1) * 100.0f);
      c.text(6, cy + ch - 12, t, signCol(h.price - a), 1, C_BG);
    }
  } else {
    chart(c, 0, cy, SCR_W, ch, h.close, 0, 0, false);
  }

  // three rows of two stats
  struct Stat { const char* label; char val[24]; uint16_t col; };
  Stat st[6] = {};
  int k = 0;
  st[k].label = "QTY";
  fmtNum(st[k].val, sizeof st[k].val, h.qty, h.qty == (int)h.qty ? 0 : 2);
  st[k].col = C_FG;
  ++k;
  char priceLabel[16], valueLabel[16];
  snprintf(priceLabel, sizeof priceLabel, "PRICE %s", h.cur[0] ? h.cur : "?");
  snprintf(valueLabel, sizeof valueLabel, "VALUE %s", pf.base);
  st[k].label = priceLabel;
  fmtNum(st[k].val, sizeof st[k].val, h.price, h.price >= 1000 ? 1 : 2);
  st[k].col = C_FG;
  ++k;
  st[k].label = valueLabel;
  fmtMoney(st[k].val, sizeof st[k].val, h.value);
  st[k].col = C_FG;
  ++k;
  st[k].label = "TODAY";
  fmtPct(st[k].val, sizeof st[k].val, h.dayPct);
  st[k].col = signCol(h.dayPct);
  ++k;
  st[k].label = "TODAY DELTA";
  fmtSigned(st[k].val, sizeof st[k].val, h.dayDelta);
  st[k].col = signCol(h.dayDelta);
  ++k;
  st[k].label = "WEIGHT";
  snprintf(st[k].val, sizeof st[k].val, "%.1f%%", pf.value > 0 ? (double)(h.value / pf.value * 100) : 0.0);
  st[k].col = C_FG;
  ++k;

  int y0 = cy + ch + 4;
  for (int i = 0; i < k; ++i) {
    int x = (i & 1) ? 168 : 8;
    int y = y0 + (i / 2) * 24;
    c.text(x, y, st[i].label, C_MUTE, 1);
    c.text(x, y + 9, st[i].val, st[i].col, 2);
  }
}

// ------------------------------------------------------------------ status / settings
static void drawInfo(Canvas& c) {
  c.clear(C_BG);
  c.fillRect(0, 0, SCR_W, UI_BAR_H, C_CARD);
  c.fillRect(0, UI_BAR_H - 1, SCR_W, 1, C_LINE);
  button(c, 2, 2, 52, UI_BAR_H - 5, "BACK", false);
  c.text(62, 6, "STATUS", C_ACC, 2);

  char t[80];
  int y = UI_BAR_H + 8;
  snprintf(t, sizeof t, "Firmware   v%s", gVersion);
  c.text(8, y, t, C_FG, 1); y += 14;
  snprintf(t, sizeof t, "Holdings   %d of %d priced", pf.nPriced, pf.nh);
  c.text(8, y, t, C_FG, 1); y += 14;
  snprintf(t, sizeof t, "Base       %s", pf.base);
  c.text(8, y, t, C_FG, 1); y += 14;
  if (ageSec < 0) snprintf(t, sizeof t, "Prices     never fetched");
  else if (ageSec < 90) snprintf(t, sizeof t, "Prices     %d s old", ageSec);
  else snprintf(t, sizeof t, "Prices     %d min old", ageSec / 60);
  c.text(8, y, t, ageSec > 3600 ? C_DOWN : C_FG, 1); y += 18;

  for (int i = 0; i < pf.nfx; ++i) {
    snprintf(t, sizeof t, "%-10s %s %.4f", i ? "" : "FX", pf.fx[i].pair,
             (double)(pf.fx[i].n ? pf.fx[i].rate[pf.fx[i].n - 1] * pf.fx[i].scale : 0.0f));
    c.text(8, y, t, pf.fx[i].n ? C_FG : C_DOWN, 1);
    y += 12;
  }
  y += 6;

  c.fillRect(8, y, SCR_W - 16, 1, C_LINE); y += 8;
  c.text(8, y, status, C_MUTE, 1); y += 12;
  if (strlen(status) > 50) { c.text(8, y, status + 50, C_MUTE, 1); y += 12; }

  button(c, 8, SCR_H - 36, 140, 28, "REFRESH", false);
  button(c, SCR_W - 148, SCR_H - 36, 140, 28, "UPDATE FW", false);
}

// ------------------------------------------------------------------ loading
static void drawLoading(Canvas& c) {
  c.clear(C_BG);
  c.text((SCR_W - Canvas::textW("PORTFOLIO", 3)) / 2, 54, "PORTFOLIO", C_ACC, 3);
  c.text((SCR_W - Canvas::textW("TRACKER", 2)) / 2, 84, "TRACKER", C_MUTE, 2);

  int bx = 40, bw = SCR_W - 80, by = 130;
  c.rect(bx, by, bw, 12, C_LINE);
  if (progress >= 0) {
    c.fillRect(bx + 2, by + 2, (bw - 4) * (progress < 100 ? progress : 100) / 100, 8, C_ACC);
  } else {
    int w = 40, x = bx + 2 + (int)((bw - 4 - w) * (0.5 + 0.5 * sin(anim * 0.08)));
    c.fillRect(x, by + 2, w, 8, C_ACC);
  }
  char line[52];
  snprintf(line, sizeof line, "%.50s", status);
  c.text((SCR_W - Canvas::textW(line, 1)) / 2, by + 22, line, C_FG, 1);
  if (strlen(status) > 50)
    c.text((SCR_W - Canvas::textW(status + 50, 1)) / 2, by + 34, status + 50, C_FG, 1);
  c.text((SCR_W - Canvas::textW("tap for status", 1)) / 2, SCR_H - 16, "tap for status", C_MUTE, 1);
}

// ------------------------------------------------------------------ frame
void uiRender(Canvas& c) {
  if (sel >= pf.nh) sel = 0;                   // portfolio.json may have shrunk since the tap
  if (view == UV_DETAIL && !pf.nh) view = UV_LIST;
  switch (view) {
    case UV_LIST:   c.clear(C_BG); drawList(c); break;
    case UV_DETAIL: drawDetail(c); break;
    case UV_INFO:   drawInfo(c); break;
    default:        drawLoading(c); break;
  }
}

void uiUpdate(float dt) {
  anim++;
  if (view != UV_LIST || down) return;
  if (fabsf(vel) > 8) {
    scroll += vel * dt;
    vel *= expf(-4.0f * dt);
  } else {
    vel = 0;
  }
  float m = listMaxScroll();
  if (scroll < 0) { scroll = 0; vel = 0; }
  if (scroll > m) { scroll = m; vel = 0; }
}

// ------------------------------------------------------------------ touch
static void tap(int x, int y) {
  switch (view) {
    case UV_LOADING:
      view = UV_INFO;
      break;
    case UV_LIST:
      if (y < UI_HEAD_H) {
        if (x >= SCR_W - 30) view = UV_INFO;
      } else if (y >= UI_LIST_Y) {
        int i = (y - UI_LIST_Y + (int)scroll) / UI_ROW_H;
        if (i >= 0 && i < pf.nh) { sel = i; detDays = 14; view = UV_DETAIL; }
      }
      break;
    case UV_DETAIL:
      if (y < UI_BAR_H) {
        if (hit(x, y, 2, 2, 52, UI_BAR_H - 5)) view = UV_LIST;
        else if (hit(x, y, SCR_W - 76, 2, 36, UI_BAR_H - 5)) detDays = 14;
        else if (hit(x, y, SCR_W - 38, 2, 36, UI_BAR_H - 5)) detDays = 31;
      }
      break;
    case UV_INFO:
      if (hit(x, y, 2, 2, 52, UI_BAR_H - 5)) view = pf.ngrid ? UV_LIST : UV_LOADING;
      else if (hit(x, y, 8, SCR_H - 36, 140, 28)) pending = UA_REFRESH;
      else if (hit(x, y, SCR_W - 148, SCR_H - 36, 140, 28)) pending = UA_UPDATE;
      break;
  }
}

void uiTouch(bool d, int x, int y, uint32_t nowMs) {
  float dt = lastMs ? (nowMs - lastMs) / 1000.0f : 0.016f;
  if (dt <= 0) dt = 0.016f;
  lastMs = nowMs;

  if (d && !down) {
    down = true;
    dragging = false;
    downX = x;
    downY = y;
    lastY = y;
    vel = 0;
  } else if (d && down) {
    int move = y - lastY;
    if (!dragging && abs(y - downY) > 6) dragging = true;
    if (dragging && view == UV_LIST) {
      scroll -= move;
      float v = -move / dt;
      vel = vel * 0.6f + v * 0.4f;
      float m = listMaxScroll();
      if (scroll < 0) scroll = 0;
      if (scroll > m) scroll = m;
    }
    lastY = y;
  } else if (!d && down) {
    down = false;
    if (!dragging) tap(downX, downY);
  }
}
