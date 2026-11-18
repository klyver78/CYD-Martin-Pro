// STAR RING - the game. See game.h.
//
// Geometry: everything lives in polar coordinates around the screen centre. The ship moves on
// a ring of radius R and always points at the centre. Enemies spawn in the centre (far away)
// and travel out to a fixed target point on the ring; they grow as they come closer, which
// together with the starfield gives the "flying into the screen" depth.
//
// Rules:
//  * Enemies (and power-ups) are SHIELDED inside the inner zone (r < IMMUNE_R): shots pass
//    through/fizzle. So you can't camp one spot and shoot everything as it leaves the centre -
//    you have to get under an enemy's path once it's out of the zone.
//  * Fire only while the middle third of the screen is held. Left/right third = orbit.
//  * Power-ups (Spread, Laser, Grenades) fly out like enemies; catch them by being where they
//    reach the ring. Shooting one destroys it.
#include "game.h"
#include <esp_random.h>

char        gFpsText[16]    = "";
char        gStatusText[48] = "";
const char* gVersion        = "dev";

// ---------------------------------------------------------------- tuning
static const int   CX = SCR_W / 2, CY = 126;      // ring centre (HUD sits above)
static const float R  = 102;                      // ring radius
static const float IMMUNE_R     = 0.42f * R;      // shielded inside this radius
static const float TURN_SPEED   = 3.4f;           // rad/s
static const float CATCH_DIST   = 20;             // px along the ring: hit / catch distance
static const int   START_LIVES  = 3;
static const float ENEMY_SPEED[2] = {0.24f, 0.31f};   // saucer, spinner (ring radii / s, base)
static const float PU_SPEED     = 0.22f;
static const float TAU = 6.2831853f;

// weapons
enum Weapon : uint8_t { W_GUN, W_SPREAD, W_LASER, W_GRENADE };
static const char*  WEAPON_NAME[] = {"GUN", "SPREAD", "LASER", "GRENADE"};
static const float  GUN_EVERY = 0.17f, SPREAD_EVERY = 0.15f, GRENADE_EVERY = 0.5f;
static const float  SPREAD_TIME = 15, LASER_TIME = 9, LASER_DPS = 7;
static const int    GRENADE_AMMO = 8;
static const float  BLAST_R = 34;                 // px

// ---------------------------------------------------------------- colours (byte-swapped)
static const uint16_t C_TEXT   = rgbBE(235, 238, 245);
static const uint16_t C_DIM    = rgbBE(120, 130, 155);
static const uint16_t C_ACC    = rgbBE(255, 203, 45);
static const uint16_t C_RING   = rgbBE(40, 70, 130);
static const uint16_t C_ZONE   = rgbBE(70, 30, 90);
static const uint16_t C_SHIP   = rgbBE(120, 230, 255);
static const uint16_t C_SHIP2  = rgbBE(30, 120, 200);
static const uint16_t C_FLAME  = rgbBE(255, 140, 30);
static const uint16_t C_BULLET = rgbBE(255, 245, 140);
static const uint16_t C_SAU_A  = rgbBE(255, 120, 40);   // saucer
static const uint16_t C_SAU_B  = rgbBE(150, 20, 30);
static const uint16_t C_SAU_C  = rgbBE(255, 230, 90);
static const uint16_t C_SPI_A  = rgbBE(70, 230, 110);   // spinner
static const uint16_t C_SPI_B  = rgbBE(190, 255, 200);
static const uint16_t C_GHOST  = rgbBE(60, 60, 85);     // shielded enemy
static const uint16_t C_GHOST2 = rgbBE(35, 35, 50);
static const uint16_t C_WHITE  = 0xFFFF;
static const uint16_t PU_COL[] = {0, rgbBE(80, 160, 255), rgbBE(255, 70, 220), rgbBE(170, 255, 60)};
static const char     PU_CHAR[] = {' ', 'S', 'L', 'G'};

// ---------------------------------------------------------------- state
struct Star     { float x, y, z; };
struct Enemy    { bool alive; uint8_t type; float hp, r, a, target, spiral, phase, flash, speed; };
struct Shot     { bool alive; bool grenade; float r, a; };
struct PowerUp  { bool alive; uint8_t type; float r, a, phase; };
struct Particle { float x, y, vx, vy, life; uint16_t c; };
struct Blast    { float x, y, life; };

static const int N_STARS = 170, N_ENEMIES = 24, N_SHOTS = 40, N_PU = 3, N_PARTS = 140, N_BLASTS = 6;
static Star     stars[N_STARS];
static Enemy    enemies[N_ENEMIES];
static Shot     shots[N_SHOTS];
static PowerUp  pups[N_PU];
static Particle parts[N_PARTS];
static Blast    blasts[N_BLASTS];

static GameState state = GS_TITLE;
static float     shipA = TAU / 4, shipVel = 0;     // start at the bottom of the ring
static float     fireT = 0, spawnT = 1.0f, puT = 8, playT = 0, invuln = 0, overT = 0, starSpeed = 0.45f;
static int       lives = START_LIVES;
static uint32_t  score = 0, best = 0;
static Weapon    weapon = W_GUN;
static float     weaponT = 0;                      // seconds left (spread / laser)
static int       ammo = 0;                         // grenades left
static bool      laserOn = false, firing = false;
static char      msg[24] = "";
static float     msgT = 0;

// ---------------------------------------------------------------- helpers
static float frand() { return (esp_random() & 0xFFFFFF) / 16777216.0f; }
static float wrapPi(float a) { while (a > PI) a -= TAU; while (a < -PI) a += TAU; return a; }
static void  polar(float r, float a, float& x, float& y) { x = CX + cosf(a) * r; y = CY + sinf(a) * r; }
static float enemySize(const Enemy& e) { return 3 + 14 * (e.r / R); }
static float puSize(const PowerUp& p) { return 3 + 10 * (p.r / R); }
// does a shot at (r, a) touch an object of size s at (or, oa)?
static bool  touches(float r, float a, float orr, float oa, float s, float extra) {
  return fabsf(r - orr) <= s + extra && fabsf(wrapPi(a - oa)) * orr <= s + extra - 1;
}
static void showMsg(const char* m) { strlcpy(msg, m, sizeof msg); msgT = 1.6f; }

static void respawnStar(Star& s, bool anyDepth) {
  s.x = frand() * 2 - 1; s.y = frand() * 2 - 1;
  s.z = anyDepth ? 0.05f + frand() * 0.95f : 0.8f + frand() * 0.2f;
}

static void burst(float x, float y, int n, uint16_t c1, uint16_t c2, float speed) {
  for (int k = 0; k < n; k++) {
    for (int i = 0; i < N_PARTS; i++) {
      if (parts[i].life > 0) continue;
      float a = frand() * TAU, v = speed * (0.3f + frand());
      parts[i] = {x, y, cosf(a) * v, sinf(a) * v, 0.4f + frand() * 0.5f, (k & 1) ? c1 : c2};
      break;
    }
  }
}

static void resetPlay() {
  for (auto& e : enemies) e.alive = false;
  for (auto& b : shots) b.alive = false;
  for (auto& p : pups) p.alive = false;
  for (auto& b : blasts) b.life = 0;
  lives = START_LIVES; score = 0; playT = 0; spawnT = 1.2f; puT = 8 + frand() * 6; fireT = 0; invuln = 1.5f;
  shipA = TAU / 4; shipVel = 0;
  weapon = W_GUN; weaponT = 0; ammo = 0; laserOn = false; msgT = 0;
}

// ---------------------------------------------------------------- public
void gameInit(uint32_t seed) {
  (void)seed;
  for (auto& s : stars) respawnStar(s, true);
  for (auto& p : parts) p.life = 0;
  resetPlay();
  state = GS_TITLE;
}
GameState gameState() { return state; }
uint32_t  gameScore() { return score; }
uint32_t  gameBest() { return best; }
void      gameSetBest(uint32_t b) { best = b; }

static const int UPD_X = SCR_W - 84, UPD_Y = 4, UPD_W = 80, UPD_H = 22;
bool gameTitleUpdateHit(int x, int y) {
  return state == GS_TITLE && x >= UPD_X && x < UPD_X + UPD_W && y >= UPD_Y && y < UPD_Y + UPD_H;
}

static void spawnEnemy() {
  for (auto& e : enemies) {
    if (e.alive) continue;
    float diff = min(1.0f, playT / 120.0f);                        // 0..1 over two minutes
    e.alive  = true;
    e.type   = frand() < 0.12f + 0.33f * diff ? 1 : 0;
    e.hp     = e.type ? 2 : 1;
    e.r      = 2;
    e.target = frand() * TAU;
    e.spiral = e.type ? (frand() < 0.5f ? -1 : 1) * (2.5f + frand() * 1.5f) : 0;
    e.a      = e.target + e.spiral;
    e.phase  = frand() * TAU;
    e.flash  = 0;
    e.speed  = ENEMY_SPEED[e.type] * (1 + 0.6f * diff) * (0.85f + frand() * 0.3f);
    return;
  }
}

static void spawnPowerUp() {
  for (auto& p : pups) {
    if (p.alive) continue;
    p = {true, (uint8_t)(1 + esp_random() % 3), 2, frand() * TAU, 0};
    return;
  }
}

static void loseLife(float x, float y) {
  burst(x, y, 40, C_SHIP, C_FLAME, 140);
  weapon = W_GUN; laserOn = false;                                 // lose the power-up too
  if (--lives <= 0) {
    state = GS_OVER; overT = 0;
    if (score > best) best = score;
  } else invuln = 2.0f;
}

static void killEnemy(Enemy& e) {
  e.alive = false;
  score += e.type ? 250 : 100;
  float x, y; polar(e.r, e.a, x, y);
  float p = e.r / R;
  burst(x, y, e.type ? 26 : 18, e.type ? C_SPI_A : C_SAU_A, e.type ? C_SPI_B : C_SAU_C, 40 + 90 * p);
}
static void damageEnemy(Enemy& e, float dmg) {
  e.hp -= dmg;
  if (e.hp <= 0.001f) killEnemy(e);
  else e.flash = 0.12f;
}
static void destroyPowerUp(PowerUp& p) {
  p.alive = false;
  float x, y; polar(p.r, p.a, x, y);
  burst(x, y, 14, PU_COL[p.type], C_DIM, 60);
  showMsg("POWER-UP LOST");
}
static void collect(PowerUp& p) {
  p.alive = false;
  float x, y; polar(R, p.a, x, y);
  burst(x, y, 20, PU_COL[p.type], C_WHITE, 80);
  weapon = (Weapon)p.type;
  if (weapon == W_SPREAD) weaponT = SPREAD_TIME;
  if (weapon == W_LASER) weaponT = LASER_TIME;
  if (weapon == W_GRENADE) ammo = GRENADE_AMMO;
  char t[24]; snprintf(t, sizeof t, "%s!", WEAPON_NAME[weapon]);
  showMsg(t);
}

static void explode(float r, float a) {
  float x, y; polar(r, a, x, y);
  for (auto& b : blasts) if (b.life <= 0) { b = {x, y, 0.35f}; break; }
  burst(x, y, 22, C_ACC, PU_COL[W_GRENADE], 110);
  for (auto& e : enemies) {
    if (!e.alive || e.r < IMMUNE_R) continue;
    float ex, ey; polar(e.r, e.a, ex, ey);
    if ((ex - x) * (ex - x) + (ey - y) * (ey - y) <= (BLAST_R + enemySize(e)) * (BLAST_R + enemySize(e))) damageEnemy(e, 2);
  }
  for (auto& p : pups) {
    if (!p.alive || p.r < IMMUNE_R) continue;
    float px, py; polar(p.r, p.a, px, py);
    if ((px - x) * (px - x) + (py - y) * (py - y) <= BLAST_R * BLAST_R) destroyPowerUp(p);
  }
}

static void addShot(float a, bool grenade) {
  for (auto& b : shots) if (!b.alive) { b = {true, grenade, R - 16, a}; return; }
}

static void updateWeapons(const GameInput& in, float dt) {
  if (weapon == W_SPREAD || weapon == W_LASER) {
    weaponT -= dt;
    if (weaponT <= 0) { weapon = W_GUN; showMsg("GUN"); }
  }
  firing = in.fire;
  laserOn = in.fire && weapon == W_LASER;
  fireT -= dt;
  if (in.fire && !laserOn && fireT <= 0) {
    switch (weapon) {
      case W_SPREAD:  fireT = SPREAD_EVERY; addShot(shipA - 0.13f, false); addShot(shipA, false); addShot(shipA + 0.13f, false); break;
      case W_GRENADE: fireT = GRENADE_EVERY; addShot(shipA, true);
                      if (--ammo <= 0) { weapon = W_GUN; } break;
      default:        fireT = GUN_EVERY; addShot(shipA, false); break;
    }
  }
  if (fireT < 0) fireT = 0;

  // shots fly inwards (perspective: slower far away); they fizzle at the shield zone
  for (auto& b : shots) {
    if (!b.alive) continue;
    b.r -= dt * (b.grenade ? 0.6f : 1.0f) * (110 + 340 * (b.r / R));
    if (b.r < IMMUNE_R) {
      b.alive = false;
      if (b.grenade) explode(IMMUNE_R, b.a);
      else { float x, y; polar(IMMUNE_R, b.a, x, y); burst(x, y, 2, C_ZONE, C_DIM, 20); }
      continue;
    }
    for (auto& e : enemies) {
      if (!e.alive || e.r < IMMUNE_R) continue;
      if (!touches(b.r, b.a, e.r, e.a, enemySize(e), 4)) continue;
      b.alive = false;
      if (b.grenade) explode(b.r, b.a); else damageEnemy(e, 1);
      break;
    }
    if (!b.alive) continue;
    for (auto& p : pups) {
      if (!p.alive || p.r < IMMUNE_R) continue;
      if (!touches(b.r, b.a, p.r, p.a, puSize(p), 4)) continue;
      b.alive = false;
      if (b.grenade) explode(b.r, b.a); else destroyPowerUp(p);
      break;
    }
  }

  // laser: everything outside the shield zone on the ship's line takes damage
  if (laserOn) {
    for (auto& e : enemies)
      if (e.alive && e.r >= IMMUNE_R && fabsf(wrapPi(e.a - shipA)) * e.r <= enemySize(e) + 2) damageEnemy(e, LASER_DPS * dt);
    for (auto& p : pups)
      if (p.alive && p.r >= IMMUNE_R && fabsf(wrapPi(p.a - shipA)) * p.r <= puSize(p) + 2) destroyPowerUp(p);
  }
}

void gameUpdate(const GameInput& in, float dt) {
  dt = min(dt, 0.05f);                                             // avoid huge steps after a stall

  // stars (always) - flying towards us = z decreasing, faster while playing
  float targetSpeed = state == GS_PLAY ? 0.55f + 0.25f * min(1.0f, playT / 120.0f) : 0.35f;
  starSpeed += (targetSpeed - starSpeed) * min(1.0f, dt * 2);
  for (auto& s : stars) {
    s.z -= dt * starSpeed;
    float sx = CX + s.x / s.z * 60, sy = CY + s.y / s.z * 60;
    if (s.z < 0.03f || sx < 0 || sx >= SCR_W || sy < 0 || sy >= SCR_H) respawnStar(s, false);
  }
  for (auto& p : parts) {
    if (p.life <= 0) continue;
    p.life -= dt; p.x += p.vx * dt; p.y += p.vy * dt; p.vx *= 0.98f; p.vy *= 0.98f;
  }
  for (auto& b : blasts) if (b.life > 0) b.life -= dt;
  if (msgT > 0) msgT -= dt;

  if (state == GS_TITLE) {
    shipA += dt * 0.6f;                                            // idle orbit for show
    if (in.tap && !gameTitleUpdateHit(in.tapX, in.tapY)) { resetPlay(); state = GS_PLAY; }
    return;
  }
  if (state == GS_OVER) {
    overT += dt;
    laserOn = false;
    for (auto& e : enemies) if (e.alive) { e.r += dt * 60; if (e.r > R + 40) e.alive = false; }
    if (in.tap && overT > 1.0f) { resetPlay(); state = GS_PLAY; }
    return;
  }

  // ---- playing
  playT += dt;
  if (invuln > 0) invuln -= dt;

  shipVel += (in.turn * TURN_SPEED - shipVel) * min(1.0f, dt * 14);   // smoothed orbit
  shipA = wrapPi(shipA + shipVel * dt);

  // spawn
  spawnT -= dt;
  if (spawnT <= 0) {
    spawnEnemy();
    spawnT = max(0.55f, 1.9f - playT * 0.011f) * (0.7f + frand() * 0.6f);
  }
  puT -= dt;
  if (puT <= 0) { spawnPowerUp(); puT = 12 + frand() * 8; }

  // enemies move
  for (auto& e : enemies) {
    if (!e.alive) continue;
    float p = e.r / R;
    e.r += dt * e.speed * R * (0.25f + 1.2f * p);                 // accelerate as they approach
    p = min(1.0f, e.r / R);
    float rest = 1 - p;
    e.a = e.target + e.spiral * rest * rest;                       // spinners spiral in
    e.phase += dt * (e.type ? 7 : 2);
    if (e.flash > 0) e.flash -= dt;
    if (e.r >= R) {                                                // reached the ring
      e.alive = false;
      float x, y; polar(R, e.target, x, y);
      if (invuln <= 0 && fabsf(wrapPi(e.target - shipA)) * R < CATCH_DIST) loseLife(x, y);
      else burst(x, y, 10, e.type ? C_SPI_A : C_SAU_A, C_DIM, 60);
      if (state != GS_PLAY) return;
    }
  }
  // power-ups move; caught by flying into them on the ring
  for (auto& p : pups) {
    if (!p.alive) continue;
    p.r += dt * PU_SPEED * R * (0.25f + 1.2f * p.r / R);
    p.phase += dt * 3;
    if (p.r >= R) {
      if (fabsf(wrapPi(p.a - shipA)) * R < CATCH_DIST + 4) collect(p);
      else { p.alive = false; float x, y; polar(R, p.a, x, y); burst(x, y, 8, PU_COL[p.type], C_DIM, 40); }
    }
  }

  updateWeapons(in, dt);
}

// ================================================================ rendering
static void ctext(Canvas& c, int cx, int y, const char* s, uint16_t col, int size) {
  c.text(cx - Canvas::textW(s, size) / 2, y, s, col, size);
}

static void drawStars(Canvas& c) {
  for (auto& s : stars) {
    int sx = CX + (int)(s.x / s.z * 60), sy = CY + (int)(s.y / s.z * 60);
    if (sy < c.y0 - 3 || sy >= c.y0 + c.h) continue;
    int sz = s.z < 0.25f ? 3 : (s.z < 0.55f ? 2 : 1);
    uint8_t v = (uint8_t)min(255.0f, 60 + 200 * (1 - s.z));
    c.fillRect(sx, sy, sz, sz, rgbBE(v, v, (uint8_t)min(255, v + 30)));
  }
}

static void drawRings(Canvas& c) {
  if (c.y0 > CY + R + 2 || c.y0 + c.h < CY - R - 2) return;
  for (int i = 0; i < 90; i++) {
    float a = i * TAU / 90;
    c.fillRect(CX + (int)(cosf(a) * R), CY + (int)(sinf(a) * R), 2, 2, C_RING);
  }
  if (state == GS_PLAY) for (int i = 0; i < 40; i++) {                 // shield zone
    float a = i * TAU / 40;
    c.fillRect(CX + (int)(cosf(a) * IMMUNE_R), CY + (int)(sinf(a) * IMMUNE_R), 1, 1, C_ZONE);
  }
}

static void drawShip(Canvas& c, float a, bool blink) {
  if (blink) return;
  float px, py; polar(R, a, px, py);
  if (py + 22 < c.y0 || py - 22 >= c.y0 + c.h) return;
  float ux = -cosf(a), uy = -sinf(a);                   // towards the centre
  float tx = -uy, ty = ux;                              // tangent
  auto P = [&](float fwd, float side, int& x, int& y) { x = (int)(px + ux * fwd + tx * side); y = (int)(py + uy * fwd + ty * side); };
  int nx, ny, lx, ly, rx, ry, mx, my, f1x, f1y, f2x, f2y, ftx, fty, cx1, cy1, cx2, cy2, cx3, cy3;
  P(17, 0, nx, ny); P(-7, 12, lx, ly); P(-7, -12, rx, ry); P(-2, 0, mx, my);
  float flick = 5 + (esp_random() % 5);
  P(-5, 4, f1x, f1y); P(-5, -4, f2x, f2y); P(-5 - flick, 0, ftx, fty);
  c.fillTriangle(f1x, f1y, f2x, f2y, ftx, fty, C_FLAME);
  uint16_t wing = weapon == W_GUN ? C_SHIP2 : PU_COL[weapon];        // wing shows the weapon
  c.fillTriangle(nx, ny, lx, ly, mx, my, C_SHIP);
  c.fillTriangle(nx, ny, rx, ry, mx, my, wing);
  P(9, 0, cx1, cy1); P(1, 3, cx2, cy2); P(1, -3, cx3, cy3);
  c.fillTriangle(cx1, cy1, cx2, cy2, cx3, cy3, C_WHITE);
}

static void drawLaser(Canvas& c) {
  if (!laserOn) return;
  float x0, y0, x1, y1; polar(R - 18, shipA, x0, y0); polar(IMMUNE_R, shipA, x1, y1);
  if (max(y0, y1) + 3 < c.y0 || min(y0, y1) - 3 >= c.y0 + c.h) return;
  bool flick = esp_random() & 1;
  c.thickLine((int)x0, (int)y0, (int)x1, (int)y1, 5, flick ? PU_COL[W_LASER] : rgbBE(200, 40, 170));
  c.thickLine((int)x0, (int)y0, (int)x1, (int)y1, 2, C_WHITE);
}

static void drawSaucer(Canvas& c, int x, int y, float s, bool flash, bool ghost) {
  int si = max(2, (int)s);
  uint16_t a = ghost ? C_GHOST : (flash ? C_WHITE : C_SAU_A);
  c.fillRect(x - (int)(s * 1.5f), y - (int)(s * 0.2f), (int)(s * 3), max(1, (int)(s * 0.4f)), a);
  c.fillCircle(x, y, si, a);
  c.fillCircle(x, y, max(1, (int)(s * 0.62f)), ghost ? C_GHOST2 : C_SAU_B);
  c.fillCircle(x, y - (int)(s * 0.1f), max(1, (int)(s * 0.3f)), ghost ? C_GHOST : C_SAU_C);
}

static void drawSpinner(Canvas& c, int x, int y, float s, float ph, bool flash, bool ghost) {
  uint16_t col = ghost ? C_GHOST : (flash ? C_WHITE : C_SPI_A);
  for (int k = 0; k < 3; k++) {
    float a = ph + k * TAU / 3, ca = cosf(a), sa = sinf(a);
    c.fillTriangle(x + (int)(ca * s * 1.5f), y + (int)(sa * s * 1.5f),
                   x + (int)(-sa * s * 0.5f), y + (int)(ca * s * 0.5f),
                   x + (int)(sa * s * 0.5f), y + (int)(-ca * s * 0.5f), col);
  }
  c.fillCircle(x, y, max(1, (int)(s * 0.45f)), ghost ? C_GHOST2 : (flash ? C_WHITE : C_SPI_B));
}

static void drawEnemies(Canvas& c) {
  for (auto& e : enemies) {
    if (!e.alive) continue;
    float s = enemySize(e), x, y; polar(e.r, e.a, x, y);
    if (y + s * 1.6f < c.y0 || y - s * 1.6f >= c.y0 + c.h) continue;
    bool ghost = e.r < IMMUNE_R;
    if (e.type == 0) drawSaucer(c, (int)x, (int)y, s, e.flash > 0, ghost);
    else drawSpinner(c, (int)x, (int)y, s, e.phase, e.flash > 0, ghost);
  }
}

// Power-up: spinning diamond with its letter.
static void drawPowerUps(Canvas& c) {
  for (auto& p : pups) {
    if (!p.alive) continue;
    float s = puSize(p), x, y; polar(p.r, p.a, x, y);
    if (y + s * 1.5f < c.y0 || y - s * 1.5f >= c.y0 + c.h) continue;
    bool ghost = p.r < IMMUNE_R;
    uint16_t col = ghost ? C_GHOST : PU_COL[p.type];
    float w = s * (0.55f + 0.45f * fabsf(cosf(p.phase)));            // "spin" = width pulses
    int ix = (int)x, iy = (int)y;
    c.fillTriangle(ix - (int)w, iy, ix + (int)w, iy, ix, iy - (int)(s * 1.3f), col);
    c.fillTriangle(ix - (int)w, iy, ix + (int)w, iy, ix, iy + (int)(s * 1.3f), col);
    int ts = s > 9 ? 2 : 1;
    char t[2] = {PU_CHAR[p.type], 0};
    if (s > 4) c.text(ix - 3 * ts + 1, iy - 4 * ts, t, ghost ? C_GHOST2 : 0, ts);
  }
}

static void drawShots(Canvas& c) {
  for (auto& b : shots) {
    if (!b.alive) continue;
    float x, y; polar(b.r, b.a, x, y);
    if (b.grenade) {
      int r = 2 + (int)(3 * b.r / R);
      c.fillCircle((int)x, (int)y, r, PU_COL[W_GRENADE]);
      c.fillCircle((int)x, (int)y, max(1, r / 2), C_WHITE);
    } else {
      int sz = 2 + (int)(3 * b.r / R);
      c.fillRect((int)x - sz / 2, (int)y - sz / 2, sz, sz, C_BULLET);
    }
  }
}

static void drawBlasts(Canvas& c) {
  for (auto& b : blasts) {
    if (b.life <= 0) continue;
    int r = (int)(BLAST_R * (1 - b.life / 0.35f)) + 4;
    if (b.y + r < c.y0 || b.y - r >= c.y0 + c.h) continue;
    c.fillRing((int)b.x, (int)b.y, max(0, r - 3), r, C_ACC);
  }
}

static void drawParticles(Canvas& c) {
  for (auto& p : parts) if (p.life > 0) c.fillRect((int)p.x, (int)p.y, 2, 2, p.c);
}

static void drawHud(Canvas& c) {
  if (c.y0 >= 32 && c.y0 + c.h < SCR_H - 14) return;
  char t[24];
  if (state != GS_TITLE) {
    snprintf(t, sizeof t, "%06u", (unsigned)score);
    c.text(6, 4, t, C_ACC, 2);
    for (int i = 0; i < lives; i++) {                              // lives as mini ships
      int x = SCR_W - 16 - i * 18;
      c.fillTriangle(x, 4, x - 7, 17, x + 7, 17, C_SHIP);
    }
  }
  if (state == GS_PLAY) {
    // weapon (under the score)
    if (weapon == W_GUN) snprintf(t, sizeof t, "GUN");
    else if (weapon == W_GRENADE) snprintf(t, sizeof t, "GRENADE x%d", ammo);
    else snprintf(t, sizeof t, "%s %ds", WEAPON_NAME[weapon], (int)ceilf(weaponT));
    c.text(6, 22, t, weapon == W_GUN ? C_DIM : PU_COL[weapon], 1);
    // touch zones hint (bottom)
    c.text(SCR_W / 6 - 3, SCR_H - 10, "<", C_DIM, 1);
    c.text(SCR_W / 2 - 12, SCR_H - 10, "FIRE", firing ? C_BULLET : C_DIM, 1);
    c.text(SCR_W * 5 / 6 - 3, SCR_H - 10, ">", C_DIM, 1);
  }
  c.text(4, SCR_H - 10, gFpsText, C_DIM, 1);
}

static void drawTitle(Canvas& c) {
  if (c.y0 > 200 || c.y0 + c.h < 20) { drawHud(c); return; }
  ctext(c, SCR_W / 2, 62, "STAR RING", C_ACC, 4);
  ctext(c, SCR_W / 2, 100, "Hold LEFT / RIGHT third to orbit", C_TEXT, 1);
  ctext(c, SCR_W / 2, 111, "Hold the MIDDLE to fire", C_TEXT, 1);
  ctext(c, SCR_W / 2, 122, "Enemies are shielded near the centre", C_DIM, 1);
  ctext(c, SCR_W / 2, 133, "Fly into power-ups - don't shoot them", C_DIM, 1);
  ctext(c, SCR_W / 2, 152, "TAP TO START", C_WHITE, 2);
  char t[32]; snprintf(t, sizeof t, "BEST %06u", (unsigned)best);
  ctext(c, SCR_W / 2, 176, t, C_DIM, 1);
  c.fillRect(UPD_X, UPD_Y, UPD_W, UPD_H, rgbBE(28, 34, 56));
  c.rect(UPD_X, UPD_Y, UPD_W, UPD_H, rgbBE(52, 60, 90));
  ctext(c, UPD_X + UPD_W / 2, UPD_Y + 7, "UPDATE", C_TEXT, 1);
  c.text(6, 8, (String("v") + gVersion).c_str(), C_DIM, 1);
  ctext(c, SCR_W / 2, 196, gStatusText, C_DIM, 1);
  drawHud(c);
}

void gameRender(Canvas& c) {
  c.clear(0);
  drawStars(c);
  drawRings(c);
  drawEnemies(c);
  drawPowerUps(c);
  drawLaser(c);
  drawShots(c);
  drawBlasts(c);
  if (state != GS_OVER) drawShip(c, shipA, state == GS_PLAY && invuln > 0 && (int)(invuln * 10) % 2);
  drawParticles(c);
  if (state == GS_TITLE) { drawTitle(c); return; }
  if (state == GS_OVER) {
    ctext(c, SCR_W / 2, 90, "GAME OVER", rgbBE(255, 90, 80), 3);
    char t[32]; snprintf(t, sizeof t, "SCORE %06u", (unsigned)score);
    ctext(c, SCR_W / 2, 124, t, C_TEXT, 2);
    snprintf(t, sizeof t, "BEST  %06u", (unsigned)best);
    ctext(c, SCR_W / 2, 146, t, C_DIM, 2);
    if (overT > 1.0f) ctext(c, SCR_W / 2, 176, "tap to play again", C_TEXT, 1);
  }
  if (state == GS_PLAY && msgT > 0) ctext(c, SCR_W / 2, 34, msg, C_WHITE, 2);
  drawHud(c);
}
