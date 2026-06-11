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
static const int SLOT_THEME_N = 3;  // CASINO / FRUIT / SUITS

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

static const char* SLOT_THEME_NAME[] = { "CASINO", "FRUIT", "SUITS" };
static void gSlotSym(int cx, int cy, int idx, int theme) {
  if (theme == 1) gSym1(cx, cy, idx);
  else if (theme == 2) gSym2(cx, cy, idx);
  else gSym0(cx, cy, idx);
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
    int y = 68 + i * 32;
    if (sel) { spr.fillRoundRect(10, y - 4, W - 20, 26, 4, exit ? 0x4000 : 0x2945);
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
  if (gScreen == GS_PICKER) { gSel = (gSel + 1) % PICK_N; beep(1800, 30); return; }
  switch (gGame) { case 0: gMazeA(); break; case 1: gSlotA(); break; case 2: gRaceA(); break; }
}
void gameButtonB() {             // B
  if (gScreen == GS_PICKER) {
    if (gSel == GAME_N) { gameExit(); return; }   // EXIT row
    gGame = gSel; gScreen = GS_PLAY; beep(2400, 40);
    switch (gGame) { case 0: gMazeInit(); break; case 1: gSlotInit(); break; case 2: gRaceInit(); break; }
  } else {
    gScreen = GS_PICKER; beep(1200, 40);    // back to the picker
  }
}
void gameExit() { gameActive = false; beep(900, 60); }
void gameButtonALong() {         // long A
  // In slots, long-press cycles the symbol skin; everywhere else it exits.
  if (gScreen == GS_PLAY && gGame == 1) {
    slTheme = (slTheme + 1) % SLOT_THEME_N; gSlotSaveTheme();
    beep(2600, 50); beep(3000, 70);
  } else {
    gameExit();
  }
}
