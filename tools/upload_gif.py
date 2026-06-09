#!/usr/bin/env python3
"""
Upload a GIF character to the StickS3 buddy over BLE.

The current firmware is built with ARDUINO_USB_MODE=1, so the USB-serial
command path is compiled out — uploads MUST go over BLE (Nordic UART
Service). Only one BLE central can be connected at a time, so STOP the
background bridge first:

    launchctl unload ~/Library/LaunchAgents/com.claude-buddy.bridge.plist
    # ... upload ...
    launchctl load   ~/Library/LaunchAgents/com.claude-buddy.bridge.plist

Two input modes:

  1) A single image/video — auto-converted and mapped to every state:
       upload_gif.py tom.webp --name tom        # animated webp/gif keep frames
       upload_gif.py clip.mp4 --name tom        # video → 12fps gif (needs ffmpeg)
       upload_gif.py ready.gif --name tom --raw # skip conversion

  2) A character directory — uploads files as-is (must contain
     manifest.json + the .gif files it references):
       upload_gif.py ./mychar/ --name mychar

Conversion (ImageMagick, auto unless --raw) does the critical step:
  • -coalesce so EVERY frame is full-canvas. The decoder has no disposal/
    delta handling, so an optimized/sub-rect GIF garbles or shows one frame.
  • fit to --box, flatten transparency onto black, reduce to <=255 colors.

Device constraints (see README):
  • Screen 135x240. Home view centers the GIF in the top 140px, so keep it
    <=135 wide and <=140 tall (square ~120x120 looks best). Bigger is cropped.
  • Filesystem budget ~1.9 MB for the whole character (all states + manifest).

Persona states (a single GIF fills all 7):
  sleep idle busy attention celebrate dizzy heart
"""
import argparse
import asyncio
import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("bleak not installed: ~/.pio-venv/bin/pip install bleak")

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # client -> stick (write)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # stick -> client (notify)
NAME_PREFIX = "Claude-"

STATES = ["sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"]

# Keep each JSON command line under one BLE notify/MTU so writes land intact.
CHUNK = 96
FS_BUDGET = 1_900_000  # spiffs partition is 0x1E0000; leave headroom


# ───────────────────────── staging ─────────────────────────

VIDEO_EXT = (".mp4", ".mov", ".m4v", ".webm", ".avi", ".mkv")


def prep_gif(src: str, dst: str, box: str) -> None:
    """Produce a device-ready GIF: every frame full-canvas (coalesced — the
    decoder has no disposal/delta handling), fit inside `box`, transparency
    flattened onto black, <=255 colors. Animated webp/gif keep all frames;
    video is first rendered to frames via ffmpeg. Uses ImageMagick."""
    if not shutil.which("magick"):
        sys.exit("prep needs ImageMagick (brew install imagemagick)")
    img = src
    tmp = None
    if src.lower().endswith(VIDEO_EXT):
        if not shutil.which("ffmpeg"):
            sys.exit("video input needs ffmpeg (brew install ffmpeg)")
        w = box.split("x")[0]
        tmp = dst + ".src.gif"
        subprocess.run(["ffmpeg", "-y", "-i", src, "-vf",
                        f"fps=12,scale={w}:-1:flags=lanczos", tmp],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        img = tmp
    cmd = ["magick", img, "-coalesce", "-resize", box,
           "-background", "black", "-alpha", "remove", "-alpha", "off",
           "+repage", "-colors", "255", "-loop", "0", dst]
    print(f"  prep: {' '.join(cmd)}")
    subprocess.run(cmd, check=True)
    if tmp and os.path.exists(tmp):
        os.remove(tmp)
    n = subprocess.run(["magick", "identify", dst], capture_output=True, text=True)
    print(f"  -> {len(n.stdout.strip().splitlines())} frame(s), "
          f"{os.path.getsize(dst)/1024:.1f} KB")


def stage_single(gif: str, name: str, raw: bool, box: str) -> str:
    """Build a temp char dir from one source: manifest maps all states to it."""
    d = tempfile.mkdtemp(prefix="buddy_char_")
    out = os.path.join(d, "idle.gif")
    if raw:
        shutil.copy(gif, out)   # already device-ready full-frame gif
    else:
        prep_gif(gif, out, box)
    manifest = {
        "name": name,
        "colors": {"bg": "#000000"},
        "states": {s: "idle.gif" for s in STATES},
    }
    with open(os.path.join(d, "manifest.json"), "w") as f:
        json.dump(manifest, f)
    return d


def validate_dir(d: str) -> list:
    mpath = os.path.join(d, "manifest.json")
    if not os.path.exists(mpath):
        sys.exit(f"{d}: no manifest.json")
    manifest = json.load(open(mpath))
    files = sorted(
        os.path.join(d, f) for f in os.listdir(d)
        if os.path.isfile(os.path.join(d, f))
    )
    # Sanity: every gif referenced in states must exist on disk.
    refs = set()
    for v in (manifest.get("states") or {}).values():
        for r in (v if isinstance(v, list) else [v]):
            if r:
                refs.add(r)
    missing = [r for r in refs if not os.path.exists(os.path.join(d, r))]
    if missing:
        sys.exit(f"manifest references missing files: {missing}")
    return files


# ───────────────────────── BLE transport ─────────────────────────

class Link:
    def __init__(self, client: BleakClient):
        self.client = client
        self._buf = b""
        self._acks = asyncio.Queue()

    def _on_tx(self, _c, data: bytearray):
        self._buf += bytes(data)
        while b"\n" in self._buf:
            line, self._buf = self._buf.split(b"\n", 1)
            line = line.strip()
            if line.startswith(b"{"):
                try:
                    self._acks.put_nowait(json.loads(line))
                except Exception:
                    pass

    async def start(self):
        await self.client.start_notify(NUS_TX, self._on_tx)

    async def send(self, obj: dict):
        line = (json.dumps(obj) + "\n").encode()
        await self.client.write_gatt_char(NUS_RX, line, response=False)

    async def wait_ack(self, what: str, timeout=8.0):
        loop = asyncio.get_event_loop()
        deadline = loop.time() + timeout
        while True:
            remain = deadline - loop.time()
            if remain <= 0:
                return None
            try:
                a = await asyncio.wait_for(self._acks.get(), timeout=remain)
            except asyncio.TimeoutError:
                return None
            if a.get("ack") == what:
                return a

    async def cmd(self, obj: dict, ack: str, timeout=8.0, retries=1):
        for _ in range(retries + 1):
            await self.send(obj)
            a = await self.wait_ack(ack, timeout)
            if a is not None:
                return a
        return None


async def send_file(link: "Link", name: str, path: str) -> bool:
    data = open(path, "rb").read()
    print(f"  {name}: {len(data)} bytes ", end="", flush=True)
    a = await link.cmd({"cmd": "file", "path": name, "size": len(data)}, "file")
    if not a or not a.get("ok"):
        print("— open FAILED")
        return False
    for i in range(0, len(data), CHUNK):
        b64 = base64.b64encode(data[i:i + CHUNK]).decode()
        a = await link.cmd({"cmd": "chunk", "d": b64}, "chunk", timeout=6.0, retries=2)
        if not a or not a.get("ok"):
            print(f"— chunk @{i} FAILED")
            return False
        if i and i % 8192 == 0:
            print(".", end="", flush=True)
    a = await link.cmd({"cmd": "file_end"}, "file_end", timeout=10.0)
    ok = bool(a and a.get("ok") and a.get("n") == len(data))
    print(f"— {'ok' if ok else 'FAILED'}")
    return ok


async def find_dev():
    return await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or "").startswith(NAME_PREFIX)
        or NUS_SERVICE.lower() in [u.lower() for u in (ad.service_uuids or [])],
        timeout=12.0,
    )


async def run(files: list, name: str):
    total = sum(os.path.getsize(f) for f in files)
    if total + 4096 > FS_BUDGET:
        sys.exit(f"character is {total/1024:.0f}K — over the ~1.9MB budget. "
                 f"Shrink it (--prep --lossy).")
    print(f"{len(files)} files, {total/1024:.1f} KB total")

    print("scanning for the buddy (is the bridge stopped?)...", flush=True)
    dev = await find_dev()
    if not dev:
        sys.exit("no buddy found. Stop the bridge and make sure BLE is on.")
    print(f"connecting to {dev.name or dev.address}...")
    async with BleakClient(dev) as client:
        link = Link(client)
        await link.start()

        a = await link.cmd({"cmd": "char_begin", "name": name}, "char_begin",
                            timeout=4.0, retries=6)
        if not a or not a.get("ok"):
            sys.exit(f"char_begin failed: {a}")
        print("char_begin ok — streaming...")

        import time
        t0 = time.monotonic()
        for f in files:
            if not await send_file(link, os.path.basename(f), f):
                sys.exit("transfer aborted")
        a = await link.cmd({"cmd": "char_end"}, "char_end", timeout=12.0)
        dt = time.monotonic() - t0
        if not a or not a.get("ok"):
            sys.exit(f"char_end failed (manifest invalid?): {a}")
        print(f"\ndone: '{name}' installed in {dt:.0f}s "
              f"({total/dt/1024:.1f} KB/s). The buddy switched to GIF mode.")


SRC_EXT = (".gif", ".webp", ".png", ".jpg", ".jpeg", ".apng") + VIDEO_EXT


def main():
    ap = argparse.ArgumentParser(
        description="Upload a buddy character over BLE. A single image/video is "
                    "auto-converted to a device-ready GIF (frames coalesced, "
                    "fit to the screen). Animated webp/gif keep their frames.")
    ap.add_argument("path", help="a single image/video, or a character directory")
    ap.add_argument("--name", help="character name on device (default: file stem)")
    ap.add_argument("--box", default="120x120", help="fit box, WxH (default 120x120)")
    ap.add_argument("--raw", action="store_true",
                    help="skip conversion — input is already a full-frame GIF")
    args = ap.parse_args()

    name = args.name or os.path.splitext(os.path.basename(args.path.rstrip("/")))[0]
    name = "".join(c for c in name if c.isalnum() or c in "-_")[:20] or "pet"

    if os.path.isdir(args.path):
        files = validate_dir(args.path)
        staged = None
    elif args.path.lower().endswith(SRC_EXT):
        staged = stage_single(args.path, name, args.raw, args.box)
        files = sorted(os.path.join(staged, f) for f in os.listdir(staged))
    else:
        sys.exit("path must be an image/video file or a directory with manifest.json")

    try:
        asyncio.run(run(files, name))
    finally:
        if staged:
            shutil.rmtree(staged, ignore_errors=True)


if __name__ == "__main__":
    main()
