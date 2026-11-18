#pragma once
void netSetup(const char* fwName, const char* version, void (*redrawFn)());
void netLoop();
void netCheckAndUpdate(const char* runningVersion);   // blocking; reboots if it installs
