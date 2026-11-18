// Game logic + renderer. No hardware here: main.cpp feeds input and time, and pushes the
// strips that renderStrip() fills. That keeps the game runnable in the PC simulator.
#pragma once
#include <Arduino.h>
#include "canvas.h"

struct GameInput {
  int8_t turn = 0;        // -1 = counter-clockwise (left half held), +1 = clockwise (right half)
  bool   tap  = false;    // a new press this frame (title / game over)
  int    tapX = 0, tapY = 0;
};

enum GameState { GS_TITLE, GS_PLAY, GS_OVER };

void        gameInit(uint32_t seed);
void        gameUpdate(const GameInput& in, float dt);   // dt in seconds
void        gameRender(Canvas& c);                       // fill one strip (every pixel)
GameState   gameState();
uint32_t    gameScore();
uint32_t    gameBest();
void        gameSetBest(uint32_t b);
bool        gameTitleUpdateHit(int x, int y);           // "UPDATE" button on the title screen

// Shown on screen (set by main.cpp)
extern char gFpsText[16];
extern char gStatusText[48];    // title screen status line (Wi-Fi / update)
extern const char* gVersion;
