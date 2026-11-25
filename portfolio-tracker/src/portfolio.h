// Portfolio model: the holdings from portfolio.json, the daily closes fetched for each of them,
// and the derived numbers the UI shows. Hardware-free and allocation-free, so the simulator can
// run the whole thing on a PC against recorded API responses.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define MAX_HOLDINGS 20
#define MAX_PTS      32      // daily closes kept per symbol (range=1mo is ~23 trading days)
#define MAX_CUR       6      // distinct quote currencies besides the base currency

struct Holding {
  char  key[20];        // exactly what portfolio.json asked for (ticker or ISIN)
  char  symbol[20];     // the API symbol: the ticker itself, or looked up from the ISIN
  char  name[24];       // display name
  char  cur[8];         // quote currency, e.g. "DKK" ("GBp" = London pence)
  bool  isIsin;
  float qty;

  // --- filled by the price fetch, in the quote currency ---
  float close[MAX_PTS];
  long  day[MAX_PTS];   // UTC day number (unix time / 86400), ascending
  int   n;
  float price;          // most recent close
  float dayPct;         // today's change in %, as the API reports it
  bool  priced;

  // --- derived, in the base currency ---
  float value;          // qty * price * fx
  float dayDelta;       // change in value since the previous day on the grid
};

struct Fx {
  char  cur[8];         // quote currency this converts from
  char  pair[16];       // API symbol, e.g. "USDDKK=X"
  float scale;          // 0.01 for pence-quoted markets, else 1
  float rate[MAX_PTS];
  long  day[MAX_PTS];
  int   n;
};

struct Portfolio {
  char    base[8];                // base currency (portfolio.json "base", default DKK)
  Holding h[MAX_HOLDINGS];
  int     nh;
  Fx      fx[MAX_CUR];
  int     nfx;

  // --- computed by pfCompute() ---
  long    grid[MAX_PTS];          // UTC day numbers that any holding traded on, ascending
  float   total[MAX_PTS];         // portfolio value in the base currency, per grid day
  int     ngrid;
  float   value;                  // today's total
  float   dayDelta, dayPct;       // change since the previous grid day
  int     nPriced;                // holdings we have a price for
};

extern Portfolio pf;

void pfReset();

// --- parsing (all tolerant: they return false and leave the model usable) ---
bool pfParsePortfolio(const char* json);                       // portfolio.json
bool pfParseSearch(const char* json, char* sym, size_t cap);   // /v1/finance/search  -> symbol
bool pfParseChartMeta(const char* json, char* cur, size_t curCap,
                      char* name, size_t nameCap);             // /v8/finance/chart   -> currency
bool pfParseSpark(const char* json);                           // /v8/finance/spark   -> closes

// --- fetch plumbing ---
void pfPrepareFx();                              // derive the FX pairs the holdings need
int  pfSymbolList(char* out, size_t cap);        // "NOVO-B.CO,AAPL,USDDKK=X" for the spark call
int  pfFirstUnresolved();                        // holding needing an ISIN lookup, or -1
int  pfFirstUnknownCurrency();                   // holding needing a currency lookup, or -1

// --- derived numbers ---
void pfCompute();
float pfRateFor(const char* cur, long day);      // quote currency -> base currency on `day`
bool  pfWindow(int calendarDays, int& from, int& to);   // grid slice covering the last N days
