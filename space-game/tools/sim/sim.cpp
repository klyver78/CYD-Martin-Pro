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
static int collected = 0, lostPu = 0;
// Simple bot: go to where the next threat/power-up will reach the ring, fire when an enemy
// outside the shield zone is on our line and no power-up is in the way.
static GameInput bot() {
  GameInput in;
  float goal = shipA; float bestR = -1; bool goalIsPu = false;
  for (auto& p : pups) if (p.alive && p.r > bestR) { bestR = p.r + 30; goal = p.a; goalIsPu = true; }
  if (!goalIsPu) for (auto& e : enemies) if (e.alive && e.r > bestR) { bestR = e.r; goal = e.target; }
  float d = wrapPi(goal - shipA);
  bool aligned = fabsf(d) * R < 8;
  bool puInLine = false;
  for (auto& p : pups) if (p.alive && fabsf(wrapPi(p.a - shipA)) * p.r < puSize(p) + 6) puInLine = true;
  bool enemyInLine = false;
  for (auto& e : enemies) if (e.alive && e.r > IMMUNE_R && fabsf(wrapPi(e.a - shipA)) * e.r < enemySize(e) + 6) enemyInLine = true;
  if (!aligned) in.turn = d > 0 ? 1 : -1;
  else if (enemyInLine && !puInLine) in.fire = true;
  return in;
}
static float playBot(float secs, const char* shotName = nullptr, int wantWeapon = -1) {
  float t = 0;
  for (; t < secs && gameState() == GS_PLAY; t += 1 / 60.0f) {
    Weapon before = weapon;
    gameUpdate(bot(), 1 / 60.0f);
    if (weapon != before && weapon != W_GUN) collected++;
    if (shotName && wantWeapon >= 0 && weapon == wantWeapon && firing && (wantWeapon != W_LASER || laserOn)) {
      bool busy = false; for (auto& e : enemies) if (e.alive && e.r > IMMUNE_R) busy = true;
      if (busy && (wantWeapon != W_GRENADE || blasts[0].life > 0 || blasts[1].life > 0)) { save(shotName); shotName = nullptr; }
    }
  }
  return t;
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
  // the shield: enemies near the centre are ghosted and shots fizzle
  run(4, 0); save("play");
  // force each power-up so the screenshots show them in action
  int ws[] = {W_SPREAD, W_LASER, W_GRENADE}; const char* names[] = {"spread", "laser", "grenade"};
  for (int i = 0; i < 3; i++) {
    lives = 3; state = GS_PLAY; weapon = (Weapon)ws[i]; weaponT = 30; ammo = 30;
    playBot(30, names[i], ws[i]);
  }
  weapon = W_GUN; lives = 3; state = GS_PLAY; puT = 0;
  // a power-up on its way out
  for (int i = 0; i < 60 * 40 && gameState() == GS_PLAY; i++) {
    bool any = false; for (auto& p : pups) if (p.alive && p.r > IMMUNE_R * 1.3f) any = true;
    if (any) { save("powerup"); break; }
    gameUpdate(bot(), 1 / 60.0f);
  }
  // let the bot play a full game for stats
  resetPlay(); state = GS_PLAY;
  float t = playBot(600);
  printf("bot survived %.0f s, score %u, power-ups collected %d\n", t, (unsigned)gameScore(), collected);
  run(1.5f, 0); save("gameover");
  printf("score %u state %d\n", (unsigned)gameScore(), (int)gameState());
}
