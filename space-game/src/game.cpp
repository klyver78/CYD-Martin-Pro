// STAR RING - the game. See game.h.
//
// Geometry: everything lives in polar coordinates around the screen centre. The ship moves on
// a ring of radius R and always points at the centre. Enemies spawn in the centre (far away)
// and travel out to a fixed target point on the ring; they grow as they come closer, which
// together with the starfield gives the "flying into the screen" depth. Bullets are fired
// automatically from the ship towards the centre.
#include "game.h"
#include <esp_random.h>

char        gFpsText[16]    = "";
char        gStatusText[48] = "";
const char* gVersion        = "dev";

// ---------------------------------------------------------------- tuning
static const int   CX = SCR_W / 2, CY = 126;      // ring centre (HUD sits above)
static const float R  = 102;                      // ring radius
static const float TURN_SPEED   = 3.4f;           // rad/s
static const float FIRE_EVERY   = 0.17f;          // s
static const int   START_LIVES  = 3;
static const float TAU = 6.2831853f;

// ---------------------------------------------------------------- colours (byte-swapped)
static const uint16_t C_TEXT   = rgbBE(235, 238, 245);
static const uint16_t C_DIM    = rgbBE(120, 130, 155);
static const uint16_t C_ACC    = rgbBE(255, 203, 45);
static const uint16_t C_RING   = rgbBE(40, 70, 130);
static const uint16_t C_SHIP   = rgbBE(120, 230, 255);
static const uint16_t C_SHIP2  = rgbBE(30, 120, 200);
static const uint16_t C_FLAME  = rgbBE(255, 140, 30);
static const uint16_t C_BULLET = rgbBE(255, 245, 140);
static const uint16_t C_SAU_A  = rgbBE(255, 120, 40);   // saucer
static const uint16_t C_SAU_B  = rgbBE(150, 20, 30);
static const uint16_t C_SAU_C  = rgbBE(255, 230, 90);
static const uint16_t C_SPI_A  = rgbBE(70, 230, 110);   // spinner
static const uint16_t C_SPI_B  = rgbBE(190, 255, 200);
static const uint16_t C_WHITE  = 0xFFFF;

// ---------------------------------------------------------------- state
struct Star     { float x, y, z; };
struct Enemy    { bool alive; uint8_t type, hp; float r, a, target, spiral, phase, flash, speed; };
struct Bullet   { bool alive; float r, a; };
struct Particle { float x, y, vx, vy, life; uint16_t c; };

static const int N_STARS = 170, N_ENEMIES = 24, N_BULLETS = 16, N_PARTS = 120;
static Star     stars[N_STARS];
static Enemy    enemies[N_ENEMIES];
static Bullet   bullets[N_BULLETS];
static Particle parts[N_PARTS];

static GameState state = GS_TITLE;
static float     shipA = TAU / 4, shipVel = 0;     // start at the bottom of the ring
static float     fireT = 0, spawnT = 1.0f, playT = 0, invuln = 0, overT = 0, starSpeed = 0.45f;
static int       lives = START_LIVES;
static uint32_t  score = 0, best = 0;

// ---------------------------------------------------------------- helpers
static float frand() { return (esp_random() & 0xFFFFFF) / 16777216.0f; }
static float wrapPi(float a) { while (a > PI) a -= TAU; while (a < -PI) a += TAU; return a; }
static void  polar(float r, float a, float& x, float& y) { x = CX + cosf(a) * r; y = CY + sinf(a) * r; }
static float enemySize(const Enemy& e) { return 3 + 14 * (e.r / R); }

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
  for (auto& b : bullets) b.alive = false;
  lives = START_LIVES; score = 0; playT = 0; spawnT = 1.2f; fireT = 0; invuln = 1.5f;
  shipA = TAU / 4; shipVel = 0;
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
    e.speed  = (e.type ? 0.42f : 0.32f) * (1 + 0.7f * diff) * (0.85f + frand() * 0.3f);
    return;
  }
}

static void loseLife(float x, float y) {
  burst(x, y, 40, C_SHIP, C_FLAME, 140);
  if (--lives <= 0) {
    state = GS_OVER; overT = 0;
    if (score > best) best = score;
  } else invuln = 2.0f;
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
  // particles (always)
  for (auto& p : parts) {
    if (p.life <= 0) continue;
    p.life -= dt; p.x += p.vx * dt; p.y += p.vy * dt; p.vx *= 0.98f; p.vy *= 0.98f;
  }

  if (state == GS_TITLE) {
    shipA += dt * 0.6f;                                            // idle orbit for show
    if (in.tap && !gameTitleUpdateHit(in.tapX, in.tapY)) { resetPlay(); state = GS_PLAY; }
    return;
  }
  if (state == GS_OVER) {
    overT += dt;
    for (auto& e : enemies) if (e.alive) { e.r += dt * 60; if (e.r > R + 40) e.alive = false; }
    if (in.tap && overT > 1.0f) { resetPlay(); state = GS_PLAY; }
    return;
  }

  // ---- playing
  playT += dt;
  if (invuln > 0) invuln -= dt;

  // ship: smoothed angular velocity around the ring
  shipVel += (in.turn * TURN_SPEED - shipVel) * min(1.0f, dt * 14);
  shipA = wrapPi(shipA + shipVel * dt);

  // auto fire
  fireT -= dt;
  if (fireT <= 0) {
    fireT += FIRE_EVERY;
    for (auto& b : bullets) if (!b.alive) { b = {true, R - 16, shipA}; break; }
  }
  for (auto& b : bullets) {
    if (!b.alive) continue;
    b.r -= dt * (110 + 340 * (b.r / R));                          // perspective: slower far away
    if (b.r < 5) b.alive = false;
  }

  // spawn
  spawnT -= dt;
  if (spawnT <= 0) {
    spawnEnemy();
    spawnT = max(0.42f, 1.7f - playT * 0.012f) * (0.7f + frand() * 0.6f);
  }

  // enemies
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
      if (invuln <= 0 && fabsf(wrapPi(e.target - shipA)) * R < 20) loseLife(x, y);
      else burst(x, y, 10, e.type ? C_SPI_A : C_SAU_A, C_DIM, 60);
      continue;
    }
    // bullets
    float s = enemySize(e);
    for (auto& b : bullets) {
      if (!b.alive) continue;
      if (fabsf(b.r - e.r) > s + 4) continue;
      if (fabsf(wrapPi(b.a - e.a)) * e.r > s + 3) continue;
      b.alive = false;
      if (--e.hp == 0) {
        e.alive = false;
        score += e.type ? 250 : 100;
        float x, y; polar(e.r, e.a, x, y);
        burst(x, y, e.type ? 26 : 18, e.type ? C_SPI_A : C_SAU_A, e.type ? C_SPI_B : C_SAU_C, 40 + 90 * p);
      } else e.flash = 0.12f;
      break;
    }
  }
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

static void drawRing(Canvas& c) {
  if (c.y0 > CY + R + 2 || c.y0 + c.h < CY - R - 2) return;
  for (int i = 0; i < 90; i++) {
    float a = i * TAU / 90;
    c.fillRect(CX + (int)(cosf(a) * R), CY + (int)(sinf(a) * R), 2, 2, C_RING);
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
  c.fillTriangle(nx, ny, lx, ly, mx, my, C_SHIP);
  c.fillTriangle(nx, ny, rx, ry, mx, my, C_SHIP2);
  P(9, 0, cx1, cy1); P(1, 3, cx2, cy2); P(1, -3, cx3, cy3);
  c.fillTriangle(cx1, cy1, cx2, cy2, cx3, cy3, C_WHITE);
}

static void drawSaucer(Canvas& c, int x, int y, float s, bool flash) {
  int si = max(2, (int)s);
  c.fillRect(x - (int)(s * 1.5f), y - (int)(s * 0.2f), (int)(s * 3), max(1, (int)(s * 0.4f)), flash ? C_WHITE : C_SAU_A);
  c.fillCircle(x, y, si, flash ? C_WHITE : C_SAU_A);
  c.fillCircle(x, y, max(1, (int)(s * 0.62f)), C_SAU_B);
  c.fillCircle(x, y - (int)(s * 0.1f), max(1, (int)(s * 0.3f)), C_SAU_C);
}

static void drawSpinner(Canvas& c, int x, int y, float s, float ph, bool flash) {
  uint16_t col = flash ? C_WHITE : C_SPI_A;
  for (int k = 0; k < 3; k++) {
    float a = ph + k * TAU / 3, ca = cosf(a), sa = sinf(a);
    c.fillTriangle(x + (int)(ca * s * 1.5f), y + (int)(sa * s * 1.5f),
                   x + (int)(-sa * s * 0.5f), y + (int)(ca * s * 0.5f),
                   x + (int)(sa * s * 0.5f), y + (int)(-ca * s * 0.5f), col);
  }
  c.fillCircle(x, y, max(1, (int)(s * 0.45f)), flash ? C_WHITE : C_SPI_B);
}

static void drawEnemies(Canvas& c) {
  for (auto& e : enemies) {
    if (!e.alive) continue;
    float s = enemySize(e), x, y; polar(e.r, e.a, x, y);
    if (y + s * 1.6f < c.y0 || y - s * 1.6f >= c.y0 + c.h) continue;
    if (e.type == 0) drawSaucer(c, (int)x, (int)y, s, e.flash > 0);
    else drawSpinner(c, (int)x, (int)y, s, e.phase, e.flash > 0);
  }
}

static void drawBullets(Canvas& c) {
  for (auto& b : bullets) {
    if (!b.alive) continue;
    float x, y; polar(b.r, b.a, x, y);
    int sz = 2 + (int)(3 * b.r / R);
    c.fillRect((int)x - sz / 2, (int)y - sz / 2, sz, sz, C_BULLET);
  }
}

static void drawParticles(Canvas& c) {
  for (auto& p : parts) if (p.life > 0) c.fillRect((int)p.x, (int)p.y, 2, 2, p.c);
}

static void drawHud(Canvas& c) {
  if (c.y0 >= 20 && c.y0 + c.h < SCR_H - 12) return;
  char t[24];
  if (state != GS_TITLE) {
    snprintf(t, sizeof t, "%06u", (unsigned)score);
    c.text(6, 4, t, C_ACC, 2);
    for (int i = 0; i < lives; i++) {                              // lives as mini ships
      int x = SCR_W - 16 - i * 18;
      c.fillTriangle(x, 4, x - 7, 17, x + 7, 17, C_SHIP);
    }
  }
  c.text(4, SCR_H - 10, gFpsText, C_DIM, 1);
}

static void drawTitle(Canvas& c) {
  if (c.y0 > 200 || c.y0 + c.h < 20) { drawHud(c); return; }
  ctext(c, SCR_W / 2, 70, "STAR RING", C_ACC, 4);
  ctext(c, SCR_W / 2, 112, "Hold LEFT / RIGHT to orbit", C_TEXT, 1);
  ctext(c, SCR_W / 2, 124, "Auto fire - shoot what comes", C_TEXT, 1);
  ctext(c, SCR_W / 2, 150, "TAP TO START", C_WHITE, 2);
  char t[32]; snprintf(t, sizeof t, "BEST %06u", (unsigned)best);
  ctext(c, SCR_W / 2, 176, t, C_DIM, 1);
  // update button + status
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
  drawRing(c);
  drawEnemies(c);
  drawBullets(c);
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
  drawHud(c);
}
