#pragma once
#include "Arduino.h"
struct Preferences {
  bool begin(const char*, bool) { return true; }
  void end() {}
  String getString(const char*, const char* d) { return d; }
  unsigned getUInt(const char*, unsigned d) { return d; }
  void putString(const char*, const String&) {}
  void putUInt(const char*, unsigned) {}
};
