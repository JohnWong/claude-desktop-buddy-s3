#!/usr/bin/env python3
"""Phase 2 / M2 — Claude Code → StickS3 bridge (no desktop app).

A single asyncio process that:
  1. Listens on a Unix socket (~/.claude-buddy/bridge.sock) for events posted by
     Claude Code hooks (see hooks/buddy_hook.py).
  2. Aggregates per-session state across all terminals into the firmware's
     official schema {total, running, waiting, msg}.
  3. Holds a persistent BLE link to the buddy (Claude-XXXX) over Nordic UART and
     pushes a frame every ~2.5s (firmware staleness window is 30s), auto-reconnecting.

Design notes (borrowed from srokaw/terminal-claude-code-buddy-m5stack):
  - Socket IPC, newline-delimited JSON, one object per line.
  - Data minimization: only counts + a short non-sensitive label reach the device.
    No prompt text, file contents, diffs, or transcript ever leave the host.
  - Fail-open: if the bridge is down, hooks just no-op and Claude Code works
    normally (see hooks/buddy_hook.py).

Run:  ~/.pio-venv/bin/python bridge/buddy_bridge.py
"""
import asyncio
import json
import os
import subprocess
import time

from bleak import BleakClient, BleakScanner

# ── BLE / Nordic UART ────────────────────────────────────────────────────────
NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # host -> stick (write)
NUS_TX      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # stick -> host (notify)
NAME_PREFIX = "Claude-"

# ── Socket IPC ───────────────────────────────────────────────────────────────
SOCK_DIR  = os.path.expanduser("~/.claude-buddy")
SOCK_PATH = os.path.join(SOCK_DIR, "bridge.sock")

PUSH_PERIOD   = 2.5     # seconds between device frames (< 30s staleness window)
SESSION_TTL   = 3600    # drop sessions with no events for this long (safety)

# session_id -> {"state": "idle"|"running"|"waiting", "label": str, "seen": ts}
SESSIONS: dict[str, dict] = {}

# Fixed display slots for the 3-light strip. A session keeps its slot (stable
# position) while shown, but is evicted when a more-active session pushes it out
# of the top-3-by-activity. None = empty slot. See aggregate().
SLOTS: list = [None, None, None]

# Ghostty tab ordering (opt-in, auto): when a session reports it runs under
# Ghostty (term=="ghostty" + a tty), the 3-light strip is ordered by Ghostty
# (window, tab, split) instead of seating order — i.e. by TAB position, with a
# fixed intra-tab order for splits. Built from Ghostty's AppleScript dictionary
# (tty is the stable join key; GHOSTTY_SURFACE_ID env does NOT map to it). The
# map is refreshed only while a Ghostty session exists, so non-Ghostty setups
# never invoke osascript and keep the original fixed-slot behavior untouched.
GHOSTTY_MAP: dict = {}          # "/dev/ttysNNN" -> (window_idx, tab_idx, split_idx)
GHOSTTY_MAP_PERIOD = 3.0        # seconds between tty->tab refreshes (while active)
_GHOSTTY_OSA = r'''
set out to ""
tell application "Ghostty"
  set wi to 0
  repeat with w in windows
    set wi to wi + 1
    set ti to 0
    repeat with t in tabs of w
      set ti to ti + 1
      set si to 0
      repeat with s in terminals of t
        set si to si + 1
        set out to out & (tty of s) & "\t" & wi & "\t" & ti & "\t" & si & "\n"
      end repeat
    end repeat
  end repeat
end tell
return out
'''


def _read_ghostty_map() -> dict:
    """tty -> (window_idx, tab_idx, split_idx) via Ghostty's scripting dictionary.
    Blocking (run in a thread). Returns {} on any failure — no Ghostty, an older
    Ghostty without the dictionary, or Automation permission not granted — so the
    caller transparently falls back to the non-Ghostty ordering."""
    try:
        r = subprocess.run(["osascript", "-e", _GHOSTTY_OSA],
                           capture_output=True, text=True, timeout=3.0)
    except Exception:
        return {}
    if r.returncode != 0:
        return {}
    m: dict = {}
    for ln in r.stdout.splitlines():
        f = ln.split("\t")
        if len(f) != 4 or not f[0].startswith("/dev/"):
            continue
        try:
            m[f[0]] = (int(f[1]), int(f[2]), int(f[3]))
        except ValueError:
            continue
    return m

# Pending permission requests: id -> {"writer", "tool", "hint"}. A hook connection
# stays open while its prompt is pending; the device's A/B decision (or the hook
# closing — keyboard answered / timeout) resolves it.
PENDING: dict[str, dict] = {}

# Pending AskUserQuestion relays: id -> {"writer", "header", "opts"}. Same
# request/response shape as PENDING — resolved by a device pick or hook close.
ASK: dict[str, dict] = {}

# Cumulative output tokens per session (from the hook's transcript read) → feeds
# the buddy's leveling / every-50K-tokens celebrate.
SESSION_TOKENS: dict[str, int] = {}

# Persistent per-day output-token ledger for the today/week readout.
# LEDGER: "YYYY-MM-DD" -> tokens that day. LEDGER_LAST: sid -> last-counted
# cumulative (first-sight latched so a resumed session doesn't dump its whole
# history into today). Persisted to tokens.json so week survives restarts.
TOKENS_FILE = os.path.join(SOCK_DIR, "tokens.json")
LEDGER: dict[str, int] = {}
LEDGER_LAST: dict[str, int] = {}


def _today() -> str:
    return time.strftime("%Y-%m-%d", time.localtime())


def ledger_load():
    try:
        with open(TOKENS_FILE) as f:
            d = json.load(f)
        LEDGER.update({k: int(v) for k, v in (d.get("days") or {}).items()})
        # Persisting per-session last-counted survives bridge restarts so
        # "today" doesn't reset (the dev pain we hit kickstarting the bridge).
        LEDGER_LAST.update({k: int(v) for k, v in (d.get("last") or {}).items()})
    except Exception:
        pass


def ledger_save():
    try:
        os.makedirs(SOCK_DIR, exist_ok=True)
        with open(TOKENS_FILE, "w") as f:
            json.dump({"days": LEDGER, "last": LEDGER_LAST}, f)
    except Exception:
        pass


def ledger_add(sid: str, cumulative: int):
    """Count this session's growth into today's bucket (first-sight latched)."""
    prev = LEDGER_LAST.get(sid)
    LEDGER_LAST[sid] = cumulative
    if prev is None or cumulative < prev:
        ledger_save()                # persist the latch so a restart resumes it
        return
    delta = cumulative - prev
    if delta <= 0:
        return
    LEDGER[_today()] = LEDGER.get(_today(), 0) + delta
    ledger_save()


def tokens_today() -> int:
    return LEDGER.get(_today(), 0)


def tokens_lifetime() -> int:
    # Sum of every day ever recorded — a persistent, monotonic lifetime total
    # that drives (and restores) the pet's level even after a device erase.
    return sum(LEDGER.values())

# Latest /usage rate-limit snapshot from the statusLine (five_hour / seven_day
# used_percentage + reset epoch). Account-wide, not per-session.
USAGE: dict = {}

# One-shot flags merged into the very next device frame, then cleared:
#   completed → confetti celebrate (a turn just finished)
#   nudge     → gentle "awaiting your input" amber chime (idle_prompt)
ONESHOT: dict[str, bool] = {}


def now() -> float:
    return time.monotonic()


def touch(sid: str) -> dict:
    s = SESSIONS.get(sid)
    if s is None:
        s = {"state": "idle", "label": "", "seen": now(), "awaiting": False, "asking": False}
        SESSIONS[sid] = s
    s["seen"] = now()
    return s


def prune():
    cut = now() - SESSION_TTL
    for sid in [k for k, v in SESSIONS.items() if v["seen"] < cut]:
        del SESSIONS[sid]


def apply_event(ev: dict):
    """Update the session registry from a hook event."""
    sid = ev.get("sid") or "?"
    evt = ev.get("evt")
    if evt == "usage":
        USAGE.update({k: ev.get(k) for k in ("s5", "s5_reset", "w7", "w7_reset")})
        return
    if evt == "end":
        SESSIONS.pop(sid, None)
        SESSION_TOKENS.pop(sid, None)
        return
    s = touch(sid)
    if "cwd" in ev:
        s["label"] = ev["cwd"]
    if ev.get("tty"):
        s["tty"] = ev["tty"]            # stable per-session key for Ghostty ordering
    if ev.get("term"):
        s["term"] = ev["term"]          # TERM_PROGRAM (e.g. "ghostty")
    if "tokens" in ev:
        SESSION_TOKENS[sid] = int(ev["tokens"])
        ledger_add(sid, int(ev["tokens"]))
    if evt == "start":
        s["state"] = "idle"
        s["awaiting"] = False
    elif evt == "run":
        s["state"] = "running"           # whole turn counts as busy (our redefine)
        s["awaiting"] = False            # you replied → clear the awaiting border
        s["asking"] = False              # you replied → clear any pending question
    elif evt == "idle":
        s["state"] = "idle"              # turn ended (border waits for idle_prompt)
        s["asking"] = bool(ev.get("asking"))  # heuristic: turn ended on a question
        ONESHOT["completed"] = True      # brief celebrate, like the desktop app
    elif evt == "notify":
        nt = ev.get("ntype", "")
        if nt == "idle_prompt":
            s["state"] = "idle"
            s["awaiting"] = True         # persistent: drives the amber border
            ONESHOT["nudge"] = True      # one-shot chime the moment it starts
        # permission_prompt handled in M3 (waiting + prompt relay)


def aggregate() -> dict:
    """Collapse the registry into the firmware's official schema."""
    prune()

    # Single per-session classification, so the aggregate counts (running/
    # waiting) and the per-session traffic-light strip can never disagree:
    #   perm (blocked on approval) > run (processing) > wait (awaiting your
    #   input) > idle. running counts "run"; waiting counts "wait" + "perm"
    #   (everything that needs you).
    # Red = there is an explicit prompt on the device awaiting you (a permission
    # request or an AskUserQuestion). A bare idle_prompt timeout (s["awaiting"])
    # is NOT explicit — it stays idle/yellow so parked sessions don't glow red.
    perm_sids = {p.get("sid") for p in PENDING.values() if p.get("sid")}
    ask_sids  = {a.get("sid") for a in ASK.values() if a.get("sid")}
    def classify(sid, s):
        if sid in perm_sids:          return "perm"   # permission prompt -> red
        if s["state"] == "running":   return "run"    # processing -> green
        if sid in ask_sids:           return "wait"   # on-screen question -> red
        if s.get("asking"):           return "wait"   # ended on a question (heuristic) -> red
        return "idle"                                  # idle / passive wait -> yellow
    states = {sid: classify(sid, s) for sid, s in SESSIONS.items()}

    total    = len(SESSIONS)
    running  = sum(1 for v in states.values() if v == "run")    # green
    waiting  = sum(1 for v in states.values() if v == "wait")   # yellow: awaiting input
    approval = sum(1 for v in states.values() if v == "perm")   # red: needs approval
    if total == 0:
        msg = "no sessions"
    elif running:
        lbl = next((s["label"] for sid, s in SESSIONS.items()
                    if states[sid] == "run" and s["label"]), "")
        msg = f"working: {lbl}" if lbl else f"{running} working"
    else:
        msg = "awaiting you"
    tokens = tokens_lifetime()   # persistent → drives & restores the pet level
    # Amber border only when you're truly free — ALL sessions idle (running==0).
    awaiting = (running == 0 and not PENDING
                and any(s.get("awaiting") for s in SESSIONS.values()))
    # Fixed-slot assignment with activity-based eviction: a session keeps its
    # slot while it stays in the 3-most-active set, but is evicted the moment a
    # more-active session needs in — so the strip always shows the top-3 active
    # without reshuffling on every event.
    for i in range(3):                                    # free slots gone idle/ended
        if SLOTS[i] is not None and SLOTS[i] not in SESSIONS:
            SLOTS[i] = None
    ranked = [sid for sid, _ in sorted(SESSIONS.items(),
                                       key=lambda kv: kv[1]["seen"], reverse=True)]
    top3 = ranked[:3]                                     # the 3 most-active sessions
    # Order the 3 shown sessions. If any maps to a Ghostty terminal, order ALL of
    # them by (window, tab, split) — tab order, with a fixed intra-tab order for
    # splits; unmapped sessions sort after, keeping their by-recency order (stable
    # sort). When the map is empty (no Ghostty session, or no permission) fall
    # back to the original fixed-slot eviction/seating so non-Ghostty is unaffected.
    gkeys = {sid: GHOSTTY_MAP.get(SESSIONS[sid].get("tty", "")) for sid in top3}
    if any(gkeys.values()):
        ordered = sorted(top3, key=lambda sid: (0,) + gkeys[sid] if gkeys[sid]
                         else (1, 0, 0, 0))
        for i in range(3):
            SLOTS[i] = ordered[i] if i < len(ordered) else None
    else:
        top = set(top3)
        for i in range(3):                                # evict the ones bumped out
            if SLOTS[i] is not None and SLOTS[i] not in top:
                SLOTS[i] = None
        slotted = {s for s in SLOTS if s is not None}
        for sid in top3:                                  # seat newcomers in free slots
            if sid not in slotted:
                for i in range(3):
                    if SLOTS[i] is None:
                        SLOTS[i] = sid
                        slotted.add(sid)
                        break
    sessions = [states[SLOTS[i]] if SLOTS[i] is not None else "none"
                for i in range(3)]

    frame = {"total": total, "running": running, "waiting": waiting,
             "approval": approval, "msg": msg, "tokens": tokens,
             "awaiting": awaiting, "tokens_today": tokens_today(),
             "sessions": sessions}
    if USAGE.get("s5") is not None or USAGE.get("w7") is not None:
        # Send remaining-seconds-to-reset (computed fresh) so the device needs
        # no clock of its own; -1 when unknown.
        now = int(time.time())
        def remain(ts):
            return max(0, int(ts) - now) if ts else -1
        frame["usage"] = {"s5": USAGE.get("s5", -1), "s5_in": remain(USAGE.get("s5_reset")),
                          "w7": USAGE.get("w7", -1), "w7_in": remain(USAGE.get("w7_reset"))}
    if PENDING:
        # Surface the oldest pending approval as the device's prompt screen.
        pid = next(iter(PENDING))
        p = PENDING[pid]
        frame["prompt"] = {"id": pid, "tool": p["tool"], "hint": p["hint"]}
        frame["msg"] = f"approve? {p['tool']}"
    if ASK:
        aid = next(iter(ASK))
        a = ASK[aid]
        frame["ask"] = {"id": aid, "header": a["header"],
                        "proj": a.get("proj", ""), "opts": a["opts"]}
        frame["msg"] = "question"
    return frame


# ── Socket server (hooks → bridge) ───────────────────────────────────────────
async def handle_conn(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    owned: set[str] = set()   # prompt ids this connection is waiting on
    try:
        async for raw in reader:
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            if ev.get("evt") == "prompt":
                pid = str(ev.get("id") or "")
                if pid:
                    PENDING[pid] = {"writer": writer,
                                    "sid": str(ev.get("sid") or ""),
                                    "tool": str(ev.get("tool", ""))[:18],
                                    "hint": str(ev.get("hint", ""))[:42]}
                    owned.add(pid)
                    print(f"[prompt] {ev.get('tool')} id={pid} -> awaiting device",
                          flush=True)
                # keep the connection OPEN; resolved by on_tx or by EOF below
            elif ev.get("evt") == "ask":
                aid = str(ev.get("id") or "")
                if aid:
                    opts = [str(o)[:21] for o in (ev.get("opts") or [])][:4]
                    ASK[aid] = {"writer": writer,
                                "sid": str(ev.get("sid") or ""),
                                "header": str(ev.get("header", ""))[:21],
                                "proj": str(ev.get("proj", ""))[:21],
                                "opts": opts}
                    owned.add(aid)
                    print(f"[ask] id={aid} opts={opts} -> awaiting device",
                          flush=True)
                # keep OPEN; resolved by device pick (on_tx) or EOF below
            else:
                apply_event(ev)
                print(f"[sock] {ev.get('evt')} sid={ev.get('sid')} -> {aggregate()}",
                      flush=True)
    except Exception:
        pass
    finally:
        # Hook closed before the device answered → keyboard answered or timed
        # out. Cancel any still-pending prompt so the device clears its screen.
        for pid in owned:
            if PENDING.pop(pid, None) is not None:
                print(f"[prompt] id={pid} cancelled (hook closed)", flush=True)
            if ASK.pop(pid, None) is not None:
                print(f"[ask] id={pid} cancelled (hook closed)", flush=True)
        try:
            writer.close()
        except Exception:
            pass


async def socket_server():
    os.makedirs(SOCK_DIR, exist_ok=True)
    if os.path.exists(SOCK_PATH):
        os.unlink(SOCK_PATH)
    server = await asyncio.start_unix_server(handle_conn, path=SOCK_PATH)
    os.chmod(SOCK_PATH, 0o600)
    print(f"[bridge] socket listening at {SOCK_PATH}")
    async with server:
        await server.serve_forever()


# ── BLE push loop (bridge → device) ──────────────────────────────────────────
def on_tx(_char, data: bytearray):
    # Device → host. The buddy notifies a permission decision when you press
    # A (approve) or B (deny) on the approval screen.
    line = data.decode("utf-8", "replace").strip()
    if not line:
        return
    print(f"[ble] <- {line}", flush=True)
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        return
    if obj.get("cmd") == "permission":
        pid = str(obj.get("id") or "")
        decision = obj.get("decision")           # "once" | "always" | "deny"
        p = PENDING.pop(pid, None)
        if p is None:
            return
        reply = {"once": "allow", "always": "always"}.get(decision, "deny")
        try:
            p["writer"].write((json.dumps({"decision": reply}) + "\n").encode())
            p["writer"].close()                  # unblocks the waiting hook
            print(f"[prompt] id={pid} -> {reply} (from device)", flush=True)
        except Exception as e:
            print(f"[prompt] reply failed: {e}", flush=True)
    elif obj.get("cmd") == "ask":
        aid = str(obj.get("id") or "")
        idx = obj.get("index")
        a = ASK.pop(aid, None)
        if a is None:
            return
        # index 255 (the "terminal" escape row) → no answer, fall back to picker.
        payload = {"escape": True} if idx == 255 else {"index": int(idx)}
        try:
            a["writer"].write((json.dumps(payload) + "\n").encode())
            a["writer"].close()
            print(f"[ask] id={aid} -> {payload} (from device)", flush=True)
        except Exception as e:
            print(f"[ask] reply failed: {e}", flush=True)


async def find_dev():
    return await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or "").startswith(NAME_PREFIX)
        or NUS_SERVICE.lower() in [u.lower() for u in (ad.service_uuids or [])],
        timeout=10.0,
    )


def _restart(reason: str):
    # On macOS, bleak/CoreBluetooth often can't rediscover the device in-process
    # after the link drops (e.g. a firmware reflash reboots it) — it just spins on
    # "device not found" forever, while a brand-new process reconnects instantly.
    # So once an established link goes away, exit and let launchd (KeepAlive) spawn
    # a fresh process with a clean CoreBluetooth central. os._exit bypasses the
    # surrounding try/except (sys.exit would be swallowed by it).
    print(f"[ble] {reason}; exiting for a clean launchd restart", flush=True)
    os._exit(1)


async def ble_loop():
    quick_fail = 0   # consecutive connections that dropped almost immediately
    notfound = 0     # consecutive scans that found nothing
    while True:
        try:
            dev = await find_dev()
            if not dev:
                notfound += 1
                print(f"[ble] device not found; scanning... ({notfound})", flush=True)
                # Genuine CoreBluetooth scan wedge: nothing found for a long stretch
                # even though the device should be there -> a fresh process is the
                # only known cure (~5 min of 10s-scan + 3s-sleep).
                if notfound >= 24:
                    _restart("scan wedged (no device for too long)")
                await asyncio.sleep(3.0)
                continue
            notfound = 0
            print(f"[ble] found {dev.name} [{dev.address}], connecting...", flush=True)
            t0 = time.monotonic()
            async with BleakClient(dev) as client:
                print(f"[ble] connected={client.is_connected}", flush=True)
                try:
                    await client.start_notify(NUS_TX, on_tx)
                except Exception as e:
                    print(f"[ble] TX subscribe failed: {e}", flush=True)
                # Sync the clock once per connection so the device RTC is right.
                lt = time.localtime()
                tzoff = lt.tm_gmtoff if lt.tm_gmtoff is not None else 0
                ts = (json.dumps({"time": [int(time.time()), tzoff]}) + "\n").encode()
                try:
                    await client.write_gatt_char(NUS_RX, ts, response=True)
                except Exception:
                    pass
                while client.is_connected:
                    frame = aggregate()
                    if ONESHOT:                       # merge + clear one-shots
                        frame.update(ONESHOT)
                        ONESHOT.clear()
                    line = (json.dumps(frame) + "\n").encode()
                    try:
                        await client.write_gatt_char(NUS_RX, line, response=True)
                    except Exception as e:
                        print(f"[ble] write failed ({e})", flush=True)
                        break
                    await asyncio.sleep(PUSH_PERIOD)
            # Link ended. Reconnect IN-PROCESS — restarting doesn't fix a flaky link
            # and just thrashes launchd (we saw 70 restarts). Back off if it dropped
            # almost immediately so we don't tight-loop connect/drop.
            dur = time.monotonic() - t0
            if dur < 8:
                quick_fail += 1
                backoff = min(30.0, 2.0 ** quick_fail)
                print(f"[ble] link dropped after {dur:.1f}s (x{quick_fail}); retry in {backoff:.0f}s", flush=True)
                await asyncio.sleep(backoff)
            else:
                quick_fail = 0
                print(f"[ble] link dropped after {dur:.0f}s; reconnecting", flush=True)
                await asyncio.sleep(1.0)
        except Exception as e:
            print(f"[ble] link error: {e!r}; reconnecting in 3s", flush=True)
            await asyncio.sleep(3.0)


async def ghostty_map_loop():
    """Keep GHOSTTY_MAP fresh — but ONLY while at least one session reports it
    runs under Ghostty. With no Ghostty session, osascript is never invoked (no
    Automation prompt, zero behavior change for non-Ghostty users)."""
    global GHOSTTY_MAP
    last = None
    while True:
        ng = sum(1 for s in SESSIONS.values()
                 if s.get("term") == "ghostty" and s.get("tty"))
        if ng:
            GHOSTTY_MAP = await asyncio.to_thread(_read_ghostty_map)
        elif GHOSTTY_MAP:
            GHOSTTY_MAP = {}
        sig = (ng, len(GHOSTTY_MAP))
        if sig != last:
            print(f"[ghostty] sessions={ng} map={len(GHOSTTY_MAP)} {GHOSTTY_MAP}",
                  flush=True)
            last = sig
        await asyncio.sleep(GHOSTTY_MAP_PERIOD)


async def main():
    print("[bridge] starting (socket + BLE)")
    ledger_load()
    await asyncio.gather(socket_server(), ble_loop(), ghostty_map_loop())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[bridge] bye")
