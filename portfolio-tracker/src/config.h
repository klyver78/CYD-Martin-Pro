// User settings for the portfolio tracker. Change things here, not in board.h (hardware) or
// net.cpp. These are compiled into the firmware, so a change only reaches a device after it is
// rebuilt: push to main with the experiment's VERSION bumped, then tap UPDATE FW on the device.
// Each setting can also be overridden from platformio.ini, e.g. -D REFRESH_MS=60000UL.
#pragma once
#include "board.h"

// Where the device fetches portfolio.json. Any HTTPS URL that returns the file works, e.g.
//   "https://gist.githubusercontent.com/<user>/<id>/raw/portfolio.json"
// The host's certificate must chain to one of the roots in src/api_certs.h: ISRG (Let's Encrypt),
// DigiCert G2/G3, Amazon, GlobalSign R3/R46, USERTrust (Sectigo) and GoDaddy G2. If it does
// not, add its root to the WANT list in tools/gen_certs.py and regenerate.
// Default: the copy that tools/build_site.py publishes next to the web flasher.
#ifndef PORTFOLIO_URL
#define PORTFOLIO_URL  PAGES_BASE "portfolio.json"
#endif

// How often portfolio.json and the prices are re-fetched. portfolio.json is only re-parsed when
// its content changed, so an unchanged file costs one small download.
#ifndef REFRESH_MS
#define REFRESH_MS     (5 * 60 * 1000UL)
#endif
