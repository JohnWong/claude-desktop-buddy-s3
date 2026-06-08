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

# Cumulative output tokens per session (from the hook's transcript read) → feeds
# the buddy's leveling / every-50K-tokens celebrate.
SESSION_TOKENS: dict[str, int] = {}

# One-shot flags merged into the very next device frame, then cleared:
#   completed → confetti celebrate (a turn just finished)
#   nudge     → gentle "awaiting your input" amber chime (idle_prompt)
ONESHOT: dict[str, bool] = {}


def now() -> float:
    return time.monotonic()


def touch(sid: str) -> dict:
    s = SESSIONS.get(sid)
    if s is None:
        s = {"state": "idle", "label": "", "seen": now()}
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
    if evt == "end":
        SESSIONS.pop(sid, None)
        SESSION_TOKENS.pop(sid, None)
        return
    s = touch(sid)
    if "cwd" in ev:
        s["label"] = ev["cwd"]
    if "tokens" in ev:
        SESSION_TOKENS[sid] = int(ev["tokens"])
    if evt == "start":
        s["state"] = "idle"
    elif evt == "run":
        s["state"] = "running"           # whole turn counts as busy (our redefine)
    elif evt == "idle":
        s["state"] = "idle"              # turn ended → awaiting your input
        ONESHOT["completed"] = True      # brief celebrate, like the desktop app
    elif evt == "notify":
        nt = ev.get("ntype", "")
        if nt == "idle_prompt":
            s["state"] = "idle"
            ONESHOT["nudge"] = True      # gentle "awaiting your input" reminder
        # permission_prompt handled in M3 (waiting + prompt relay)


def aggregate() -> dict:
    """Collapse the registry into the firmware's official schema."""
    prune()
    total   = len(SESSIONS)
    running = sum(1 for s in SESSIONS.values() if s["state"] == "running")
    waiting = len(PENDING)   # firmware 'waiting' = blocked on a permission prompt
    if total == 0:
        msg = "no sessions"
    elif running:
        # non-sensitive label = project dir basename of a running session
        lbl = next((s["label"] for s in SESSIONS.values()
                    if s["state"] == "running" and s["label"]), "")
        msg = f"working: {lbl}" if lbl else f"{running} working"
    else:
        msg = "awaiting you"
    tokens = sum(SESSION_TOKENS.get(sid, 0) for sid in SESSIONS)
    frame = {"total": total, "running": running, "waiting": waiting,
             "msg": msg, "tokens": tokens}
    if PENDING:
        # Surface the oldest pending approval as the device's prompt screen.
        pid = next(iter(PENDING))
        p = PENDING[pid]
        frame["prompt"] = {"id": pid, "tool": p["tool"], "hint": p["hint"]}
        frame["msg"] = f"approve? {p['tool']}"
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
                                    "tool": str(ev.get("tool", ""))[:18],
                                    "hint": str(ev.get("hint", ""))[:42]}
                    owned.add(pid)
                    print(f"[prompt] {ev.get('tool')} id={pid} -> awaiting device",
                          flush=True)
                # keep the connection OPEN; resolved by on_tx or by EOF below
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
        decision = obj.get("decision")           # "once" (approve) | "deny"
        p = PENDING.pop(pid, None)
        if p is None:
            return
        reply = "allow" if decision == "once" else "deny"
        try:
            p["writer"].write((json.dumps({"decision": reply}) + "\n").encode())
            p["writer"].close()                  # unblocks the waiting hook
            print(f"[prompt] id={pid} -> {reply} (from device)", flush=True)
        except Exception as e:
            print(f"[prompt] reply failed: {e}", flush=True)


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
    await asyncio.gather(socket_server(), ble_loop())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[bridge] bye")
