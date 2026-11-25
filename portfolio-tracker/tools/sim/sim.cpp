// PC harness: runs the real portfolio maths (src/portfolio.cpp) and the real renderer
// (src/ui.cpp) against recorded API responses in fixtures/, prints the computed numbers so
// they can be checked against an independent calculation, and writes screenshots
// (out/*.ppm -> docs/*.png). No hardware and no network needed.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Arduino.h"

uint32_t g_millis = 0;
HardwareSerial Serial;
EspClass ESP;

#include "../../src/portfolio.cpp"
#include "../../src/ui.cpp"

// ------------------------------------------------------------------ helpers
static char* slurp(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char* b = (char*)malloc((size_t)n + 1);
  size_t got = fread(b, 1, (size_t)n, f);
  b[got] = 0;
  fclose(f);
  return b;
}

static uint16_t fb[SCR_W * SCR_H], strip[SCR_W * STRIP_H];
static void frame() {
  for (int y = 0; y < SCR_H; y += STRIP_H) {
    Canvas c;
    c.attach(strip, 0, y, SCR_W, STRIP_H);
    uiRender(c);
    for (int i = 0; i < SCR_W * STRIP_H; i++) fb[y * SCR_W + i] = swap16(strip[i]);
  }
}
static void save(const char* name) {
  frame();
  char p[64];
  snprintf(p, sizeof p, "out/%s.ppm", name);
  FILE* f = fopen(p, "wb");
  fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H);
  for (uint16_t c : fb) {
    uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}
static void tick(int ms = 16) {
  g_millis += (uint32_t)ms;
  uiUpdate(ms / 1000.0f);
}
static void press(int x, int y) { uiTouch(true, x, y, g_millis); tick(); }
static void release() { uiTouch(false, 0, 0, g_millis); tick(); }
static void tapAt(int x, int y) { press(x, y); release(); }
static void drag(int x, int y0, int y1, int steps) {
  press(x, y0);
  for (int i = 1; i <= steps; ++i) {
    uiTouch(true, x, y0 + (y1 - y0) * i / steps, g_millis);
    tick();
  }
  release();
}

// ------------------------------------------------------------------ the recorded fetch
// Mirrors what net.cpp does on a cold boot, but reads the fixtures instead of the network.
static void loadFixtures() {
  char* portfolio = slurp("../../portfolio.json");
  if (!pfParsePortfolio(portfolio)) { fprintf(stderr, "portfolio.json rejected\n"); exit(1); }
  printf("portfolio: %d holdings, base %s\n", pf.nh, pf.base);

  char* search = slurp("fixtures/search-isin.json");
  for (int i; (i = pfFirstUnresolved()) >= 0;) {
    char sym[20] = {0};
    if (!pfParseSearch(search, sym, sizeof sym)) { fprintf(stderr, "search fixture rejected\n"); exit(1); }
    printf("  ISIN %s -> %s\n", pf.h[i].key, sym);
    snprintf(pf.h[i].symbol, sizeof pf.h[i].symbol, "%s", sym);
  }

  char* chart = slurp("fixtures/chart-novo.json");
  for (int i; (i = pfFirstUnknownCurrency()) >= 0;) {
    char cur[8] = {0}, name[24] = {0};
    if (!pfParseChartMeta(chart, cur, sizeof cur, name, sizeof name)) {
      fprintf(stderr, "chart fixture rejected\n");
      exit(1);
    }
    printf("  %s -> currency %s, name \"%s\"\n", pf.h[i].symbol, cur, name);
    snprintf(pf.h[i].cur, sizeof pf.h[i].cur, "%s", cur);
    if (!pf.h[i].name[0]) snprintf(pf.h[i].name, sizeof pf.h[i].name, "%s", name);
  }

  pfPrepareFx();
  char symbols[320];
  int n = pfSymbolList(symbols, sizeof symbols);
  printf("spark call: %d symbols: %s\n", n, symbols);

  char* spark = slurp("fixtures/spark.json");
  if (!pfParseSpark(spark)) { fprintf(stderr, "spark fixture rejected\n"); exit(1); }
  pfCompute();

  free(portfolio);
  free(search);
  free(chart);
  free(spark);
}

static void report() {
  printf("\n%-16s %-12s %4s %10s %12s %8s\n", "NAME", "SYMBOL", "CUR", "PRICE", "VALUE", "DAY%");
  for (int i = 0; i < pf.nh; ++i) {
    const Holding& h = pf.h[i];
    printf("%-16s %-12s %4s %10.2f %12.2f %+8.2f%s\n", h.name, h.symbol, h.cur,
           (double)h.price, (double)h.value, (double)h.dayPct, h.priced ? "" : "  (NO PRICE)");
  }
  printf("\ngrid: %d days, total %.2f %s, today %+.2f (%+.2f%%)\n",
         pf.ngrid, (double)pf.value, pf.base, (double)pf.dayDelta, (double)pf.dayPct);
  int from, to;
  if (pfWindow(14, from, to))
    printf("2W window: %d points, %.2f -> %.2f (%+.2f%%)\n", to - from + 1,
           (double)pf.total[from], (double)pf.total[to],
           (double)((pf.total[to] / pf.total[from] - 1) * 100));
  printf("series:");
  for (int i = 0; i < pf.ngrid; ++i) printf(" %ld=%.0f", pf.grid[i], (double)pf.total[i]);
  printf("\n");
}

int main() {
  loadFixtures();
  report();

  gVersion = "1.0.0";
  uiInit();
  uiSetAge(-1);
  uiSetProgress(40);
  uiSetStatus("Fetching 10 quotes...");
  save("loading");

  uiSetAge(18);
  uiSetStatus("8 of 8 holdings priced - Wi-Fi 192.168.1.42");
  uiSetView(UV_LIST);
  save("overview");

  drag(160, 225, 120, 8);
  for (int i = 0; i < 40; ++i) tick();
  save("list");

  uiSetView(UV_LIST);                       // resets the scroll
  tapAt(160, UI_LIST_Y + 12);                  // first row
  if (uiView() != UV_DETAIL) { fprintf(stderr, "tap did not open the detail view\n"); return 1; }
  printf("detail view: %s\n", pf.h[uiSelected()].name);
  save("detail");

  tapAt(SCR_W - 20, 10);                    // the "1M" button
  save("detail-1m");

  uiSetView(UV_INFO);
  save("status");

  tapAt(20, 10);                            // BACK
  if (uiView() != UV_LIST) { fprintf(stderr, "back did not return to the list\n"); return 1; }
  printf("screenshots written to out/\n");
  return 0;
}
