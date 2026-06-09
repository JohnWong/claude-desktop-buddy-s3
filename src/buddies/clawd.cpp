#include "../buddy.h"
#include "../buddy_common.h"
#include "../m5_compat.h"
#include <string.h>

// Clawd — the Claude Code welcome-screen lobster mascot (LogoV2/Clawd.tsx),
// ported as a buddy species. The original is drawn from Unicode quadrant
// blocks which the bitmap font can't render, so we rebuild Clawd from solid
// rects via buddyFillRect: coral body + black eye notches + two claws that
// raise for excited poses. Overlay particles (Z / ! / stars / hearts) reuse
// the shared ASCII overlay path.

namespace clawd {

static const uint16_t BODY = 0xDBAA;   // coral-orange (~216,116,80)
static const uint16_t DARK = 0x9225;   // darker coral for claws/legs/antennae

// Eye render modes
enum { EYE_OPEN, EYE_CLOSED, EYE_WIDE, EYE_HEART };

// Wipe the full motion envelope so moving claws / hops leave no trail.
static void clear() { buddyFillRect(-26, -9, 52, 38, BUDDY_BG); }

static void body(int dx, int dy, bool clawUp) {
  // antennae
  buddyFillRect(dx - 11, dy - 4, 2, 6, DARK);
  buddyFillRect(dx + 9,  dy - 4, 2, 6, DARK);
  // head + body + lower taper
  buddyFillRect(dx - 13, dy + 2,  26, 8, BODY);
  buddyFillRect(dx - 15, dy + 10, 30, 8, BODY);
  buddyFillRect(dx - 12, dy + 18, 24, 6, BODY);
  // claws — raised above the head, or down at the sides
  if (clawUp) {
    buddyFillRect(dx - 20, dy - 6, 7, 9, BODY);
    buddyFillRect(dx + 13, dy - 6, 7, 9, BODY);
    buddyFillRect(dx - 18, dy - 6, 3, 3, BUDDY_BG);   // pincer gap
    buddyFillRect(dx + 15, dy - 6, 3, 3, BUDDY_BG);
  } else {
    buddyFillRect(dx - 21, dy + 11, 7, 10, BODY);
    buddyFillRect(dx + 14, dy + 11, 7, 10, BODY);
    buddyFillRect(dx - 21, dy + 11, 3, 3, BUDDY_BG);
    buddyFillRect(dx + 18, dy + 11, 3, 3, BUDDY_BG);
  }
  // little legs
  for (int i = 0; i < 4; i++)
    buddyFillRect(dx - 9 + i * 6, dy + 24, 2, 3, DARK);
}

static void eyes(int dx, int dy, int gaze, uint8_t mode) {
  int lx = dx - 8, rx = dx + 5, ey = dy + 4;
  if (mode == EYE_CLOSED) {
    buddyFillRect(lx, ey + 1, 4, 1, BUDDY_BG);
    buddyFillRect(rx - 1, ey + 1, 4, 1, BUDDY_BG);
    return;
  }
  uint16_t col = (mode == EYE_HEART) ? BUDDY_HEART : BUDDY_BG;
  int sz = (mode == EYE_WIDE) ? 4 : 3;
  buddyFillRect(lx + gaze, ey, sz, sz, col);
  buddyFillRect(rx + gaze, ey, sz, sz, col);
}

// ─── SLEEP ───  slow breathe, closed eyes, drifting Z + a rising bubble
static void doSleep(uint32_t t) {
  clear();
  int br = ((t / 5) & 1) ? 1 : 0;
  body(0, br, false);
  eyes(0, br, 0, EYE_CLOSED);
  int p = t % 12;
  buddySetColor(BUDDY_WHITE);
  buddySetCursor(BUDDY_X_CENTER + 10 + p, BUDDY_Y_OVERLAY + 12 - p);
  buddyPrint((t / 3) & 1 ? "Z" : "z");
  int b = (t + 5) % 16;
  buddySetColor(BUDDY_CYAN);
  buddySetCursor(BUDDY_X_CENTER - 16, BUDDY_Y_OVERLAY + 16 - b);
  buddyPrint("o");
}

// ─── IDLE ───  glance around, blink, occasional claw clack
static void doIdle(uint32_t t) {
  clear();
  static const uint8_t SEQ[] = { 0,0,0,1,0,0,2,0,0,3,0,1,0,4,0,0 };
  uint8_t s = SEQ[(t / 5) % sizeof(SEQ)];
  int gaze = (s == 2) ? -2 : (s == 3) ? 2 : 0;
  bool claw = (s == 4) && ((t / 2) & 1);
  body(0, 0, claw);
  eyes(0, 0, gaze, s == 1 ? EYE_CLOSED : EYE_OPEN);
}

// ─── BUSY ───  claws snip fast, focused; bubbles stream up + a sweat bead
static void doBusy(uint32_t t) {
  clear();
  body(0, 0, (t & 1));
  eyes(0, 0, 0, EYE_OPEN);
  buddySetColor(BUDDY_CYAN);
  for (int i = 0; i < 3; i++) {
    int b = (t * 2 + i * 5) % 14;
    buddySetCursor(BUDDY_X_CENTER + 14 + i * 4, BUDDY_Y_OVERLAY + 16 - b);
    buddyPrint(".");
  }
  if ((t / 4) & 1) {
    buddySetColor(BUDDY_WHITE);
    buddySetCursor(BUDDY_X_CENTER - 16, BUDDY_Y_OVERLAY + 4);
    buddyPrint(",");
  }
}

// ─── ATTENTION ───  claws up, wide eyes, bounce, pulsing !
static void doAttention(uint32_t t) {
  clear();
  int bob = ((t / 2) & 1) ? -2 : 0;
  body(0, bob, true);
  eyes(0, bob, 0, EYE_WIDE);
  if ((t / 2) & 1) {
    buddySetColor(BUDDY_RED);
    buddySetCursor(BUDDY_X_CENTER - 2, BUDDY_Y_OVERLAY);
    buddyPrint("!");
  }
}

// ─── CELEBRATE ───  jump with claws up, confetti raining
static void doCelebrate(uint32_t t) {
  clear();
  static const int8_t JY[] = { 0,-3,-6,-8,-6,-3,0,0 };
  int dy = JY[(t / 2) % 8];
  body(0, dy, true);
  eyes(0, dy, 0, EYE_WIDE);
  static const char* const C[] = { "*", "+", "." };
  static const uint16_t CC[] = { BUDDY_YEL, BUDDY_CYAN, BUDDY_HEART };
  for (int i = 0; i < 5; i++) {
    int ph = (t + i * 3) % 14;
    buddySetColor(CC[i % 3]);
    buddySetCursor(BUDDY_X_CENTER - 18 + i * 9, BUDDY_Y_OVERLAY - 2 + ph);
    buddyPrint(C[i % 3]);
  }
}

// ─── DIZZY ───  body wobbles side to side, stars circle overhead
static void doDizzy(uint32_t t) {
  clear();
  static const int8_t WX[] = { -3,-2,0,2,3,2,0,-2 };
  int dx = WX[(t / 2) % 8];
  body(dx, 0, false);
  eyes(dx, 0, 0, EYE_CLOSED);
  static const int8_t SX[] = { -6,-3,0,3,6,3,0,-3 };
  static const int8_t SY[] = { 0,1,2,1,0,-1,-2,-1 };
  buddySetColor(BUDDY_YEL);
  for (int i = 0; i < 3; i++) {
    int k = (t + i * 3) % 8;
    buddySetCursor(BUDDY_X_CENTER + SX[k], BUDDY_Y_OVERLAY + SY[k]);
    buddyPrint("*");
  }
}

// ─── HEART ───  heart eyes, gentle bob, rising hearts
static void doHeart(uint32_t t) {
  clear();
  int bob = ((t / 4) & 1) ? 1 : 0;
  body(0, bob, false);
  eyes(0, bob, 0, EYE_HEART);
  buddySetColor(BUDDY_HEART);
  for (int i = 0; i < 5; i++) {
    int ph = (t + i * 4) % 16;
    int y = BUDDY_Y_OVERLAY + 16 - ph;
    if (y < -2 || y > BUDDY_Y_BASE) continue;
    buddySetCursor(BUDDY_X_CENTER - 16 + i * 8, y);
    buddyPrint("v");
  }
}

}  // namespace clawd

extern const Species CLAWD_SPECIES = {
  "clawd",
  0xDBAA,
  { clawd::doSleep, clawd::doIdle, clawd::doBusy, clawd::doAttention,
    clawd::doCelebrate, clawd::doDizzy, clawd::doHeart }
};
