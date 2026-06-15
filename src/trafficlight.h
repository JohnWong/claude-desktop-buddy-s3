#pragma once
#include "m5_compat.h"
#include "data.h"

// ===========================================================================
// PbHub-driven physical traffic lights — the external twin of the on-screen
// session strip.
//
//   M5StickC Plus S3 --Grove/I2C--> PbHub (0x61) --> 3x 4-pin traffic-light
//   modules (GND/RE/YE/GR, common-cathode: drive a signal HIGH = that LED on).
//
// 9 signal lines spread over 5 PbHub channels — CH3 is skipped (awkward to
// wire) — using both digital outputs (IO0 + IO1) of each channel:
//
//   module 0  R=CH0.IO0  Y=CH0.IO1  G=CH1.IO0
//   module 1  R=CH1.IO1  Y=CH2.IO0  G=CH2.IO1
//   module 2  R=CH4.IO0  Y=CH4.IO1  G=CH5.IO0
//
// PbHub register layout (verified against m5stack/M5Unit-HUB unit_PbHub.cpp):
//   reg = (0x40 + 0x10 * chTable[ch]) + io , chTable = {0,1,2,3,4,6}
//   => CH0..CH5 bases 0x40/0x50/0x60/0x70/0x80/0xA0 ; io 0/1 picks IO0/IO1.
//   writeRegister8(0x61, reg, 0|1) drives that output low/high.
//
// This file only uses spr / W / H / compat::beep, all defined in main.cpp
// before it is included.
// ===========================================================================

static const uint8_t  TL_ADDR   = 0x61;
static const uint32_t TL_FREQ   = 100000;
static const uint8_t  TL_CHTAB[6] = {0, 1, 2, 3, 4, 6};

struct TLPin { uint8_t ch, io; };
// Logical lamp index 0..8 -> {channel, io}. Order within a module is R, Y, G.
// NOTE: on this build the two signal wires of every channel are swapped vs the
// IO0/IO1 convention (bring-up self-test showed a consistent per-channel flip),
// so each pair's io index is 1/0 instead of 0/1. Verified against hardware.
static const TLPin TL_LAMP[9] = {
  {0, 1}, {0, 0}, {1, 1},   // module 0: RED YEL GRN
  {1, 0}, {2, 1}, {2, 0},   // module 1: RED YEL GRN
  {4, 1}, {4, 0}, {5, 1},   // module 2: RED YEL GRN   (CH3 avoided)
};

static bool     tlPresent  = false;
// Per-module animation state for the session mirror:
//   tlState — displayed colour currently targeted (0 off / 1 green / 2 yellow / 3 red)
//   tlPhys  — last colour physically pushed (0 off, else 1/2/3)
//   tlBurst — deadline (ms) for the fast emphasis blink after a colour change
static uint8_t  tlState[3] = {0, 0, 0};
static uint8_t  tlPhys[3]  = {0xFF, 0xFF, 0xFF};
static uint32_t tlBurst[3] = {0, 0, 0};
static const uint32_t TL_BURST_MS   = 1200;  // emphasis blink duration on change
static const uint32_t TL_BURST_HALF = 150;   // emphasis on/off half-period (~4 flashes)
static const uint32_t TL_RED_PER    = 1200;  // steady "needs you" red blink period

static inline uint8_t tlReg(uint8_t ch, uint8_t io) {
  return (uint8_t)(io + 0x40 + 0x10 * TL_CHTAB[ch]);
}
static inline void tlSet(uint8_t lamp, bool on) {
  const TLPin& p = TL_LAMP[lamp];
  M5.Ex_I2C.writeRegister8(TL_ADDR, tlReg(p.ch, p.io), on ? 1 : 0, TL_FREQ);
}
static void tlAllOff() {
  for (uint8_t i = 0; i < 9; i++) tlSet(i, false);
}

// Session state -> module color. 0 idle=off, 1 running=green, 2 awaiting=yellow,
// 3 approval=red. A module's lamps are [R, Y, G] at base+0 / +1 / +2.
static void tlModule(uint8_t m, uint8_t st) {
  uint8_t b = m * 3;
  tlSet(b + 0, st == 3);   // red    — needs your approval
  tlSet(b + 1, st == 2);   // yellow — awaiting your input
  tlSet(b + 2, st == 1);   // green  — running
}

// Probe the bus, blank all lamps. Returns true if the PbHub answered at 0x61.
static bool tlBegin() {
  M5.Ex_I2C.begin();
  tlPresent = M5.Ex_I2C.scanID(TL_ADDR, TL_FREQ);
  if (tlPresent) tlAllOff();
  for (uint8_t m = 0; m < 3; m++) { tlState[m] = 0; tlPhys[m] = 0xFF; tlBurst[m] = 0; }
  return tlPresent;
}

// Invalidate the physical shadow so the next tlUpdate re-pushes every module.
// Call after something else (e.g. a game) has driven the lamps directly.
static inline void tlResync() { tlPhys[0] = tlPhys[1] = tlPhys[2] = 0xFF; }

// Map a session state to a lamp colour. perm is rare here, so wait + perm both
// go red ("needs you"); idle shows amber standby; running is green. 0xFF (the
// "none"/empty-slot marker) and anything out of range stays dark.
//   st:     0 idle / 1 run / 2 wait / 3 perm / 0xFF empty
//   colour: 0 off / 1 green / 2 yellow / 3 red   (matches tlModule's selector)
static inline uint8_t tlColour(uint8_t st) {
  if (st == 1)            return 1;   // running -> green
  if (st == 2 || st == 3) return 3;   // awaiting input / approval -> red
  if (st == 0)            return 2;   // idle -> yellow standby
  return 0;                           // empty / unknown -> off
}

// Mirror the live sessions onto the physical lamps with attention cues: a colour
// change kicks off a fast emphasis blink (~4 flashes); in steady state only red
// ("needs you") keeps a low-frequency blink, while green/yellow stay solid and
// no-session is off. Only writes the bus on a flip.
static void tlUpdate(const TamaState& s) {
  if (!tlPresent) return;
  uint32_t now = millis();
  for (uint8_t m = 0; m < 3; m++) {
    uint8_t col = tlColour((m < s.sessCount) ? s.sessState[m] : 0xFF);
    if (col != tlState[m]) {                       // colour changed -> emphasize
      tlState[m] = col;
      tlBurst[m] = (col == 0) ? 0 : now + TL_BURST_MS;  // no burst when going dark
    }
    uint8_t want;                                  // 0 = off, else lit colour = col
    if (col == 0) {
      want = 0;
    } else if (now < tlBurst[m]) {                 // fast emphasis blink on change
      want = ((now / TL_BURST_HALF) & 1) ? 0 : col;
    } else if (col == 3) {                          // steady: only red blinks, low freq
      want = (now % TL_RED_PER < TL_RED_PER * 55 / 100) ? 3 : 0;
    } else {                                        // green / yellow -> solid
      want = col;
    }
    if (want != tlPhys[m]) { tlModule(m, want); tlPhys[m] = want; }
  }
}

// ---------------------------------------------------------------------------
// Visual bring-up self-test: probe 0x61, then walk every lamp one at a time so
// you can eyeball that each color lights on the right module. Blocking (~8s),
// draws its own frames — same pattern as the boot splash. Menu-triggered.
// ---------------------------------------------------------------------------
static const char* TL_LBL[9] = {
  "L1 RED", "L1 YEL", "L1 GRN",
  "L2 RED", "L2 YEL", "L2 GRN",
  "L3 RED", "L3 YEL", "L3 GRN",
};
static const uint16_t TL_COL[9] = {
  0xF800, 0xFFE0, 0x07E0, 0xF800, 0xFFE0, 0x07E0, 0xF800, 0xFFE0, 0x07E0,
};

void tlSelfTest() {
  bool ok = tlBegin();   // re-probe so the test reflects the current wiring

  spr.fillSprite(0x0000);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(0xFFFF, 0x0000);
  spr.setTextSize(2);
  spr.setCursor(8, 12);  spr.print("LAMP TEST");
  spr.setTextSize(1);
  spr.setTextColor(0xC618, 0x0000);
  spr.setCursor(8, 44);  spr.printf("SDA %d  SCL %d",
                                    M5.Ex_I2C.getSDA(), M5.Ex_I2C.getSCL());
  spr.setCursor(8, 60);
  if (ok) { spr.setTextColor(0x07E0, 0x0000); spr.print("PbHub 0x61: OK"); }
  else    { spr.setTextColor(0xF800, 0x0000); spr.print("PbHub 0x61: ---"); }
  spr.pushSprite(0, 0);
  compat::beep(ok ? 2600 : 400, ok ? 80 : 300);
  delay(1300);

  if (!ok) {
    spr.setTextColor(0xFFFF, 0x0000);
    spr.setCursor(8, 84);  spr.print("check Grove cable");
    spr.setCursor(8, 100); spr.print("& PbHub power");
    spr.pushSprite(0, 0);
    delay(1800);
    spr.setTextDatum(TL_DATUM);
    return;
  }

  for (uint8_t i = 0; i < 9; i++) {
    tlAllOff();
    tlSet(i, true);
    spr.fillSprite(0x0000);
    spr.setTextColor(0xFFFF, 0x0000);
    spr.setTextSize(1);
    spr.setCursor(8, 12);  spr.printf("LAMP %d / 9", i + 1);
    spr.setTextSize(3);
    spr.setTextColor(TL_COL[i], 0x0000);
    spr.setCursor(8, 70);  spr.print(TL_LBL[i]);
    spr.fillCircle(W / 2, 165, 26, TL_COL[i]);
    spr.drawCircle(W / 2, 165, 26, 0xFFFF);
    spr.setTextSize(1);
    spr.pushSprite(0, 0);
    compat::beep(1500 + i * 110, 60);
    delay(700);
  }

  tlAllOff();
  spr.fillSprite(0x0000);
  spr.setTextColor(0x07E0, 0x0000);
  spr.setTextSize(2);
  spr.setCursor(8, H / 2 - 8);  spr.print("TEST DONE");
  spr.setTextSize(1);
  spr.pushSprite(0, 0);
  compat::beep(2600, 80);
  compat::beep(3200, 120);
  delay(1100);

  spr.setTextDatum(TL_DATUM);
  tlState[0] = tlState[1] = tlState[2] = 0;
  tlPhys[0] = tlPhys[1] = tlPhys[2] = 0xFF;          // force re-mirror after test
}
