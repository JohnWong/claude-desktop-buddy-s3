#pragma once
// Tilt-ball maze mini-game for StickS3 (portrait 135x240).
// Self-contained: draws into the global `spr`, reads the IMU via compat,
// uses the global beep() and lastInteractMs. The main loop calls gameTick()
// once per frame while gameActive, and owns the single spr.pushSprite().
//
// Relies on these symbols already declared in main.cpp before this include:
//   extern TFT_eSprite spr;  const int W, H;  uint32_t lastInteractMs;
//   void beep(uint16_t, uint16_t);  namespace compat { getAccel(...); }

extern bool gameActive;

// ---- Tunables (file-static so they're easy to tweak later) -----------------
static const float GAME_K       = 0.6f;   // tilt -> acceleration gain
static const float GAME_DAMP    = 0.92f;  // per-frame velocity damping
static const float GAME_LP      = 0.4f;   // accel low-pass: smoothed = old*0.6 + raw*0.4
static const float GAME_R       = 4.0f;   // ball radius (px)
static const float GAME_VMAX    = 4.0f;   // velocity clamp (px/frame)
static const uint32_t GAME_WIN_MS = 1500; // auto-restart delay after a win

static const uint16_t COL_WALL = 0x2104; // dark
static const uint16_t COL_GOAL = 0x07E0; // green
static const uint16_t COL_BALL = 0xFFE0; // amber/yellow
static const uint16_t COL_BG   = 0x0000; // black
static const uint16_t COL_HUD  = 0xFFFF; // white

struct GameRect { int x, y, w, h; };

// Fixed serpentine maze: horizontal bars with alternating gaps. The ball
// must weave top -> bottom. Walls are AABBs in sprite coordinates.
static const GameRect GAME_WALLS[] = {
  { 0,   40, 100, 8 },   // bar leaving a gap on the right
  { 35,  80, 100, 8 },   // gap on the left
  { 0,  120, 100, 8 },   // gap on the right
  { 35, 160, 100, 8 },   // gap on the left
  { 0,  200,  70, 8 },   // gap on the right, just above the goal
};
static const int GAME_WALL_N = sizeof(GAME_WALLS) / sizeof(GAME_WALLS[0]);

static const GameRect GAME_GOAL = { 95, 218, 32, 18 };

// ---- State -----------------------------------------------------------------
static float gBx, gBy, gVx, gVy;
static float gSmAx, gSmAy;        // low-passed accel
static float gAx0, gAy0;          // calibration (neutral tilt)
static bool  gWon = false;
static uint32_t gWonMs = 0;
static uint32_t gStartMs = 0;

static inline bool gAabbOverlap(float cx, float cy, float r, const GameRect& w) {
  return (cx + r > w.x) && (cx - r < w.x + w.w) &&
         (cy + r > w.y) && (cy - r < w.y + w.h);
}

// Push the ball out of one wall along the axis of least penetration and kill
// that velocity component. Treats the ball as an AABB of half-size r.
static void gResolve(const GameRect& w) {
  if (!gAabbOverlap(gBx, gBy, GAME_R, w)) return;
  // overlap on each side
  float penL = (gBx + GAME_R) - w.x;          // push left by this
  float penR = (w.x + w.w) - (gBx - GAME_R);  // push right
  float penT = (gBy + GAME_R) - w.y;          // push up
  float penB = (w.y + w.h) - (gBy - GAME_R);  // push down
  float minX = (penL < penR) ? penL : penR;
  float minY = (penT < penB) ? penT : penB;
  if (minX < minY) {
    gBx += (penL < penR) ? -penL : penR;
    gVx = 0;
  } else {
    gBy += (penT < penB) ? -penT : penB;
    gVy = 0;
  }
}

static void gReadAccelCalibrated(float* dx, float* dy) {
  float ax, ay, az;
  compat::getAccel(&ax, &ay, &az);
  gSmAx = gSmAx * (1.0f - GAME_LP) + ax * GAME_LP;
  gSmAy = gSmAy * (1.0f - GAME_LP) + ay * GAME_LP;
  *dx = gSmAx - gAx0;
  *dy = gSmAy - gAy0;
}

void gameInit() {
  gBx = 18; gBy = 16;            // top-left, before the first bar
  gVx = gVy = 0;
  gWon = false;
  gWonMs = 0;
  gStartMs = millis();
  // Seed the low-pass and capture neutral tilt as the calibration baseline.
  float ax, ay, az;
  compat::getAccel(&ax, &ay, &az);
  gSmAx = ax; gSmAy = ay;
  gAx0 = ax;  gAy0 = ay;
}

void gameTick() {
  lastInteractMs = millis();   // keep the device awake while playing

  if (!gWon) {
    float dx, dy;
    gReadAccelCalibrated(&dx, &dy);

    // TUNABLE: portrait gravity sits mostly on X (vertical) and Y (horizontal).
    // First-guess mapping; the user may want a sign flip or an axis swap.
    gVx += dy * GAME_K;
    gVy += dx * GAME_K;

    gVx *= GAME_DAMP;
    gVy *= GAME_DAMP;
    if (gVx >  GAME_VMAX) gVx =  GAME_VMAX;
    if (gVx < -GAME_VMAX) gVx = -GAME_VMAX;
    if (gVy >  GAME_VMAX) gVy =  GAME_VMAX;
    if (gVy < -GAME_VMAX) gVy = -GAME_VMAX;

    gBx += gVx;
    gBy += gVy;

    // Border collision (1px wall around the playfield).
    if (gBx - GAME_R < 1)       { gBx = 1 + GAME_R;       gVx = 0; }
    if (gBx + GAME_R > W - 2)    { gBx = W - 2 - GAME_R;   gVx = 0; }
    if (gBy - GAME_R < 1)       { gBy = 1 + GAME_R;       gVy = 0; }
    if (gBy + GAME_R > H - 2)    { gBy = H - 2 - GAME_R;   gVy = 0; }

    // Wall collisions.
    for (int i = 0; i < GAME_WALL_N; i++) gResolve(GAME_WALLS[i]);

    // Win check: ball center inside the goal.
    if (gBx > GAME_GOAL.x && gBx < GAME_GOAL.x + GAME_GOAL.w &&
        gBy > GAME_GOAL.y && gBy < GAME_GOAL.y + GAME_GOAL.h) {
      gWon = true;
      gWonMs = millis();
      beep(2200, 80); beep(2600, 80); beep(3100, 120);  // happy rising tune
    }
  } else if (millis() - gWonMs > GAME_WIN_MS) {
    gameInit();   // auto-restart the level
  }

  // ---- Render the whole playfield --------------------------------------
  spr.fillSprite(COL_BG);
  // border
  spr.drawRect(0, 0, W, H, COL_WALL);
  // goal
  spr.fillRect(GAME_GOAL.x, GAME_GOAL.y, GAME_GOAL.w, GAME_GOAL.h, COL_GOAL);
  // walls
  for (int i = 0; i < GAME_WALL_N; i++)
    spr.fillRect(GAME_WALLS[i].x, GAME_WALLS[i].y,
                 GAME_WALLS[i].w, GAME_WALLS[i].h, COL_WALL);
  // ball
  spr.fillCircle((int)gBx, (int)gBy, (int)GAME_R, COL_BALL);

  // thin HUD line: elapsed seconds (or WIN banner).
  spr.setTextSize(1);
  if (gWon) {
    spr.setTextColor(COL_GOAL, COL_BG);
    spr.setCursor(W / 2 - 12, 4);
    spr.print("WIN");
  } else {
    spr.setTextColor(COL_HUD, COL_BG);
    spr.setCursor(3, 3);
    spr.printf("%lus", (millis() - gStartMs) / 1000);
  }
}

void gameButtonA() {   // short A = restart the level
  beep(1800, 40);
  gameInit();
}

void gameExit() {
  gameActive = false;
  beep(900, 60);
}
