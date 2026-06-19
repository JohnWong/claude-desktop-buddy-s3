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
static const char* GAME_NAMES[] = { "MAZE", "SLOTS", "RACER", "REACT", "TETRIS" };
static const int   GAME_N = 5;

// Forward decls for per-game lifecycles.
static void gMazeInit();  static void gMazeTick();  static void gMazeA();
static void gSlotInit();  static void gSlotTick();  static void gSlotA();
static void gRaceInit();  static void gRaceTick();  static void gRaceA();
static void gReactInit(); static void gReactTick(); static void gReactA();
static void gTetrisInit(); static void gTetrisTick(); static void gTetrisA();

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
// 6 graphic symbols; index 0 (SEVEN) is the jackpot.
//   0 SEVEN  1 CHERRY  2 BELL  3 STAR  4 DIAMOND  5 BAR
// Each reel is a strip of repeating symbols scrolling downward. slPos is the
// vertical scroll offset in pixels; the symbol "in the window" is the strip
// cell at floor(slPos/CELL). Reels spin at full speed, then decelerate and
// snap to a cell boundary when stopped (skill-stop with A, or auto after a
// staggered timeout).
static const float    SLOT_CELL  = 84.0f;  // px per symbol on the strip = window height
static const float    SLOT_SPIN  = 24.0f;  // full-speed scroll (px/frame)
static const float    SLOT_DECEL = 1.4f;   // deceleration while stopping
static const float    SLOT_MINV  = 7.0f;   // snap-to-cell below this speed
static float    slPos[3];           // scroll offset per reel (px, grows downward)
static float    slVel[3];           // current scroll speed per reel
static uint8_t  slSt[3];            // 0 idle/aligned, 1 spinning, 2 stopping
static uint32_t slSpinMs;           // when this spin started (for auto-stop)
static int      slCredits;
static int      slWin;              // last payout (for the result flash)
static uint32_t slMsgMs;            // result-message timestamp
static int      slTheme = -1;       // symbol skin (-1 = not loaded), persisted
static const int SLOT_THEME_N = 5;  // CASINO / FRUIT / SUITS / FACES / RIDES

static int gSlotLoadTheme() {
  Preferences p; p.begin("buddy", true);
  int t = p.getInt("sltheme", 0); p.end();
  return (t >= 0 && t < SLOT_THEME_N) ? t : 0;
}
static void gSlotSaveTheme() {
  Preferences p; p.begin("buddy", false);
  p.putInt("sltheme", slTheme); p.end();
}

static inline int slReelVal(int i) {
  return ((int)floorf(slPos[i] / SLOT_CELL)) % 6;
}
static void gSlotInit() {
  for (int i = 0; i < 3; i++) { slPos[i] = gRand(6) * SLOT_CELL; slVel[i] = 0; slSt[i] = 0; }
  slCredits = 10; slWin = 0; slMsgMs = 0; slSpinMs = 0;
  if (slTheme < 0) slTheme = gSlotLoadTheme();
}
static void gSlotEvaluate() {
  int a = slReelVal(0), b = slReelVal(1), c = slReelVal(2);
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
  bool anyActive = slSt[0] || slSt[1] || slSt[2];
  if (!anyActive) {                     // start a new spin
    if (slCredits <= 0) { beep(400, 120); return; }
    slCredits--; slWin = 0; slMsgMs = 0;
    for (int i = 0; i < 3; i++) { slSt[i] = 1; slVel[i] = SLOT_SPIN; }
    slSpinMs = millis();
    beep(1500, 40);
  } else {                              // skill-stop the leftmost still-spinning reel
    for (int i = 0; i < 3; i++) if (slSt[i] == 1) { slSt[i] = 2; beep(1900, 35); break; }
  }
}
// --- Theme 0: CASINO (7 / cherry / bell / star / diamond / bar) -------------
static void gSym0(int cx, int cy, int idx) {
  switch (idx) {
    case 0: { // SEVEN (red) — top bar + slanted leg
      uint16_t C = 0xF800;
      spr.fillRect(cx - 14, cy - 18, 28, 7, C);
      spr.fillTriangle(cx + 14, cy - 11, cx + 4, cy - 11, cx - 7, cy + 18, C);
      spr.fillTriangle(cx + 14, cy - 11, cx - 7, cy + 18, cx + 3, cy + 18, C);
    } break;
    case 1: { // CHERRY (red on green stems)
      spr.drawLine(cx - 9, cy + 6, cx + 1, cy - 15, 0x07E0);
      spr.drawLine(cx + 9, cy + 4, cx + 1, cy - 15, 0x07E0);
      spr.fillCircle(cx - 9, cy + 8, 8, 0xF800);
      spr.fillCircle(cx + 9, cy + 6, 8, 0xF800);
      spr.fillCircle(cx - 11, cy + 5, 2, 0xFFFF);
    } break;
    case 2: { // BELL (gold)
      uint16_t C = 0xFD20;
      spr.fillCircle(cx, cy - 15, 3, C);
      spr.fillTriangle(cx, cy - 14, cx - 15, cy + 10, cx + 15, cy + 10, C);
      spr.fillRect(cx - 16, cy + 9, 32, 5, C);
      spr.fillCircle(cx, cy + 16, 3, C);
    } break;
    case 3: { // STAR (yellow) — two overlapping triangles
      uint16_t C = 0xFFE0;
      spr.fillTriangle(cx, cy - 18, cx - 16, cy + 9, cx + 16, cy + 9, C);
      spr.fillTriangle(cx, cy + 18, cx - 16, cy - 9, cx + 16, cy - 9, C);
    } break;
    case 4: { // DIAMOND (cyan)
      uint16_t C = 0x07FF;
      spr.fillTriangle(cx, cy - 18, cx - 15, cy, cx + 15, cy, C);
      spr.fillTriangle(cx, cy + 18, cx - 15, cy, cx + 15, cy, C);
      spr.drawLine(cx - 7, cy - 9, cx + 6, cy - 9, 0xFFFF);
    } break;
    default: { // 5 BAR (green) — three stacked bars
      uint16_t C = 0x07E0;
      spr.fillRect(cx - 16, cy - 17, 32, 8, C);
      spr.fillRect(cx - 16, cy - 4,  32, 8, C);
      spr.fillRect(cx - 16, cy + 9,  32, 8, C);
    } break;
  }
}

// --- Theme 1: FRUIT (watermelon / cherry / lemon / orange / grape / berry) --
static void gSym1(int cx, int cy, int idx) {
  switch (idx) {
    case 0: { // WATERMELON slice (jackpot)
      spr.fillTriangle(cx, cy - 16, cx - 17, cy + 13, cx + 17, cy + 13, 0x07E0);
      spr.fillTriangle(cx, cy - 11, cx - 13, cy + 11, cx + 13, cy + 11, 0xF800);
      spr.fillCircle(cx - 5, cy + 3, 1, 0x0000); spr.fillCircle(cx + 5, cy + 3, 1, 0x0000);
      spr.fillCircle(cx, cy + 7, 1, 0x0000);
    } break;
    case 1: { // CHERRY
      spr.drawLine(cx - 9, cy + 6, cx + 1, cy - 15, 0x07E0);
      spr.drawLine(cx + 9, cy + 4, cx + 1, cy - 15, 0x07E0);
      spr.fillCircle(cx - 9, cy + 8, 8, 0xF800);
      spr.fillCircle(cx + 9, cy + 6, 8, 0xF800);
      spr.fillCircle(cx - 11, cy + 5, 2, 0xFFFF);
    } break;
    case 2: { // LEMON
      spr.fillEllipse(cx, cy, 16, 11, 0xFFE0);
      spr.fillTriangle(cx - 16, cy, cx - 20, cy - 2, cx - 20, cy + 2, 0x9E60);
      spr.fillTriangle(cx + 16, cy, cx + 20, cy - 2, cx + 20, cy + 2, 0x9E60);
    } break;
    case 3: { // ORANGE
      spr.fillCircle(cx, cy, 15, 0xFD20);
      for (int a = 0; a < 6; a++) {
        float t = a * 1.0472f;   // 60° steps
        spr.drawLine(cx, cy, cx + (int)(13 * cosf(t)), cy + (int)(13 * sinf(t)), 0xC300);
      }
    } break;
    case 4: { // GRAPE cluster
      const int gx[6] = {0, -7, 7, -4, 4, 0}, gy[6] = {-10, -2, -2, 6, 6, 13};
      for (int k = 0; k < 6; k++) spr.fillCircle(cx + gx[k], cy + gy[k], 5, 0x901F);
      spr.fillRect(cx - 1, cy - 17, 2, 5, 0x07E0);
    } break;
    default: { // 5 STRAWBERRY
      spr.fillTriangle(cx, cy + 16, cx - 13, cy - 5, cx + 13, cy - 5, 0xF800);
      spr.fillTriangle(cx, cy - 13, cx - 11, cy - 3, cx + 11, cy - 3, 0x07E0);
      spr.fillCircle(cx - 5, cy + 2, 1, 0xFFE0); spr.fillCircle(cx + 5, cy + 2, 1, 0xFFE0);
      spr.fillCircle(cx, cy + 8, 1, 0xFFE0);
    } break;
  }
}

// --- Theme 2: SUITS (heart / spade / diamond / club / star / 7) -------------
static void gSym2(int cx, int cy, int idx) {
  switch (idx) {
    case 0: { // HEART (jackpot)
      spr.fillCircle(cx - 7, cy - 5, 8, 0xF800);
      spr.fillCircle(cx + 7, cy - 5, 8, 0xF800);
      spr.fillTriangle(cx - 14, cy - 1, cx + 14, cy - 1, cx, cy + 16, 0xF800);
    } break;
    case 1: { // SPADE (white)
      spr.fillCircle(cx - 7, cy + 3, 8, 0xFFFF);
      spr.fillCircle(cx + 7, cy + 3, 8, 0xFFFF);
      spr.fillTriangle(cx - 14, cy + 7, cx + 14, cy + 7, cx, cy - 16, 0xFFFF);
      spr.fillTriangle(cx - 6, cy + 17, cx + 6, cy + 17, cx, cy + 4, 0xFFFF);
    } break;
    case 2: { // DIAMOND (red)
      spr.fillTriangle(cx, cy - 17, cx - 13, cy, cx + 13, cy, 0xF800);
      spr.fillTriangle(cx, cy + 17, cx - 13, cy, cx + 13, cy, 0xF800);
    } break;
    case 3: { // CLUB (white)
      spr.fillCircle(cx, cy - 7, 7, 0xFFFF);
      spr.fillCircle(cx - 8, cy + 3, 7, 0xFFFF);
      spr.fillCircle(cx + 8, cy + 3, 7, 0xFFFF);
      spr.fillTriangle(cx - 6, cy + 17, cx + 6, cy + 17, cx, cy + 2, 0xFFFF);
    } break;
    case 4: { // STAR (yellow)
      spr.fillTriangle(cx, cy - 18, cx - 16, cy + 9, cx + 16, cy + 9, 0xFFE0);
      spr.fillTriangle(cx, cy + 18, cx - 16, cy - 9, cx + 16, cy - 9, 0xFFE0);
    } break;
    default: { // 5 SEVEN (red)
      spr.fillRect(cx - 14, cy - 18, 28, 7, 0xF800);
      spr.fillTriangle(cx + 14, cy - 11, cx + 4, cy - 11, cx - 7, cy + 18, 0xF800);
      spr.fillTriangle(cx + 14, cy - 11, cx - 7, cy + 18, cx + 3, cy + 18, 0xF800);
    } break;
  }
}

// --- Theme 3: FACES (smile / grin / cry / cool / love / angry) --------------
static void gSym3(int cx, int cy, int idx) {
  const uint16_t F = 0xFFE0, K = 0x0000;
  spr.fillCircle(cx, cy, 17, F);
  spr.drawCircle(cx, cy, 17, 0xC600);
  switch (idx) {
    case 0: // SMILE (jackpot)
      spr.fillCircle(cx - 6, cy - 4, 2, K); spr.fillCircle(cx + 6, cy - 4, 2, K);
      spr.fillArc(cx, cy + 1, 6, 9, 20, 160, K);
      break;
    case 1: // GRIN — wide open smile
      spr.fillCircle(cx - 6, cy - 5, 2, K); spr.fillCircle(cx + 6, cy - 5, 2, K);
      spr.fillArc(cx, cy + 1, 2, 10, 10, 170, K);
      break;
    case 2: // CRY
      spr.fillCircle(cx - 6, cy - 4, 2, K); spr.fillCircle(cx + 6, cy - 4, 2, K);
      spr.fillArc(cx, cy + 13, 6, 9, 200, 340, K);   // frown
      spr.fillCircle(cx - 6, cy + 3, 2, 0x051F);      // tear
      break;
    case 3: // COOL — sunglasses
      spr.fillRoundRect(cx - 13, cy - 8, 10, 7, 2, K);
      spr.fillRoundRect(cx + 3,  cy - 8, 10, 7, 2, K);
      spr.fillRect(cx - 3, cy - 6, 6, 2, K);          // bridge
      spr.fillArc(cx, cy + 2, 6, 9, 20, 160, K);
      break;
    case 4: // LOVE — heart eyes
      for (int s = -1; s <= 1; s += 2) {
        int ex = cx + s * 6;
        spr.fillCircle(ex - 2, cy - 5, 2, 0xF800); spr.fillCircle(ex + 2, cy - 5, 2, 0xF800);
        spr.fillTriangle(ex - 4, cy - 4, ex + 4, cy - 4, ex, cy, 0xF800);
      }
      spr.fillArc(cx, cy + 1, 6, 9, 20, 160, K);
      break;
    default: // ANGRY
      spr.fillCircle(cx - 6, cy - 2, 2, K); spr.fillCircle(cx + 6, cy - 2, 2, K);
      spr.drawLine(cx - 11, cy - 9, cx - 3, cy - 5, K);   // brows
      spr.drawLine(cx + 11, cy - 9, cx + 3, cy - 5, K);
      spr.fillArc(cx, cy + 14, 6, 9, 200, 340, K);        // frown
      break;
  }
}

// --- Theme 4: RIDES (car / bus / plane / rocket / ship / train) -------------
static void gSym4(int cx, int cy, int idx) {
  switch (idx) {
    case 0: { // CAR (jackpot)
      spr.fillRoundRect(cx - 16, cy - 2, 32, 10, 2, 0xF800);
      spr.fillRoundRect(cx - 9, cy - 10, 18, 9, 2, 0xF800);
      spr.fillRect(cx - 7, cy - 8, 6, 5, 0x2D7F);
      spr.fillRect(cx + 1, cy - 8, 6, 5, 0x2D7F);
      spr.fillCircle(cx - 9, cy + 8, 3, 0x0000); spr.fillCircle(cx + 9, cy + 8, 3, 0x0000);
    } break;
    case 1: { // BUS
      spr.fillRoundRect(cx - 17, cy - 10, 34, 18, 3, 0xFD20);
      for (int wx = -13; wx <= 8; wx += 7) spr.fillRect(cx + wx, cy - 6, 5, 5, 0x2D7F);
      spr.fillRect(cx - 15, cy + 2, 30, 2, 0x0000);
      spr.fillCircle(cx - 10, cy + 8, 3, 0x0000); spr.fillCircle(cx + 10, cy + 8, 3, 0x0000);
    } break;
    case 2: { // PLANE
      uint16_t C = 0xFFFF;
      spr.fillRoundRect(cx - 4, cy - 16, 8, 32, 4, C);
      spr.fillTriangle(cx - 4, cy - 2, cx - 17, cy + 6, cx - 4, cy + 6, C);
      spr.fillTriangle(cx + 4, cy - 2, cx + 17, cy + 6, cx + 4, cy + 6, C);
      spr.fillTriangle(cx - 7, cy + 16, cx + 7, cy + 16, cx, cy + 10, C);
      spr.fillCircle(cx, cy - 12, 2, 0x2D7F);
    } break;
    case 3: { // ROCKET
      spr.fillTriangle(cx, cy - 18, cx - 7, cy - 4, cx + 7, cy - 4, 0xF800);
      spr.fillRoundRect(cx - 7, cy - 4, 14, 16, 3, 0xFFFF);
      spr.fillCircle(cx, cy + 2, 3, 0x2D7F);
      spr.fillTriangle(cx - 7, cy + 6, cx - 14, cy + 14, cx - 7, cy + 12, 0xF800);
      spr.fillTriangle(cx + 7, cy + 6, cx + 14, cy + 14, cx + 7, cy + 12, 0xF800);
      spr.fillTriangle(cx - 4, cy + 12, cx + 4, cy + 12, cx, cy + 19, 0xFD20);
    } break;
    case 4: { // SHIP
      spr.fillTriangle(cx - 17, cy + 4, cx + 17, cy + 4, cx + 11, cy + 13, 0xF800);
      spr.fillTriangle(cx - 17, cy + 4, cx + 11, cy + 13, cx - 11, cy + 13, 0xF800);
      spr.fillRect(cx - 7, cy - 6, 14, 10, 0xFFFF);
      spr.fillRect(cx - 2, cy - 13, 5, 7, 0xFD20);
      spr.drawLine(cx - 17, cy + 15, cx + 17, cy + 15, 0x051F);
    } break;
    default: { // TRAIN
      spr.fillRoundRect(cx - 14, cy - 12, 26, 24, 3, 0x07E0);
      spr.fillRect(cx - 10, cy - 8, 12, 8, 0x2D7F);
      spr.fillRect(cx - 6, cy - 18, 6, 6, 0x4208);
      spr.fillRect(cx + 12, cy - 2, 4, 12, 0x07E0);
      spr.fillCircle(cx - 8, cy + 12, 3, 0x0000); spr.fillCircle(cx + 4, cy + 12, 3, 0x0000);
    } break;
  }
}

static const char* SLOT_THEME_NAME[] = { "CASINO", "FRUIT", "SUITS", "FACES", "RIDES" };
static void gSlotSym(int cx, int cy, int idx, int theme) {
  switch (theme) {
    case 1: gSym1(cx, cy, idx); break;
    case 2: gSym2(cx, cy, idx); break;
    case 3: gSym3(cx, cy, idx); break;
    case 4: gSym4(cx, cy, idx); break;
    default: gSym0(cx, cy, idx); break;
  }
}

static void gSlotTick() {
  bool wasActive = slSt[0] || slSt[1] || slSt[2];
  // Advance each reel.
  for (int i = 0; i < 3; i++) {
    if (slSt[i] == 1) {               // full-speed spin
      slPos[i] += SLOT_SPIN;
      // staggered auto-stop if the player doesn't skill-stop
      if (millis() - slSpinMs > (uint32_t)(1700 + i * 700)) slSt[i] = 2;
    } else if (slSt[i] == 2) {        // decelerating to a stop
      slPos[i] += slVel[i] > SLOT_MINV ? slVel[i] : SLOT_MINV;
      slVel[i] -= SLOT_DECEL;
      if (slVel[i] <= SLOT_MINV) {    // snap to the nearest cell boundary
        slPos[i] = roundf(slPos[i] / SLOT_CELL) * SLOT_CELL;
        slVel[i] = 0; slSt[i] = 0; beep(1700, 30);
      }
    }
  }
  bool nowActive = slSt[0] || slSt[1] || slSt[2];
  if (wasActive && !nowActive) gSlotEvaluate();   // all reels just settled

  // Landscape, full-screen: draw into the sprite rotated 90° (logical 240x135),
  // then restore rotation so the main loop's pushSprite presents it as-is.
  spr.setRotation(1);
  const int LW = 240, LH = 135;
  spr.fillSprite(0x0008);                          // deep blue felt
  spr.setTextSize(1); spr.setTextColor(0xFFE0, 0x0008);
  spr.setCursor(10, 8);  spr.print("S L O T S");
  spr.setTextColor(COL_HUD, 0x0008);
  spr.setCursor(LW - 78, 8); spr.printf("credits %d", slCredits);

  const int bw = 60, bh = (int)SLOT_CELL, gap = 12;
  const int total = bw * 3 + gap * 2, x0 = (LW - total) / 2, y0 = 26;
  for (int i = 0; i < 3; i++) {
    int x = x0 + i * (bw + gap);
    bool act = slSt[i] != 0;
    spr.fillRoundRect(x, y0, bw, bh, 6, 0x18E3);
    // clip the scrolling strip to this reel window
    spr.setClipRect(x + 1, y0 + 1, bw - 2, bh - 2);
    int base = (int)floorf(slPos[i] / SLOT_CELL);
    float shift = slPos[i] - base * SLOT_CELL;     // 0..CELL, grows downward
    for (int k = -1; k <= 1; k++) {                // symbols covering the window
      int idx = ((base - k) % 6 + 6) % 6;
      int cy = y0 + bh / 2 + (int)shift + k * (int)SLOT_CELL;
      gSlotSym(x + bw / 2, cy, idx, slTheme);
    }
    spr.clearClipRect();
    // window frame + center payline + spin highlight
    spr.drawRoundRect(x, y0, bw, bh, 6, act ? 0xFFE0 : 0x4208);
    if (act) spr.drawRoundRect(x + 1, y0 + 1, bw - 2, bh - 2, 6, 0xFFE0);
    spr.drawFastHLine(x + 4, y0 + bh / 2, bw - 8, 0x630C);
  }

  bool anySpin = nowActive;
  if (slMsgMs && millis() - slMsgMs < 2500 && !anySpin) {
    spr.setTextSize(2);
    if (slWin > 0) {
      spr.setTextColor(slWin >= 50 ? 0xFFE0 : COL_GOAL, 0x0008);
      char buf[20]; snprintf(buf, sizeof(buf), slWin >= 50 ? "JACKPOT +%d" : "WIN +%d", slWin);
      spr.setCursor(LW / 2 - (int)strlen(buf) * 6, LH - 26); spr.print(buf);
    } else {
      spr.setTextColor(0xF800, 0x0008);
      spr.setCursor(LW / 2 - 36, LH - 26); spr.print("NO WIN");
    }
  } else {
    spr.setTextSize(1); spr.setTextColor(0x8410, 0x0008);
    spr.setCursor(10, LH - 14);
    spr.print(anySpin ? "A: stop reel   B: back   hold A: skin"
                      : "A: spin   B: back   hold A: skin");
  }
  // current skin name, top-right under credits
  spr.setTextSize(1); spr.setTextColor(0x07FF, 0x0008);
  spr.setCursor(LW - 78, 18); spr.printf("skin: %s", SLOT_THEME_NAME[slTheme]);
  spr.setRotation(0);   // restore for the rest of the system
}

// ===========================================================================
// Game 3 — Tilt-to-steer racer (赛车超车). Hold upright; tilt L/R to dodge.
// ===========================================================================
static const float RACE_STEER = 6.0f;   // tilt sensitivity; steady-state max px/frame = 5*RACE_STEER*ay
static const int   RACE_LX = 14, RACE_RX = 121;   // road edges
static const int   RACE_PW = 20, RACE_PH = 28;    // player car size
static const int   RACE_PY = H - 44;              // player y (fixed)
static const int   RACE_MAXE = 4;
// Enemy paint jobs: body color + deco style (0 plain, 1 racing stripes,
// 2 contrast roof). Picked at spawn so the road has a mix of cars.
static const uint16_t RACE_PAL[] = {
  0xF800, 0x051F, 0x780F, 0xFC00, 0x07FF, 0xFFFF, 0xA145,
};
static const int RACE_PAL_N = sizeof(RACE_PAL) / sizeof(RACE_PAL[0]);
struct RaceCar { float x, y; bool active; uint16_t color; uint8_t deco; };
static float    rPx, rPvx;
static RaceCar  rEnemy[RACE_MAXE];
static int      rScore;
static int      rHigh = -1;   // best score, persisted in NVS (-1 = not loaded)
static bool     rOver;
static uint32_t rSpawnMs, rScroll;

static int gRaceLoadHigh() {
  Preferences p; p.begin("buddy", true);
  int h = p.getInt("racehi", 0); p.end(); return h;
}
static void gRaceSaveHigh(int h) {
  Preferences p; p.begin("buddy", false);
  p.putInt("racehi", h); p.end();
}

static void gRaceInit() {
  rPx = (RACE_LX + RACE_RX - RACE_PW) / 2.0f; rPvx = 0;
  for (int i = 0; i < RACE_MAXE; i++) rEnemy[i].active = false;
  rScore = 0; rOver = false; rSpawnMs = millis(); rScroll = 0;
  if (rHigh < 0) rHigh = gRaceLoadHigh();    // load once per session
}
static void gRaceA() { if (rOver) { beep(1800, 40); gRaceInit(); } }

static void gRaceSpawn() {
  for (int i = 0; i < RACE_MAXE; i++) if (!rEnemy[i].active) {
    rEnemy[i].active = true;
    rEnemy[i].x = RACE_LX + gRand(RACE_RX - RACE_LX - RACE_PW);
    rEnemy[i].y = -RACE_PH;
    rEnemy[i].color = RACE_PAL[gRand(RACE_PAL_N)];
    rEnemy[i].deco  = (uint8_t)gRand(3);
    return;
  }
}
// A little top-down car: body + cabin glass + four black wheels poking out.
// `down` = facing down (enemies); else facing up (player) — moves the cabin/
// windshield to the leading end.
static void gRaceCar(int x, int y, uint16_t body, bool down, uint8_t deco) {
  const int w = RACE_PW, h = RACE_PH;
  // wheels (front + rear pairs)
  spr.fillRect(x - 2,     y + h / 6,     3, h / 4, 0x0000);
  spr.fillRect(x + w - 1, y + h / 6,     3, h / 4, 0x0000);
  spr.fillRect(x - 2,     y + h - h/6 - h/4, 3, h / 4, 0x0000);
  spr.fillRect(x + w - 1, y + h - h/6 - h/4, 3, h / 4, 0x0000);
  // body
  spr.fillRoundRect(x, y, w, h, 4, body);
  spr.drawRoundRect(x, y, w, h, 4, 0x0000);
  // deco 1: two racing stripes running down the hood
  if (deco == 1) {
    spr.fillRect(x + w / 2 - 4, y + 2, 3, h - 4, 0xFFFF);
    spr.fillRect(x + w / 2 + 1, y + 2, 3, h - 4, 0xFFFF);
  }
  // cabin glass toward the travel (leading) end
  int cy = down ? (y + h - 11) : (y + 3);
  spr.fillRoundRect(x + 3, cy, w - 6, 8, 2, 0x2D7F);   // bluish windshield
  // roof slit behind the cabin — deco 2 paints it a contrasting white
  spr.fillRect(x + 4, down ? cy - 5 : cy + 9, w - 8, 3, deco == 2 ? 0xFFFF : 0x18C3);
}

static void gRaceTick() {
  float ax, ay, az; compat::getAccel(&ax, &ay, &az); (void)ax; (void)az;
  if (!rOver) {
    rPvx = rPvx * 0.8f - ay * RACE_STEER;       // tilt -> steer (toward the low side)
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
        if (rScore > rHigh) { rHigh = rScore; gRaceSaveHigh(rHigh); }
      }
    }
  }

  spr.fillSprite(0x10A2);                         // grass-ish dark
  spr.fillRect(RACE_LX - 2, 0, RACE_RX - RACE_LX + 4, H, 0x2104);  // road
  // dashed center line scrolling for a sense of speed
  for (int y = (int)rScroll - 28; y < H; y += 28)
    spr.fillRect(W/2 - 2, y, 4, 14, 0xCE59);
  // enemies (assorted paint jobs, facing down toward you)
  for (int i = 0; i < RACE_MAXE; i++) if (rEnemy[i].active)
    gRaceCar((int)rEnemy[i].x, (int)rEnemy[i].y, rEnemy[i].color, true, rEnemy[i].deco);
  // player (yellow car with racing stripes, facing up)
  gRaceCar((int)rPx, RACE_PY, 0xFFE0, false, 1);

  spr.setTextSize(1);
  spr.setTextColor(COL_HUD, 0x2104);
  spr.setCursor(4, 4);  spr.printf("score %d", rScore);
  spr.setTextColor(0xFFE0, 0x2104);
  spr.setCursor(4, 15); spr.printf("best  %d", rHigh < 0 ? 0 : rHigh);
  if (rOver) {
    spr.setTextColor(0xF800, 0x2104);
    spr.setCursor(W/2 - 18, H/2 - 24); spr.print("CRASH");
    spr.setTextColor(COL_HUD, 0x2104);
    spr.setCursor(W/2 - 30, H/2 - 6); spr.printf("score %d", rScore);
    bool isNew = (rScore >= rHigh && rScore > 0);
    spr.setTextColor(0xFFE0, 0x2104);
    spr.setCursor(W/2 - 30, H/2 + 8); spr.printf(isNew ? "NEW BEST!" : "best %d", rHigh);
    spr.setTextColor(0x8410, 0x2104);
    spr.setCursor(W/2 - 39, H/2 + 26); spr.print("A:retry  B:back");
  }
}

// ===========================================================================
// Game 4 — Reaction race (起步反应竞速). F1-style start lights: three reds come
// on one by one, hold a random beat, then lights-out + GREEN = GO. Slam A as
// fast as you can; the reaction time (ms) shows and the best is kept in NVS.
// Pressing before GO is a JUMP START. Drives the physical PbHub traffic lights
// in lockstep when present (red gantry building, then green on GO).
// ===========================================================================
static const uint32_t RX_STEP = 650;   // ms between each red coming on
static uint8_t  rxPhase;     // 0 ready, 1 reds-building, 2 armed-hold, 3 GO/timing, 4 result, 5 jump
static uint32_t rxT0;        // current phase start (ms)
static uint32_t rxGoMs;      // when GO fired
static uint32_t rxHold;      // random armed-hold duration
static int      rxLit;       // reds currently on (0..3)
static int      rxMs;        // last reaction time (ms)
static int      rxBest = -1; // best (lowest) reaction; -1 unloaded, 0 = none yet
static bool     rxNewBest;

static int gReactLoadBest() {
  Preferences p; p.begin("buddy", true);
  int b = p.getInt("rxbest", 0); p.end(); return b;
}
static void gReactSaveBest(int b) {
  Preferences p; p.begin("buddy", false);
  p.putInt("rxbest", b); p.end();
}

// Set physical module m: 0 off, 1 red, 2 green (no-op without a PbHub).
static inline void rxLight(int m, uint8_t which) {
  if (!tlPresent) return;
  tlSet(m * 3 + 0, which == 1);
  tlSet(m * 3 + 1, false);
  tlSet(m * 3 + 2, which == 2);
}

static void gReactArm() {              // begin a fresh start-light sequence
  rxPhase = 1; rxLit = 0; rxT0 = millis(); rxNewBest = false;
  if (tlPresent) tlAllOff();
  beep(1400, 50);
}
static bool rxPressHandled = false;     // swallow the release paired with a handled press

static void gReactInit() {
  rxPhase = 0; rxLit = 0; rxMs = 0; rxNewBest = false; rxPressHandled = false;
  if (rxBest < 0) rxBest = gReactLoadBest();
  if (tlPresent) tlAllOff();
}
// Time-critical: fires on the A DOWN edge (no press-hold/release latency).
static void gReactDown() {
  if (rxPhase == 3) {                   // reacted!
    rxMs = (int)(millis() - rxGoMs);
    rxPhase = 4;
    if (rxBest <= 0 || rxMs < rxBest) { rxBest = rxMs; gReactSaveBest(rxBest); rxNewBest = true; }
    beep(2600, 60); beep(3100, 90);
    rxPressHandled = true;
  } else if (rxPhase == 1 || rxPhase == 2) {   // pressed before GO — jump start
    rxPhase = 5;
    for (int m = 0; m < 3; m++) rxLight(m, 1);  // all red = penalty
    // "啊—哦" descending fail sfx. Kept in the 500–1000Hz band: the tiny
    // speaker barely reproduces sub-400Hz, so a low droop would be inaudible.
    beep(950, 110); delay(95);
    beep(760, 120); delay(105);
    beep(560, 360); delay(330);
    rxPressHandled = true;
  }
}
// Non-critical (start / play again): fine on the release edge. The release that
// pairs with a down-edge we already handled is swallowed.
static void gReactA() {
  if (rxPressHandled) { rxPressHandled = false; return; }
  gReactArm();   // phase 0 = start, phase 4/5 = go again
}
static void gReactTick() {
  uint32_t now = millis();
  if (rxPhase == 1) {                   // reds come on one at a time
    int want = (int)((now - rxT0) / RX_STEP) + 1; if (want > 3) want = 3;
    while (rxLit < want) { rxLight(rxLit, 1); beep(1200, 60); rxLit++; }
    if (rxLit >= 3 && now - rxT0 >= 3 * RX_STEP) {
      rxPhase = 2; rxT0 = now; rxHold = 700 + gRand(2200);   // hold 0.7–2.9s
    }
  } else if (rxPhase == 2) {            // armed: wait the random beat, then GO
    if (now - rxT0 >= rxHold) {
      for (int m = 0; m < 3; m++) rxLight(m, 2);             // lights out + GREEN
      rxGoMs = now; rxPhase = 3; beep(3200, 120);
    }
  }

  // ---- render (portrait) ----
  spr.fillSprite(COL_BG);
  spr.setTextSize(2); spr.setTextColor(COL_HUD, COL_BG);
  spr.setCursor(10, 14); spr.print("REACTION");

  // start-light gantry — three lamps mirroring the physical modules
  const int cyL = 96, r = 17, cxs[3] = { 30, 67, 104 };
  for (int m = 0; m < 3; m++) {
    uint16_t c = 0x2104;                // dark/off
    if      (rxPhase == 1)                    c = (m < rxLit) ? 0xF800 : 0x2104;
    else if (rxPhase == 2 || rxPhase == 5)    c = 0xF800;     // red
    else if (rxPhase == 3 || rxPhase == 4)    c = 0x07E0;     // green
    spr.fillCircle(cxs[m], cyL, r, c);
    spr.drawCircle(cxs[m], cyL, r, 0x630C);
  }

  // status line
  const int sy = 140;
  if (rxPhase == 0)      { spr.setTextSize(2); spr.setTextColor(0x8410, COL_BG); spr.setCursor(22, sy); spr.print("GET SET"); }
  else if (rxPhase == 1) { spr.setTextSize(2); spr.setTextColor(0xFD20, COL_BG); spr.setCursor(30, sy); spr.print("READY"); }
  else if (rxPhase == 2) { spr.setTextSize(2); spr.setTextColor(0xF800, COL_BG); spr.setCursor(30, sy); spr.print("WAIT.."); }
  else if (rxPhase == 3) { spr.setTextSize(3); spr.setTextColor(0x07E0, COL_BG); spr.setCursor(36, sy - 4); spr.print("GO!"); }
  else if (rxPhase == 4) {
    spr.setTextSize(3); spr.setTextColor(rxNewBest ? 0xFFE0 : 0x07E0, COL_BG);
    char b[12]; snprintf(b, sizeof(b), "%dms", rxMs);
    spr.setCursor(W / 2 - (int)strlen(b) * 9, sy - 4); spr.print(b);
    if (rxNewBest) { spr.setTextSize(1); spr.setTextColor(0xFFE0, COL_BG); spr.setCursor(W / 2 - 28, sy + 26); spr.print("NEW BEST!"); }
  } else {               // 5 jump start
    spr.setTextSize(2); spr.setTextColor(0xF800, COL_BG); spr.setCursor(6, sy); spr.print("JUMP START");
  }

  // best + hint
  spr.setTextSize(1); spr.setTextColor(0xFFE0, COL_BG);
  spr.setCursor(8, H - 30);
  if (rxBest > 0) spr.printf("best %d ms", rxBest); else spr.print("best  --");
  spr.setTextColor(0x8410, COL_BG); spr.setCursor(8, H - 16);
  spr.print(rxPhase == 0 ? "A: start   B: back" :
            rxPhase == 3 ? "A: NOW!" :
            rxPhase >= 4 ? "A: again   B: back" :
                           "A:(wait)   B: back");
}

// ===========================================================================
// Optional I2C joystick on the Grove port (auto-detected, shared by games)
// ===========================================================================
// Works with the M5 Joystick Unit (0x52, register-less x/y/btn bytes) and the
// Joystick2 Unit (0x63, 8-bit regs 0x10/0x11). Center is calibrated at game
// start (handles drift / both center-128 firmwares). If neither is present we
// fall back to IMU tilt for left/right so the game still plays.
static const uint32_t JS_FREQ = 100000;
static int  jsKind = -1;       // -1 unprobed, 0 none, 1 Joystick(0x52), 2 Joystick2(0x63)
static int  jsCx = 128, jsCy = 128;     // calibrated rest center
static const int  JS_THR = 40;          // dead-zone radius (raw 0..255)
static const bool JS_SWAP_XY = true;    // unit mounted rotated 90° -> swap axes
static const bool JS_INV_X = false;     // flip if left/right come out reversed
static const bool JS_INV_Y = true;      // flip if up/down come out reversed
// On-screen calibration overlay (raw stick + button levels). Set false once
// the axes/buttons are dialed in.
static const bool TET_DEBUG = false;

static void jsProbe() {
  M5.Ex_I2C.begin();                     // harmless if already begun by tlBegin
  if      (M5.Ex_I2C.scanID(0x63, JS_FREQ)) jsKind = 2;
  else if (M5.Ex_I2C.scanID(0x52, JS_FREQ)) jsKind = 1;
  else    jsKind = 0;
}
static bool jsReadRaw(int* x, int* y) {
  if (jsKind == 2) {
    *x = M5.Ex_I2C.readRegister8(0x63, 0x10, JS_FREQ);
    *y = M5.Ex_I2C.readRegister8(0x63, 0x11, JS_FREQ);
    return true;
  }
  if (jsKind == 1) {
    uint8_t b[3] = { 128, 128, 0 };
    if (!M5.Ex_I2C.start(0x52, true, JS_FREQ)) { M5.Ex_I2C.stop(); return false; }
    M5.Ex_I2C.read(b, 3);
    M5.Ex_I2C.stop();
    *x = b[0]; *y = b[1];
    return true;
  }
  return false;
}
static void jsCalibrate() {
  if (jsKind < 0) jsProbe();
  int x = 128, y = 128;
  if (jsReadRaw(&x, &y)) { jsCx = x; jsCy = y; }
}
// Quantise the stick to {-1,0,1} per axis (with dead-zone). No joystick -> use
// IMU tilt for the X axis only (Y stays 0, so gravity still drops the piece).
static void jsDir(int* dx, int* dy) {
  *dx = 0; *dy = 0;
  int x, y;
  if (!jsReadRaw(&x, &y)) {
    float ax, ay, az; compat::getAccel(&ax, &ay, &az); (void)ax; (void)az;
    if      (ay >  0.35f) *dx = -1;
    else if (ay < -0.35f) *dx =  1;
    return;
  }
  int ex = x - jsCx, ey = y - jsCy;
  if (JS_SWAP_XY) { int t = ex; ex = ey; ey = t; }
  if (JS_INV_X) ex = -ex;
  if (JS_INV_Y) ey = -ey;
  if      (ex >  JS_THR) *dx =  1;
  else if (ex < -JS_THR) *dx = -1;
  if      (ey >  JS_THR) *dy =  1;
  else if (ey < -JS_THR) *dy = -1;
}

// ===========================================================================
// Game 5 — Tetris (俄罗斯方块). Joystick-only: L/R move (DAS auto-repeat),
// UP = rotate (with wall-kick, one turn per push), down = soft drop, B = back.
// The A button is NOT used in-game (only restarts after GAME OVER).
// ===========================================================================
static const int TET_COLS = 10, TET_ROWS = 20;
static const int TET_CELL = 11;
static const int TET_FX = 12, TET_FY = 20;             // field top-left (px)
static const uint32_t TET_DAS_DELAY = 180, TET_DAS_RATE = 55;   // ms
static const uint32_t TET_SOFT_MS = 45;                // soft-drop gravity period

// 7 tetrominoes x 4 rotations, packed as 4x4 bitmaps (bit = row*4+col, row 0 top).
static const uint16_t TET_SHAPE[7][4] = {
  { 0x00F0, 0x4444, 0x0F00, 0x2222 },   // I
  { 0x0660, 0x0660, 0x0660, 0x0660 },   // O
  { 0x0072, 0x0262, 0x0270, 0x0232 },   // T
  { 0x0036, 0x0462, 0x0360, 0x0231 },   // S
  { 0x0063, 0x0264, 0x0630, 0x0132 },   // Z
  { 0x0071, 0x0226, 0x0470, 0x0322 },   // J
  { 0x0074, 0x0622, 0x0170, 0x0223 },   // L
};
static const uint16_t TET_COL[7] = {
  0x07FF, 0xFFE0, 0xF81F, 0x07E0, 0xF800, 0x001F, 0xFD20,   // I O T S Z J L
};

static uint8_t  tBoard[TET_ROWS][TET_COLS];   // 0 empty, else colour index+1
static int      tType, tNext, tRot, tPx, tPy;
static int      tScore, tLines, tLevel;
static bool     tOver;
static uint32_t tDropT;          // next gravity step (ms)
static int      tDasDir;         // current held horizontal dir (-1/0/1)
static uint32_t tDasT;           // next auto-repeat time (ms)
static bool     tPrevUp;         // edge-detect hard-drop

static inline bool tCell(int type, int rot, int r, int c) {
  return (TET_SHAPE[type][rot] >> (r * 4 + c)) & 1;
}
// Does piece (type,rot) at board top-left (px,py) collide with walls/floor/stack?
static bool tCollide(int type, int rot, int px, int py) {
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      if (!tCell(type, rot, r, c)) continue;
      int br = py + r, bc = px + c;
      if (bc < 0 || bc >= TET_COLS || br >= TET_ROWS) return true;
      if (br >= 0 && tBoard[br][bc]) return true;     // br<0 = still above field
    }
  return false;
}
static void tSpawn() {
  tType = tNext; tNext = (int)gRand(7);
  tRot = 0; tPx = 3; tPy = 0;
  if (tCollide(tType, tRot, tPx, tPy)) { tOver = true; beep(500, 120); beep(350, 200); }
}
static void tLockAndClear() {
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++)
      if (tCell(tType, tRot, r, c)) {
        int br = tPy + r, bc = tPx + c;
        if (br >= 0 && br < TET_ROWS && bc >= 0 && bc < TET_COLS)
          tBoard[br][bc] = (uint8_t)(tType + 1);
      }
  int cleared = 0;
  for (int r = TET_ROWS - 1; r >= 0; ) {
    bool full = true;
    for (int c = 0; c < TET_COLS; c++) if (!tBoard[r][c]) { full = false; break; }
    if (full) {
      cleared++;
      for (int rr = r; rr > 0; rr--)
        for (int c = 0; c < TET_COLS; c++) tBoard[rr][c] = tBoard[rr - 1][c];
      for (int c = 0; c < TET_COLS; c++) tBoard[0][c] = 0;
    } else r--;
  }
  if (cleared) {
    static const int LS[5] = { 0, 40, 100, 300, 1200 };   // classic line-clear scores
    tScore += LS[cleared] * (tLevel + 1);
    tLines += cleared; tLevel = tLines / 10;
    if (cleared >= 4) { beep(2200, 80); beep(2600, 80); beep(3100, 120); }
    else { beep(2400, 60); beep(2800, 80); }
  }
  tSpawn();
}
static uint32_t tGravityMs() {            // gravity period for the current level
  int ms = 600 - tLevel * 55;
  return (uint32_t)(ms < 90 ? 90 : ms);
}
static void tMoveH(int d) {
  if (!tCollide(tType, tRot, tPx + d, tPy)) { tPx += d; beep(1500, 12); }
}
static void tetRotate() {                 // rotate cw with simple wall-kick
  int nr = (tRot + 1) & 3;
  if (!tCollide(tType, nr, tPx, tPy)) { tRot = nr; beep(1900, 25); return; }
  static const int KICK[4] = { -1, 1, -2, 2 };
  for (int k = 0; k < 4; k++)
    if (!tCollide(tType, nr, tPx + KICK[k], tPy)) {
      tPx += KICK[k]; tRot = nr; beep(1900, 25); return;
    }
  beep(400, 30);                          // rotation blocked
}
static void tetHardDrop() {               // slam to the bottom and lock
  int dist = 0;
  while (!tCollide(tType, tRot, tPx, tPy + 1)) { tPy++; dist++; }
  tScore += dist * 2; beep(1700, 30);
  tLockAndClear();
  tDropT = millis() + tGravityMs();
}

// ---- External dual-button unit on the top HAT (G1 = rotate, G8 = hard drop)
// Wire: GND->HAT pin1, A->pin7 (G1), B->pin9 (G8). Buttons are active-low with
// the MCU's internal pull-up; the unit's 5V need not be connected.
static const int HAT_BTN_A = 1;    // HAT 'G1' pad -> rotate
static const int HAT_BTN_B = 8;    // HAT 'G8' pad -> hard drop
// This unit drives the signal HIGH on press (needs VCC -> wire it to 3V3, not
// 5V). Set false for a plain button-to-GND unit (idle pull-up, press = LOW).
static const bool HAT_ACTIVE_HIGH = false;
static bool tHatInit = false;
static bool tPrevBtnA = false, tPrevBtnB = false;
static void tHatBegin() {
  int mode = HAT_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP;
  pinMode(HAT_BTN_A, mode);
  pinMode(HAT_BTN_B, mode);
  tHatInit = true;
}
static inline bool tHatPressed(int pin) {
  return HAT_ACTIVE_HIGH ? (digitalRead(pin) == HIGH) : (digitalRead(pin) == LOW);
}

static void gTetrisInit() {
  for (int r = 0; r < TET_ROWS; r++)
    for (int c = 0; c < TET_COLS; c++) tBoard[r][c] = 0;
  tScore = 0; tLines = 0; tLevel = 0; tOver = false;
  tDasDir = 0; tDasT = 0; tPrevUp = false;
  if (!tHatInit) tHatBegin();
  tPrevBtnA = tPrevBtnB = false;
  jsCalibrate();                          // capture stick rest position
  tNext = (int)gRand(7); tSpawn();
  tDropT = millis() + tGravityMs();
}
static void gTetrisA() {                  // in-game A is unused; only restarts after GAME OVER
  if (tOver) gTetrisInit();
}

static inline void tDrawCell(int bc, int br, uint16_t col) {
  int x = TET_FX + bc * TET_CELL, y = TET_FY + br * TET_CELL;
  spr.fillRect(x, y, TET_CELL - 1, TET_CELL - 1, col);
}
static void gTetrisTick() {
  uint32_t now = millis();
  if (!tOver) {
    int dx, dy; jsDir(&dx, &dy);

    // Horizontal move with DAS (initial tap, then auto-repeat while held).
    if (dx != 0) {
      if (dx != tDasDir) { tMoveH(dx); tDasDir = dx; tDasT = now + TET_DAS_DELAY; }
      else if ((int32_t)(now - tDasT) >= 0) { tMoveH(dx); tDasT = now + TET_DAS_RATE; }
    } else tDasDir = 0;

    // External HAT buttons: A = rotate, B = hard drop (edge-triggered).
    bool bA = tHatPressed(HAT_BTN_A);
    bool bB = tHatPressed(HAT_BTN_B);
    if (bA && !tPrevBtnA) tetRotate();
    if (bB && !tPrevBtnB) tetHardDrop();
    tPrevBtnA = bA; tPrevBtnB = bB;

    // Joystick UP also rotates (fallback when the buttons aren't wired yet).
    bool up = (dy < 0);
    if (up && !tPrevUp) tetRotate();
    tPrevUp = up;

    // Gravity — faster while pushing down (soft drop).
    uint32_t period = (dy > 0) ? TET_SOFT_MS : tGravityMs();
    if ((int32_t)(now - tDropT) >= 0) {
      if (!tCollide(tType, tRot, tPx, tPy + 1)) { tPy++; if (dy > 0) tScore += 1; }
      else tLockAndClear();
      tDropT = now + period;
    }
  }

  // ---- render (portrait) ----
  spr.fillSprite(COL_BG);
  // HUD: score (left) + next-piece mini preview (right)
  spr.setTextSize(1);
  if (TET_DEBUG) {
    int rx = jsCx, ry = jsCy; jsReadRaw(&rx, &ry);
    int bA = digitalRead(HAT_BTN_A);    // raw level: 1=HIGH, 0=LOW
    int bB = digitalRead(HAT_BTN_B);
    spr.setTextColor(0x07FF, COL_BG);
    spr.setCursor(2, 2);  spr.printf("k%d x%d y%d", jsKind, rx, ry);
    spr.setCursor(2, 11); spr.printf("G1:%d G8:%d  S%d", bA, bB, tScore);
  } else {
    spr.setTextColor(COL_HUD, COL_BG);
    spr.setCursor(2, 2);  spr.printf("%d", tScore);
    spr.setTextColor(0x8410, COL_BG);
    spr.setCursor(2, 11); spr.printf("L%d", tLevel);
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++)
        if (tCell(tNext, 0, r, c))
          spr.fillRect(108 + c * 4, 2 + r * 4, 3, 3, TET_COL[tNext]);
  }

  // field border + locked stack
  spr.drawRect(TET_FX - 1, TET_FY - 1, TET_COLS * TET_CELL + 1, TET_ROWS * TET_CELL + 1, COL_WALL);
  for (int r = 0; r < TET_ROWS; r++)
    for (int c = 0; c < TET_COLS; c++)
      if (tBoard[r][c]) tDrawCell(c, r, TET_COL[tBoard[r][c] - 1]);

  if (!tOver) {
    // ghost (landing preview) — dim outline
    int gy = tPy;
    while (!tCollide(tType, tRot, tPx, gy + 1)) gy++;
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++)
        if (tCell(tType, tRot, r, c) && gy + r >= 0) {
          int x = TET_FX + (tPx + c) * TET_CELL, y = TET_FY + (gy + r) * TET_CELL;
          spr.drawRect(x, y, TET_CELL - 1, TET_CELL - 1, 0x4208);
        }
    // active piece
    for (int r = 0; r < 4; r++)
      for (int c = 0; c < 4; c++)
        if (tCell(tType, tRot, r, c) && tPy + r >= 0)
          tDrawCell(tPx + c, tPy + r, TET_COL[tType]);
  } else {
    spr.setTextSize(2); spr.setTextColor(0xF800, COL_BG);
    spr.setCursor(W / 2 - 50, H / 2 - 24); spr.print("GAME OVER");
    spr.setTextSize(1); spr.setTextColor(COL_HUD, COL_BG);
    spr.setCursor(W / 2 - 30, H / 2 + 2); spr.printf("score %d", tScore);
    spr.setTextColor(0x8410, COL_BG);
    spr.setCursor(W / 2 - 42, H / 2 + 20); spr.print("A:retry  B:back");
  }
}

// ===========================================================================
// Picker + dispatcher
// ===========================================================================
// Picker rows = the games plus a trailing EXIT entry.
static const int PICK_N = GAME_N + 1;   // ..., EXIT
static void gDrawPicker() {
  spr.fillSprite(COL_BG);
  spr.setTextColor(COL_HUD, COL_BG); spr.setTextSize(2);
  spr.setCursor(18, 22); spr.print("GAMES");
  spr.setTextSize(2);
  for (int i = 0; i < PICK_N; i++) {
    bool sel = (i == gSel);
    bool exit = (i == GAME_N);
    int y = 48 + i * 29;
    if (sel) { spr.fillRoundRect(10, y - 4, W - 20, 25, 4, exit ? 0x4000 : 0x2945);
               spr.setTextColor(0xFFE0, exit ? 0x4000 : 0x2945); }
    else spr.setTextColor(exit ? 0xC986 : 0x8410, COL_BG);
    spr.setCursor(24, y); spr.print(sel ? "> " : "  ");
    spr.print(exit ? "EXIT" : GAME_NAMES[i]);
  }
  spr.setTextSize(1); spr.setTextColor(0x8410, COL_BG);
  spr.setCursor(10, H - 16); spr.print("A:next   B:select");
}

void gameInit() {
  gScreen = GS_PICKER; gSel = 0;
  randomSeed(micros() ^ (uint32_t)esp_random());
  if (tlPresent) tlAllOff();   // games own the lights; clear the session mirror
}
void gameTick() {
  lastInteractMs = millis();
  if (gScreen == GS_PICKER) { gDrawPicker(); return; }
  switch (gGame) {
    case 0: gMazeTick(); break;
    case 1: gSlotTick(); break;
    case 2: gRaceTick(); break;
    case 3: gReactTick(); break;
    case 4: gTetrisTick(); break;
  }
}
void gameButtonA() {              // short A (release edge)
  if (gScreen == GS_PICKER) { gSel = (gSel + 1) % PICK_N; beep(1800, 30); return; }
  switch (gGame) { case 0: gMazeA(); break; case 1: gSlotA(); break; case 2: gRaceA(); break; case 3: gReactA(); break; case 4: gTetrisA(); break; }
}
void gameButtonADown() {          // A down-edge — low-latency capture for timing games
  if (gScreen == GS_PLAY && gGame == 3) gReactDown();
}
void gameButtonB() {             // B
  if (gScreen == GS_PICKER) {
    if (gSel == GAME_N) { gameExit(); return; }   // EXIT row
    gGame = gSel; gScreen = GS_PLAY; beep(2400, 40);
    switch (gGame) { case 0: gMazeInit(); break; case 1: gSlotInit(); break; case 2: gRaceInit(); break; case 3: gReactInit(); break; case 4: gTetrisInit(); break; }
  } else {
    gScreen = GS_PICKER; beep(1200, 40);    // back to the picker
    if (tlPresent) tlAllOff();              // clear any lamps a game lit
  }
}
void gameExit() { gameActive = false; beep(900, 60); if (tlPresent) tlAllOff(); tlResync(); }
void gameButtonALong() {         // long A
  // In slots, long-press cycles the symbol skin; everywhere else it exits.
  if (gScreen == GS_PLAY && gGame == 1) {
    slTheme = (slTheme + 1) % SLOT_THEME_N; gSlotSaveTheme();
    beep(2600, 50); beep(3000, 70);
  } else {
    gameExit();
  }
}
