#pragma once
#include "Arduino.h"
enum { WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
enum { WIFI_OFF = 0, WIFI_STA = 1 };
struct IPAddress { String toString() const { return "192.168.1.42"; } };
struct WiFiClass {
  int st = WL_CONNECTED;
  int status() { return st; } int getMode() { return WIFI_STA; } void mode(int) {}
  void begin(const char*, const char*) {} void disconnect(bool) {} void setAutoReconnect(bool) {}
  IPAddress localIP() { return {}; } int RSSI() { return -58; }
};
extern WiFiClass WiFi;
