// What a ROM provides to the shell: a list of scenes (grouped into tabs) that the shell can
// start, run frame by frame (measuring FPS) and stop.
#pragma once
#include <Arduino.h>

struct SceneInfo {
  uint8_t     group;     // index into groupName()
  const char* name;      // short, max ~12 chars (menu button)
};

class Experiment {
public:
  virtual ~Experiment() {}
  virtual int         groupCount() = 0;
  virtual const char* groupName(int g) = 0;
  virtual int         sceneCount() = 0;
  virtual SceneInfo   scene(int i) = 0;

  // start() allocates what the scene needs; stop() must free ALL of it (the shell needs the
  // heap back for TLS when it fetches the ROM list / runs OTA).
  virtual bool start(int i) = 0;
  virtual void stop() = 0;

  // Render one step. Return the number of complete frames shown (normally 1; LVGL may
  // return 0 when nothing was redrawn).
  virtual int frame() = 0;

  // Optional tunable (e.g. number of sprites). Shown as a [-] value [+] bar at the bottom.
  virtual bool   hasParam() { return false; }
  virtual void   param(int delta) { (void)delta; }
  virtual String paramLabel() { return ""; }

  // Optional extra metric shown next to the FPS (e.g. "9.8 MB/s").
  virtual String metric() { return ""; }

  // Draws its own FPS overlay (LVGL); the shell then skips its overlay.
  virtual bool ownOverlay() { return false; }
};
