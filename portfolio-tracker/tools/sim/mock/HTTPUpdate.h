#pragma once
#include "WiFiClientSecure.h"
enum t_httpUpdate_return { HTTP_UPDATE_FAILED, HTTP_UPDATE_NO_UPDATES, HTTP_UPDATE_OK };
struct HTTPUpdateClass { void rebootOnUpdate(bool) {} t_httpUpdate_return update(WiFiClientSecure&, const String&) { return HTTP_UPDATE_FAILED; }
  String getLastErrorString() { return "sim"; } };
extern HTTPUpdateClass httpUpdate;
