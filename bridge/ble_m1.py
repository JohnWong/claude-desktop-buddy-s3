#!/usr/bin/env python3
"""Phase 2 / M1 — prove the CLI-side BLE path works without the desktop app.

Scans for the StickS3 buddy (advertises as "Claude-XXXX"), connects over the
Nordic UART Service, subscribes to TX (so we can see button presses), and
pushes a few hardcoded state frames to RX so the device displays them.

If it works, the device should show total=3 and the msg text, with the link
page reporting "bt" — all driven from the terminal, no desktop app involved.

Run:  ~/.pio-venv/bin/python bridge/ble_m1.py
"""
import asyncio
import json
import sys

from bleak import BleakClient, BleakScanner

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # client -> stick (write)
NUS_TX      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # stick -> client (notify)

NAME_PREFIX = "Claude-"


def on_tx(_char, data: bytearray):
    # Button decisions / events the stick sends back.
    print(f"  <- from stick: {data!r}")


async def send_line(client: BleakClient, obj: dict):
    line = (json.dumps(obj) + "\n").encode()
    # response=True → we get an ACK/error, so a successful return proves the
    # encrypted write actually landed (not silently dropped).
    await client.write_gatt_char(NUS_RX, line, response=True)
    print(f"  -> sent (acked): {obj}")


FRAMES = [
    {"total": 3, "running": 1, "waiting": 0, "msg": "CLI bridge: hello"},
    {"total": 3, "running": 0, "waiting": 0, "msg": "idle / awaiting you"},
    {"total": 3, "running": 2, "waiting": 0, "msg": "2 sessions working"},
]


async def find_dev():
    return await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or "").startswith(NAME_PREFIX)
        or NUS_SERVICE.lower() in [u.lower() for u in (ad.service_uuids or [])],
        timeout=10.0,
    )


async def main():
    import time
    deadline = time.monotonic() + 120.0   # keep trying/pushing for 2 min total
    i = 0
    print(">>> WATCH THE DEVICE — auto-reconnect, pushing a frame every 3s for 2 min")
    while time.monotonic() < deadline:
        print(f"scanning for '{NAME_PREFIX}*'...")
        dev = await find_dev()
        if not dev:
            print("not found; make sure it's awake + desktop app NOT connected. retrying...")
            continue
        print(f"found: {dev.name} [{dev.address}]")
        try:
            def on_disc(_c):
                print("  !! link dropped — will reconnect")
            async with BleakClient(dev, disconnected_callback=on_disc) as client:
                print(f"connected: {client.is_connected}")
                try:
                    await client.start_notify(NUS_TX, on_tx)
                    print("subscribed to TX")
                except Exception as e:
                    print(f"(TX subscribe failed: {e})")
                while client.is_connected and time.monotonic() < deadline:
                    await send_line(client, FRAMES[i % len(FRAMES)])
                    i += 1
                    await asyncio.sleep(3.0)
        except Exception as e:
            print(f"  connection error: {e!r} — reconnecting...")
            await asyncio.sleep(1.0)

    print("done.")


if __name__ == "__main__":
    asyncio.run(main())
