#pragma once
#include "Arduino.h"
#include "WiFiClientSecure.h"
#define HTTP_CODE_OK 200
struct HTTPClient {
  void setConnectTimeout(int) {}
  void setTimeout(int) {}
  void setUserAgent(const String&) {}
  bool begin(WiFiClientSecure&, const String&) { return false; }
  int GET() { return 0; }
  String getString() { return ""; }
  void end() {}
};
