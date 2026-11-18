// PC renderer: runs the real game code (src/game.cpp) with scripted input and writes
// screenshots (out/*.ppm -> docs/*.png). No hardware needed.
#include <cstdio>
#include "Arduino.h"
uint32_t g_millis = 0;
HardwareSerial Serial; EspClass ESP;
#include "../../src/game.cpp"

static uint16_t fb[SCR_W * SCR_H], strip[SCR_W * STRIP_H];
static void frame() {
  for (int y = 0; y < SCR_H; y += STRIP_H) {
    Canvas c; c.attach(strip, 0, y, SCR_W, STRIP_H);
    gameRender(c);
    for (int i = 0; i < SCR_W * STRIP_H; i++) fb[y * SCR_W + i] = swap16(strip[i]);
  }
}
static void save(const char* n) {
  frame();
  char p[64]; snprintf(p, sizeof p, "out/%s.ppm", n);
  FILE* f = fopen(p, "wb"); fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H);
  for (uint16_t c : fb) { uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63) << 2), (uint8_t)((c & 31) << 3)}; fwrite(rgb, 1, 3, f); }
  fclose(f);
}
static void run(float secs, int turn) {
  for (int i = 0; i < (int)(secs * 60); i++) { GameInput in; in.turn = turn; gameUpdate(in, 1 / 60.0f); }
}
int main() {
  srand(7);
  gVersion = "1.0.0";
  strcpy(gFpsText, "60 fps");
  strcpy(gStatusText, "Wi-Fi: 192.168.1.42");
  gameSetBest(12350);
  gameInit(1);
  run(2, 0); save("title");
  GameInput tap; tap.tap = true; tap.tapX = 160; tap.tapY = 120; gameUpdate(tap, 1 / 60.0f);
  // play for a while: sweep around so the autofire hits things
  run(20, 1); run(5, -1); run(18, 1);
  save("play");
  run(1.3f, -1); save("play2");
  // play until game over (no input)
  for (int i = 0; i < 60 * 300 && gameState() == GS_PLAY; i++) { GameInput in; gameUpdate(in, 1 / 60.0f); }
  run(1.5f, 0); save("gameover");
  printf("score %u state %d\n", (unsigned)gameScore(), (int)gameState());
}
