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
        return
    s = touch(sid)
    if "cwd" in ev:
        s["label"] = ev["cwd"]
    if evt == "start":
        s["state"] = "idle"
    elif evt == "run":
        s["state"] = "running"           # whole turn counts as busy (our redefine)
    elif evt == "idle":
        s["state"] = "idle"              # turn ended → awaiting your input
    elif evt == "notify":
        nt = ev.get("ntype", "")
        if nt == "idle_prompt":
            s["state"] = "idle"          # Claude is idle, waiting for you
        # permission_prompt handled in M3 (waiting + prompt relay)


def aggregate() -> dict:
    """Collapse the registry into the firmware's official schema."""
    prune()
    total   = len(SESSIONS)
    running = sum(1 for s in SESSIONS.values() if s["state"] == "running")
    waiting = sum(1 for s in SESSIONS.values() if s["state"] == "waiting")
    if total == 0:
        msg = "no sessions"
    elif running:
        # non-sensitive label = project dir basename of a running session
        lbl = next((s["label"] for s in SESSIONS.values()
                    if s["state"] == "running" and s["label"]), "")
        msg = f"working: {lbl}" if lbl else f"{running} working"
    else:
        msg = "awaiting you"
    return {"total": total, "running": running, "waiting": waiting, "msg": msg}


# ── Socket server (hooks → bridge) ───────────────────────────────────────────
async def handle_conn(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    try:
        async for raw in reader:
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            apply_event(ev)
            print(f"[sock] {ev.get('evt')} sid={ev.get('sid')} -> {aggregate()}",
                  flush=True)
    except Exception:
        pass
    finally:
        writer.close()


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
    # Device → host (button decisions, etc.) — used in M3.
    line = data.decode("utf-8", "replace").strip()
    if line:
        print(f"[ble] <- {line}")


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
                while client.is_connected:
                    frame = aggregate()
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
