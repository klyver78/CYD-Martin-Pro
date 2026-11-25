#pragma once
#include <stdint.h>

void netSetup(const char* fwName, const char* version, void (*redrawFn)());
void netLoop();                                     // Improv (serial) handling
bool netOnline();                                   // connect if needed, max ~12 s

bool netLoadPortfolio();                            // GET portfolio.json -> pf
bool netResolveSymbols();                           // ISIN -> symbol, symbol -> currency (NVS cached)
bool netFetchPrices();                              // one spark call -> closes + pfCompute()

void netCheckAndUpdate(const char* runningVersion);  // blocking; reboots if it installs
