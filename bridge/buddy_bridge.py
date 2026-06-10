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
        s = {"state": "idle", "label": "", "seen": now(), "awaiting": False}
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
    if "tokens" in ev:
        SESSION_TOKENS[sid] = int(ev["tokens"])
        ledger_add(sid, int(ev["tokens"]))
    if evt == "start":
        s["state"] = "idle"
        s["awaiting"] = False
    elif evt == "run":
        s["state"] = "running"           # whole turn counts as busy (our redefine)
        s["awaiting"] = False            # you replied → clear the awaiting border
    elif evt == "idle":
        s["state"] = "idle"              # turn ended (border waits for idle_prompt)
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
    perm_sids = {p.get("sid") for p in PENDING.values() if p.get("sid")}
    def classify(sid, s):
        if sid in perm_sids:          return "perm"
        if s["state"] == "running":   return "run"
        if s.get("awaiting"):         return "wait"
        return "idle"
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
    # The 3 most-recently-seen sessions, reusing the same classification.
    recent = sorted(SESSIONS.items(), key=lambda kv: kv[1]["seen"], reverse=True)[:3]
    sessions = [states[sid] for sid, _ in recent]

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


async def ble_loop():
    while True:
        try:
            dev = await find_dev()
            if not dev:
                print("[ble] device not found; retrying...")
                await asyncio.sleep(3.0)
                continue
            print(f"[ble] found {dev.name} [{dev.address}], connecting...")
            async with BleakClient(dev) as client:
                print(f"[ble] connected={client.is_connected}")
                try:
                    await client.start_notify(NUS_TX, on_tx)
                except Exception as e:
                    print(f"[ble] TX subscribe failed: {e}")
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
                        print(f"[ble] write failed ({e}); reconnecting")
                        break
                    await asyncio.sleep(PUSH_PERIOD)
        except Exception as e:
            print(f"[ble] link error: {e!r}; reconnecting in 2s")
            await asyncio.sleep(2.0)


async def main():
    print("[bridge] starting (socket + BLE)")
    ledger_load()
    await asyncio.gather(socket_server(), ble_loop())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[bridge] bye")
