#include "portfolio.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsonlite.h"

Portfolio pf;

static void cpy(char* d, const char* s, size_t cap) {
  if (!cap) return;
  size_t i = 0;
  for (; s && s[i] && i + 1 < cap; ++i) d[i] = s[i];
  d[i] = 0;
}

// A quote currency like "GBp" means pence: the FX pair is GBP/base and the price needs /100.
static float curScale(const char* cur) {
  size_t n = strlen(cur);
  return (n == 3 && cur[2] >= 'a' && cur[2] <= 'z') ? 0.01f : 1.0f;
}
static void curUpper(const char* cur, char* out, size_t cap) {
  size_t i = 0;
  for (; cur[i] && i + 1 < cap; ++i) out[i] = (cur[i] >= 'a' && cur[i] <= 'z') ? cur[i] - 32 : cur[i];
  out[i] = 0;
}

// An ISIN is 12 characters: 2 letters, 9 alphanumerics, 1 check digit.
static bool looksLikeIsin(const char* s) {
  if (strlen(s) != 12) return false;
  if (!isalpha((unsigned char)s[0]) || !isalpha((unsigned char)s[1])) return false;
  for (int i = 2; i < 12; ++i)
    if (!isalnum((unsigned char)s[i])) return false;
  return true;
}

void pfReset() {
  memset(&pf, 0, sizeof pf);
  cpy(pf.base, "DKK", sizeof pf.base);
}

// ===================================================================== portfolio.json
//   { "base": "DKK",
//     "holdings": [ { "ticker": "NOVO-B.CO", "qty": 25 },
//                   { "isin": "US0378331005", "qty": 10, "name": "Apple", "currency": "USD" } ] }
bool pfParsePortfolio(const char* json) {
  pfReset();
  if (!json) return false;

  char base[8];
  if (jlStr(jlFind(json, nullptr, "base"), base, sizeof base) && strlen(base) >= 3)
    curUpper(base, pf.base, sizeof pf.base);

  const char* ab;
  const char* ae;
  if (!jlFindRange(json, nullptr, "holdings", ab, ae) || *ab != '[') return false;

  for (const char* el = jlArrayNext(ab, nullptr); el; el = jlArrayNext(ab, el)) {
    if (pf.nh >= MAX_HOLDINGS) break;
    const char* elEnd = jlSkipValue(el);
    if (!elEnd) break;
    Holding& h = pf.h[pf.nh];
    memset(&h, 0, sizeof h);

    if (!jlStr(jlFind(el, elEnd, "ticker"), h.key, sizeof h.key) || !h.key[0]) {
      if (!jlStr(jlFind(el, elEnd, "symbol"), h.key, sizeof h.key) || !h.key[0])
        if (!jlStr(jlFind(el, elEnd, "isin"), h.key, sizeof h.key) || !h.key[0]) continue;
    }
    h.isIsin = looksLikeIsin(h.key);
    if (!h.isIsin) cpy(h.symbol, h.key, sizeof h.symbol);

    const char* q = jlFind(el, elEnd, "qty");
    if (!q) q = jlFind(el, elEnd, "quantity");
    if (!q) q = jlFind(el, elEnd, "shares");
    if (!jlNum(q, h.qty) || !(h.qty > 0)) continue;

    jlStr(jlFind(el, elEnd, "name"), h.name, sizeof h.name);
    char cur[8] = {0};
    if (jlStr(jlFind(el, elEnd, "currency"), cur, sizeof cur) && strlen(cur) >= 3)
      cpy(h.cur, cur, sizeof h.cur);
    if (!h.name[0]) cpy(h.name, h.key, sizeof h.name);
    pf.nh++;
  }
  return pf.nh > 0;
}

// ===================================================================== /v1/finance/search
// Used only to turn an ISIN into a symbol. Prefers an equity/ETF quote over anything else.
bool pfParseSearch(const char* json, char* sym, size_t cap) {
  if (!json || !cap) return false;
  sym[0] = 0;
  const char* ab;
  const char* ae;
  if (!jlFindRange(json, nullptr, "quotes", ab, ae) || *ab != '[') return false;

  char first[20] = {0};
  for (const char* el = jlArrayNext(ab, nullptr); el; el = jlArrayNext(ab, el)) {
    const char* elEnd = jlSkipValue(el);
    if (!elEnd) break;
    char s[20] = {0};
    if (!jlStr(jlFind(el, elEnd, "symbol"), s, sizeof s) || !s[0]) continue;
    if (!first[0]) cpy(first, s, sizeof first);
    char t[16] = {0};
    jlStr(jlFind(el, elEnd, "quoteType"), t, sizeof t);
    if (!strcmp(t, "EQUITY") || !strcmp(t, "ETF") || !strcmp(t, "MUTUALFUND")) {
      cpy(sym, s, cap);
      return true;
    }
  }
  cpy(sym, first, cap);
  return sym[0] != 0;
}

// ===================================================================== /v8/finance/chart
// One call per holding, only to learn the quote currency (and a nicer name). Cached in NVS.
bool pfParseChartMeta(const char* json, char* cur, size_t curCap, char* name, size_t nameCap) {
  if (!json) return false;
  const char* mb;
  const char* me;
  if (!jlFindRange(json, nullptr, "meta", mb, me)) return false;
  if (name && nameCap) {
    if (!jlStr(jlFind(mb, me, "shortName"), name, nameCap)) jlStr(jlFind(mb, me, "longName"), name, nameCap);
  }
  return jlStr(jlFind(mb, me, "currency"), cur, curCap) && strlen(cur) >= 3;
}

// ===================================================================== /v8/finance/spark
// { "AAPL": { "timestamp": [...], "close": [...], "fulldayChangePercent": -2.04 }, ... }
// One request covers every holding and every FX pair.
static bool sparkSeries(const char* json, const char* symbol,
                        float* close, long* day, int& n, float* dayPct) {
  n = 0;
  const char* ob;
  const char* oe;
  const char* root = jlWs(json);
  if (!root || *root != '{') return false;
  const char* rootEnd = jlSkipValue(root);
  if (!rootEnd) return false;
  if (!jlMember(root, rootEnd, symbol, ob, oe)) return false;

  static float rawClose[MAX_PTS * 4];
  static long  rawDay[MAX_PTS * 4];
  int nc = jlNumArray(jlFind(ob, oe, "close"), rawClose, MAX_PTS * 4, 0.0f);
  int nt = jlLongArray(jlFind(ob, oe, "timestamp"), rawDay, MAX_PTS * 4);
  if (nc <= 0 || nt <= 0) return false;
  int m = nc < nt ? nc : nt;
  if (m > MAX_PTS * 4) m = MAX_PTS * 4;

  // Keep the most recent MAX_PTS points that actually have a close (Yahoo nulls out holidays).
  for (int i = 0; i < m; ++i) {
    if (!(rawClose[i] > 0) || rawDay[i] <= 0) continue;
    if (n == MAX_PTS) {
      memmove(close, close + 1, sizeof(float) * (MAX_PTS - 1));
      memmove(day, day + 1, sizeof(long) * (MAX_PTS - 1));
      --n;
    }
    close[n] = rawClose[i];
    day[n] = rawDay[i] / 86400L;    // UTC day; every exchange we use opens after 00:00 UTC
    ++n;
  }
  if (n == 0) return false;

  if (dayPct) {
    float v;
    if (jlNum(jlFind(ob, oe, "fulldayChangePercent"), v) && isfinite(v)) *dayPct = v;
    else if (n >= 2 && close[n - 2] > 0) *dayPct = (close[n - 1] / close[n - 2] - 1.0f) * 100.0f;
    else *dayPct = 0;
  }
  return true;
}

bool pfParseSpark(const char* json) {
  bool any = false;
  for (int i = 0; i < pf.nh; ++i) {
    Holding& h = pf.h[i];
    h.priced = false;
    if (!h.symbol[0]) continue;
    if (sparkSeries(json, h.symbol, h.close, h.day, h.n, &h.dayPct)) {
      h.price = h.close[h.n - 1];
      h.priced = true;
      any = true;
    }
  }
  for (int i = 0; i < pf.nfx; ++i) sparkSeries(json, pf.fx[i].pair, pf.fx[i].rate, pf.fx[i].day, pf.fx[i].n, nullptr);
  return any;
}

// ===================================================================== fetch plumbing
void pfPrepareFx() {
  pf.nfx = 0;
  for (int i = 0; i < pf.nh; ++i) {
    const char* cur = pf.h[i].cur;
    if (!cur[0]) continue;
    char up[8];
    curUpper(cur, up, sizeof up);
    if (!strcmp(up, pf.base)) continue;
    bool seen = false;
    for (int k = 0; k < pf.nfx; ++k)
      if (!strcmp(pf.fx[k].cur, cur)) seen = true;
    if (seen || pf.nfx >= MAX_CUR) continue;
    Fx& f = pf.fx[pf.nfx++];
    memset(&f, 0, sizeof f);
    cpy(f.cur, cur, sizeof f.cur);
    f.scale = curScale(cur);
    char base[8];
    cpy(base, pf.base, sizeof base);
    snprintf(f.pair, sizeof f.pair, "%.6s%.6s=X", up, base);
  }
}

int pfSymbolList(char* out, size_t cap) {
  size_t len = 0;
  int n = 0;
  out[0] = 0;
  for (int i = 0; i < pf.nh; ++i) {
    if (!pf.h[i].symbol[0]) continue;
    bool dup = false;
    for (int k = 0; k < i; ++k)
      if (!strcmp(pf.h[k].symbol, pf.h[i].symbol)) dup = true;
    if (dup) continue;
    size_t need = strlen(pf.h[i].symbol) + (len ? 1 : 0);
    if (len + need + 1 >= cap) break;
    if (len) out[len++] = ',';
    strcpy(out + len, pf.h[i].symbol);
    len += strlen(pf.h[i].symbol);
    ++n;
  }
  for (int i = 0; i < pf.nfx; ++i) {
    size_t need = strlen(pf.fx[i].pair) + (len ? 1 : 0);
    if (len + need + 1 >= cap) break;
    if (len) out[len++] = ',';
    strcpy(out + len, pf.fx[i].pair);
    len += strlen(pf.fx[i].pair);
    ++n;
  }
  return n;
}

int pfFirstUnresolved() {
  for (int i = 0; i < pf.nh; ++i)
    if (pf.h[i].isIsin && !pf.h[i].symbol[0]) return i;
  return -1;
}

int pfFirstUnknownCurrency() {
  for (int i = 0; i < pf.nh; ++i)
    if (pf.h[i].symbol[0] && !pf.h[i].cur[0]) return i;
  return -1;
}

// ===================================================================== derived numbers
// Last value at or before `day`, or the first value if `day` predates the series (so a holding
// that started trading mid-window does not drag the total to zero).
static float atDay(const float* v, const long* d, int n, long day) {
  if (n <= 0) return 0;
  float out = v[0];
  for (int i = 0; i < n && d[i] <= day; ++i) out = v[i];
  return out;
}

float pfRateFor(const char* cur, long day) {
  if (!cur || !cur[0]) return 1.0f;
  char up[8];
  curUpper(cur, up, sizeof up);
  if (!strcmp(up, pf.base) && curScale(cur) == 1.0f) return 1.0f;
  for (int i = 0; i < pf.nfx; ++i) {
    if (strcmp(pf.fx[i].cur, cur)) continue;
    if (pf.fx[i].n <= 0) return 0;                       // rate unknown: leave the holding out
    return atDay(pf.fx[i].rate, pf.fx[i].day, pf.fx[i].n, day) * pf.fx[i].scale;
  }
  return !strcmp(up, pf.base) ? curScale(cur) : 0.0f;
}

static void gridInsert(long d) {
  for (int i = 0; i < pf.ngrid; ++i)
    if (pf.grid[i] == d) return;
  if (pf.ngrid < MAX_PTS) {
    int i = pf.ngrid++;
    for (; i > 0 && pf.grid[i - 1] > d; --i) pf.grid[i] = pf.grid[i - 1];
    pf.grid[i] = d;
    return;
  }
  if (d <= pf.grid[0]) return;                           // older than everything we keep
  int i = 0;
  for (; i < MAX_PTS - 1 && pf.grid[i + 1] < d; ++i) pf.grid[i] = pf.grid[i + 1];
  pf.grid[i] = d;
}

void pfCompute() {
  pf.ngrid = 0;
  pf.nPriced = 0;
  pf.value = pf.dayDelta = pf.dayPct = 0;

  for (int i = 0; i < pf.nh; ++i) {
    pf.h[i].value = pf.h[i].dayDelta = 0;
    if (!pf.h[i].priced) continue;
    pf.nPriced++;
    for (int k = 0; k < pf.h[i].n; ++k) gridInsert(pf.h[i].day[k]);
  }
  if (!pf.ngrid) return;

  for (int g = 0; g < pf.ngrid; ++g) {
    float sum = 0;
    for (int i = 0; i < pf.nh; ++i) {
      const Holding& h = pf.h[i];
      if (!h.priced) continue;
      float rate = pfRateFor(h.cur, pf.grid[g]);
      if (rate <= 0) continue;
      sum += h.qty * atDay(h.close, h.day, h.n, pf.grid[g]) * rate;
    }
    pf.total[g] = sum;
  }

  long last = pf.grid[pf.ngrid - 1];
  for (int i = 0; i < pf.nh; ++i) {
    Holding& h = pf.h[i];
    if (!h.priced) continue;
    float rate = pfRateFor(h.cur, last);
    if (rate <= 0) continue;
    h.value = h.qty * h.price * rate;
    if (pf.ngrid >= 2) {
      float prevRate = pfRateFor(h.cur, pf.grid[pf.ngrid - 2]);
      h.dayDelta = h.value - h.qty * atDay(h.close, h.day, h.n, pf.grid[pf.ngrid - 2]) * prevRate;
    }
  }

  pf.value = pf.total[pf.ngrid - 1];
  if (pf.ngrid >= 2 && pf.total[pf.ngrid - 2] > 0) {
    pf.dayDelta = pf.value - pf.total[pf.ngrid - 2];
    pf.dayPct = pf.dayDelta / pf.total[pf.ngrid - 2] * 100.0f;
  }
}

bool pfWindow(int calendarDays, int& from, int& to) {
  if (pf.ngrid <= 0) return false;
  to = pf.ngrid - 1;
  long cutoff = pf.grid[to] - (calendarDays - 1);
  from = to;
  while (from > 0 && pf.grid[from - 1] >= cutoff) --from;
  return to > from;
}
