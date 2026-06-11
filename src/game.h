#pragma once
// Mini-game pack for StickS3 (portrait 135x240). A tiny picker lets you choose
// between three games; each draws into the global `spr`, reads the IMU via
// compat, and uses the global beep()/lastInteractMs. The main loop calls
// gameTick() once per frame while gameActive and owns the single pushSprite().
//
// Button contract (wired in main.cpp):
//   short A  -> gameButtonA()  : picker = next item; in-game = game action
//   B        -> gameButtonB()  : picker = start; in-game = back to picker
//   long  A  -> gameExit()     : leave games entirely (back to home)
//
// Relies on symbols from main.cpp declared before this include:
//   extern TFT_eSprite spr;  const int W, H;  uint32_t lastInteractMs;
//   void beep(uint16_t, uint16_t);  namespace compat { getAccel(...); }

extern bool gameActive;

// ===========================================================================
// Shared palette / helpers
// ===========================================================================
static const uint16_t COL_BG   = 0x0000; // black
static const uint16_t COL_HUD  = 0xFFFF; // white
static const uint16_t COL_GOAL = 0x07E0; // green
static const uint16_t COL_WALL = 0x2104; // dark
static const uint16_t COL_BALL = 0xFFE0; // amber
static const uint16_t COL_TRAP = 0xF800; // red

struct GameRect { int x, y, w, h; };

static inline uint32_t gRand(uint32_t n) { return (uint32_t)random(n); }

// ---- Picker / dispatcher state --------------------------------------------
enum { GS_PICKER = 0, GS_PLAY = 1 };
static int gScreen = GS_PICKER;
static int gSel    = 0;     // picker cursor
static int gGame   = 0;     // active game index
static const char* GAME_NAMES[] = { "MAZE", "SLOTS", "RACER" };
static const int   GAME_N = 3;

// Forward decls for per-game lifecycles.
static void gMazeInit();  static void gMazeTick();  static void gMazeA();
static void gSlotInit();  static void gSlotTick();  static void gSlotA();
static void gRaceInit();  static void gRaceTick();  static void gRaceA();

// ===========================================================================
// Game 1 — Tilt-ball maze
// ===========================================================================
static const float GAME_K       = 0.85f;  // tilt -> accel gain
static const float GAME_DAMP    = 0.92f;  // velocity damping
static const float GAME_LP      = 0.5f;    // accel low-pass
static const float GAME_R       = 4.0f;    // ball radius
static const float GAME_VMAX    = 5.5f;    // velocity clamp
static const uint32_t GAME_WIN_MS = 1500;  // auto-restart after a win

static const GameRect GAME_WALLS[] = {
  { 0,   42, 100, 8 }, { 35,  84, 100, 8 }, { 0,  126, 100, 8 },
  { 35, 168, 100, 8 }, { 0,  210,  95, 8 },
};
static const int GAME_WALL_N = sizeof(GAME_WALLS) / sizeof(GAME_WALLS[0]);
static const GameRect GAME_TRAPS[] = {
  { 54,  60, 12, 12 }, { 62, 102, 12, 12 }, { 54, 144, 12, 12 }, { 62, 186, 12, 12 },
};
static const int GAME_TRAP_N = sizeof(GAME_TRAPS) / sizeof(GAME_TRAPS[0]);
static const GameRect GAME_GOAL = { 99, 222, 34, 15 };

static float gBx, gBy, gVx, gVy;
static float gSmAx, gSmAy, gSmAz;
static float gAx0, gAy0, gAz0;
static bool  gWon = false;
static uint32_t gWonMs = 0, gStartMs = 0;
static int   gFails = 0;

static inline bool gAabb(float cx, float cy, float r, const GameRect& w) {
  return (cx + r > w.x) && (cx - r < w.x + w.w) &&
         (cy + r > w.y) && (cy - r < w.y + w.h);
}
static void gResolve(const GameRect& w) {
  if (!gAabb(gBx, gBy, GAME_R, w)) return;
  float penL = (gBx + GAME_R) - w.x, penR = (w.x + w.w) - (gBx - GAME_R);
  float penT = (gBy + GAME_R) - w.y, penB = (w.y + w.h) - (gBy - GAME_R);
  float minX = (penL < penR) ? penL : penR, minY = (penT < penB) ? penT : penB;
  if (minX < minY) { gBx += (penL < penR) ? -penL : penR; gVx = 0; }
  else             { gBy += (penT < penB) ? -penT : penB; gVy = 0; }
}
static void gReadAccel(float* dx, float* dy, float* dz) {
  float ax, ay, az; compat::getAccel(&ax, &ay, &az);
  gSmAx = gSmAx * (1 - GAME_LP) + ax * GAME_LP;
  gSmAy = gSmAy * (1 - GAME_LP) + ay * GAME_LP;
  gSmAz = gSmAz * (1 - GAME_LP) + az * GAME_LP;
  *dx = gSmAx - gAx0; *dy = gSmAy - gAy0; *dz = gSmAz - gAz0;
}
static void gMazeRespawn() { gBx = 18; gBy = 16; gVx = gVy = 0; }

static void gMazeInit() {
  gMazeRespawn();
  gWon = false; gWonMs = 0; gFails = 0; gStartMs = millis();
  float ax, ay, az; compat::getAccel(&ax, &ay, &az);
  gSmAx = ax; gSmAy = ay; gSmAz = az;     // seed the low-pass
  gAx0 = 0; gAy0 = 0; gAz0 = 0;           // neutral = TRUE flat, not start pose
}
static void gMazeA() { beep(1800, 40); gMazeInit(); }

static void gMazeTick() {
  if (!gWon) {
    float dx, dy, dz; gReadAccel(&dx, &dy, &dz); (void)dz;
    gVx -= dy * GAME_K; gVy -= dx * GAME_K;
    gVx *= GAME_DAMP;   gVy *= GAME_DAMP;
    if (gVx >  GAME_VMAX) gVx =  GAME_VMAX;  if (gVx < -GAME_VMAX) gVx = -GAME_VMAX;
    if (gVy >  GAME_VMAX) gVy =  GAME_VMAX;  if (gVy < -GAME_VMAX) gVy = -GAME_VMAX;
    gBx += gVx; gBy += gVy;
    if (gBx - GAME_R < 1)    { gBx = 1 + GAME_R;     gVx = 0; }
    if (gBx + GAME_R > W - 2) { gBx = W - 2 - GAME_R; gVx = 0; }
    if (gBy - GAME_R < 1)    { gBy = 1 + GAME_R;     gVy = 0; }
    if (gBy + GAME_R > H - 2) { gBy = H - 2 - GAME_R; gVy = 0; }
    for (int i = 0; i < GAME_WALL_N; i++) gResolve(GAME_WALLS[i]);
    for (int i = 0; i < GAME_TRAP_N; i++) {
      if (gAabb(gBx, gBy, GAME_R, GAME_TRAPS[i])) {
        gFails++; beep(300, 150); gMazeRespawn(); break;
      }
    }
    if (gBx > GAME_GOAL.x && gBx < GAME_GOAL.x + GAME_GOAL.w &&
        gBy > GAME_GOAL.y && gBy < GAME_GOAL.y + GAME_GOAL.h) {
      gWon = true; gWonMs = millis();
      beep(2200, 80); beep(2600, 80); beep(3100, 120);
    }
  } else if (millis() - gWonMs > GAME_WIN_MS) { gMazeInit(); }

  spr.fillSprite(COL_BG);
  spr.drawRect(0, 0, W, H, COL_WALL);
  spr.fillRect(GAME_GOAL.x, GAME_GOAL.y, GAME_GOAL.w, GAME_GOAL.h, COL_GOAL);
  for (int i = 0; i < GAME_WALL_N; i++)
    spr.fillRect(GAME_WALLS[i].x, GAME_WALLS[i].y, GAME_WALLS[i].w, GAME_WALLS[i].h, COL_WALL);
  for (int i = 0; i < GAME_TRAP_N; i++) {
    const GameRect& t = GAME_TRAPS[i];
    spr.fillCircle(t.x + t.w / 2, t.y + t.h / 2, t.w / 2, COL_TRAP);
  }
  spr.fillCircle((int)gBx, (int)gBy, (int)GAME_R, COL_BALL);
  spr.setTextSize(1);
  if (gWon) { spr.setTextColor(COL_GOAL, COL_BG); spr.setCursor(W/2 - 12, 4); spr.print("WIN"); }
  else { spr.setTextColor(COL_HUD, COL_BG); spr.setCursor(3, 3);
         spr.printf("%lus  x%d", (millis() - gStartMs) / 1000, gFails); }
}

// ===========================================================================
// Game 2 — Slot machine (老虎机). A = spin / skill-stop each reel; B = back.
// ===========================================================================
// 6 symbols; "7" is the jackpot. Drawn as a big letter on a colored chip.
static const char     SLOT_SYM[6]  = { '7', '$', 'B', 'C', '*', '#' };
static const uint16_t SLOT_COL[6]  = { 0xF800, 0x07E0, 0xFD20, 0xF81F, 0x07FF, 0xFFFF };
static int      slReel[3];          // current symbol per reel
static bool     slSpin[3];          // reel still spinning?
static int      slCredits;
static int      slWin;              // last payout (for the result flash)
static uint32_t slStep;             // animation timer
static uint32_t slMsgMs;            // result-message timestamp

static void gSlotInit() {
  for (int i = 0; i < 3; i++) { slReel[i] = gRand(6); slSpin[i] = false; }
  slCredits = 10; slWin = 0; slStep = millis(); slMsgMs = 0;
}
static void gSlotEvaluate() {
  int a = slReel[0], b = slReel[1], c = slReel[2];
  if (a == b && b == c) slWin = (a == 0) ? 50 : 20;        // triple (7 = jackpot)
  else if (a == b || b == c || a == c) slWin = 3;          // any pair
  else slWin = 0;
  slCredits += slWin;
  slMsgMs = millis();
  if (slWin >= 50) { beep(2200,80); beep(2600,80); beep(3100,80); beep(3500,160); }
  else if (slWin > 0) { beep(2400, 70); beep(2800, 90); }
  else beep(500, 120);
}
static void gSlotA() {
  bool anySpin = slSpin[0] || slSpin[1] || slSpin[2];
  if (!anySpin) {                       // start a new spin
    if (slCredits <= 0) { beep(400, 120); return; }
    slCredits--; slWin = 0; slMsgMs = 0;
    slSpin[0] = slSpin[1] = slSpin[2] = true;
    slStep = millis();
    beep(1500, 40);
  } else {                              // skill-stop the leftmost spinning reel
    for (int i = 0; i < 3; i++) if (slSpin[i]) { slSpin[i] = false; beep(1900, 35); break; }
    if (!(slSpin[0] || slSpin[1] || slSpin[2])) gSlotEvaluate();
  }
}
static void gSlotTick() {
  if ((slSpin[0] || slSpin[1] || slSpin[2]) && millis() - slStep > 55) {
    slStep = millis();
    for (int i = 0; i < 3; i++) if (slSpin[i]) slReel[i] = (slReel[i] + 1) % 6;
  }
  spr.fillSprite(COL_BG);
  spr.setTextColor(COL_HUD, COL_BG); spr.setTextSize(1);
  spr.setCursor(8, 8);  spr.print("SLOTS");
  spr.setCursor(8, 22); spr.printf("credits %d", slCredits);

  const int bw = 38, bh = 54, gap = 4;
  const int total = bw * 3 + gap * 2, x0 = (W - total) / 2, y0 = 88;
  for (int i = 0; i < 3; i++) {
    int x = x0 + i * (bw + gap);
    spr.fillRoundRect(x, y0, bw, bh, 4, 0x18E3);
    spr.drawRoundRect(x, y0, bw, bh, 4, slSpin[i] ? 0xFFE0 : 0x4208);
    int s = slReel[i];
    spr.setTextColor(SLOT_COL[s], 0x18E3);
    spr.setTextSize(4);
    spr.setCursor(x + bw/2 - 11, y0 + bh/2 - 14);
    spr.print(SLOT_SYM[s]);
  }
  spr.setTextSize(1);
  bool anySpin = slSpin[0] || slSpin[1] || slSpin[2];
  if (slMsgMs && millis() - slMsgMs < 2500 && !anySpin) {
    if (slWin > 0) { spr.setTextColor(COL_GOAL, COL_BG); spr.setCursor(W/2 - 30, 160);
                     spr.printf(slWin >= 50 ? "JACKPOT +%d" : "WIN +%d", slWin); }
    else { spr.setTextColor(0xF800, COL_BG); spr.setCursor(W/2 - 18, 160); spr.print("no win"); }
  }
  spr.setTextColor(0x8410, COL_BG);
  spr.setCursor(6, H - 16);
  spr.print(anySpin ? "A:stop  B:back" : "A:spin  B:back");
}

// ===========================================================================
// Game 3 — Tilt-to-steer racer (赛车超车). Hold upright; tilt L/R to dodge.
// ===========================================================================
static const float RACE_STEER = 4.0f;   // tilt sensitivity (flip sign if mirrored)
static const int   RACE_LX = 14, RACE_RX = 121;   // road edges
static const int   RACE_PW = 20, RACE_PH = 28;    // player car size
static const int   RACE_PY = H - 44;              // player y (fixed)
static const int   RACE_MAXE = 4;
struct RaceCar { float x, y; bool active; };
static float    rPx, rPvx;
static RaceCar  rEnemy[RACE_MAXE];
static int      rScore;
static bool     rOver;
static uint32_t rSpawnMs, rScroll;

static void gRaceInit() {
  rPx = (RACE_LX + RACE_RX - RACE_PW) / 2.0f; rPvx = 0;
  for (int i = 0; i < RACE_MAXE; i++) rEnemy[i].active = false;
  rScore = 0; rOver = false; rSpawnMs = millis(); rScroll = 0;
}
static void gRaceA() { if (rOver) { beep(1800, 40); gRaceInit(); } }

static void gRaceSpawn() {
  for (int i = 0; i < RACE_MAXE; i++) if (!rEnemy[i].active) {
    rEnemy[i].active = true;
    rEnemy[i].x = RACE_LX + gRand(RACE_RX - RACE_LX - RACE_PW);
    rEnemy[i].y = -RACE_PH;
    return;
  }
}
static void gRaceTick() {
  float ax, ay, az; compat::getAccel(&ax, &ay, &az); (void)ax; (void)az;
  if (!rOver) {
    rPvx = rPvx * 0.8f + ay * RACE_STEER;       // tilt -> steer
    rPx += rPvx;
    if (rPx < RACE_LX) { rPx = RACE_LX; rPvx = 0; }
    if (rPx > RACE_RX - RACE_PW) { rPx = RACE_RX - RACE_PW; rPvx = 0; }

    float speed = 3.0f + rScore * 0.15f;        // ramps up
    if (speed > 9) speed = 9;
    rScroll = (rScroll + (uint32_t)speed) % 28;
    uint32_t interval = 900 - rScore * 25; if (interval < 380) interval = 380;
    if (millis() - rSpawnMs > interval) { gRaceSpawn(); rSpawnMs = millis(); }

    for (int i = 0; i < RACE_MAXE; i++) {
      if (!rEnemy[i].active) continue;
      rEnemy[i].y += speed;
      if (rEnemy[i].y > H) { rEnemy[i].active = false; rScore++; beep(2600, 25); continue; }
      // collision (AABB)
      if (rPx < rEnemy[i].x + RACE_PW && rPx + RACE_PW > rEnemy[i].x &&
          RACE_PY < rEnemy[i].y + RACE_PH && RACE_PY + RACE_PH > rEnemy[i].y) {
        rOver = true; beep(500, 100); beep(350, 200);
      }
    }
  }

  spr.fillSprite(0x10A2);                         // grass-ish dark
  spr.fillRect(RACE_LX - 2, 0, RACE_RX - RACE_LX + 4, H, 0x2104);  // road
  // dashed center line scrolling for a sense of speed
  for (int y = (int)rScroll - 28; y < H; y += 28)
    spr.fillRect(W/2 - 2, y, 4, 14, 0xCE59);
  // enemies (red cars)
  for (int i = 0; i < RACE_MAXE; i++) if (rEnemy[i].active)
    spr.fillRoundRect((int)rEnemy[i].x, (int)rEnemy[i].y, RACE_PW, RACE_PH, 3, 0xF800);
  // player (yellow car)
  spr.fillRoundRect((int)rPx, RACE_PY, RACE_PW, RACE_PH, 3, 0xFFE0);

  spr.setTextSize(1); spr.setTextColor(COL_HUD, 0x2104);
  spr.setCursor(4, 4); spr.printf("score %d", rScore);
  if (rOver) {
    spr.setTextColor(0xF800, 0x2104);
    spr.setCursor(W/2 - 18, H/2 - 16); spr.print("CRASH");
    spr.setTextColor(COL_HUD, 0x2104);
    spr.setCursor(W/2 - 30, H/2); spr.printf("score %d", rScore);
    spr.setTextColor(0x8410, 0x2104);
    spr.setCursor(W/2 - 33, H/2 + 16); spr.print("A:retry B:back");
  }
}

// ===========================================================================
// Picker + dispatcher
// ===========================================================================
static void gDrawPicker() {
  spr.fillSprite(COL_BG);
  spr.setTextColor(COL_HUD, COL_BG); spr.setTextSize(2);
  spr.setCursor(18, 26); spr.print("GAMES");
  spr.setTextSize(2);
  for (int i = 0; i < GAME_N; i++) {
    bool sel = (i == gSel);
    int y = 80 + i * 34;
    if (sel) { spr.fillRoundRect(10, y - 4, W - 20, 28, 4, 0x2945);
               spr.setTextColor(0xFFE0, 0x2945); }
    else spr.setTextColor(0x8410, COL_BG);
    spr.setCursor(24, y); spr.print(sel ? "> " : "  "); spr.print(GAME_NAMES[i]);
  }
  spr.setTextSize(1); spr.setTextColor(0x8410, COL_BG);
  spr.setCursor(10, H - 28); spr.print("A:next  B:play");
  spr.setCursor(10, H - 16); spr.print("hold A: exit");
}

void gameInit() {
  gScreen = GS_PICKER; gSel = 0;
  randomSeed(micros() ^ (uint32_t)esp_random());
}
void gameTick() {
  lastInteractMs = millis();
  if (gScreen == GS_PICKER) { gDrawPicker(); return; }
  switch (gGame) {
    case 0: gMazeTick(); break;
    case 1: gSlotTick(); break;
    case 2: gRaceTick(); break;
  }
}
void gameButtonA() {              // short A
  if (gScreen == GS_PICKER) { gSel = (gSel + 1) % GAME_N; beep(1800, 30); return; }
  switch (gGame) { case 0: gMazeA(); break; case 1: gSlotA(); break; case 2: gRaceA(); break; }
}
void gameButtonB() {             // B
  if (gScreen == GS_PICKER) {
    gGame = gSel; gScreen = GS_PLAY; beep(2400, 40);
    switch (gGame) { case 0: gMazeInit(); break; case 1: gSlotInit(); break; case 2: gRaceInit(); break; }
  } else {
    gScreen = GS_PICKER; beep(1200, 40);    // back to the picker
  }
}
void gameExit() { gameActive = false; beep(900, 60); }
