#!/usr/bin/env python3
"""Claude Code hook client → buddy bridge (M2).

Wired into every status hook (SessionStart, UserPromptSubmit, Stop,
Notification, SessionEnd). Reads the hook JSON on stdin, maps it to a compact
event, and posts one line to the bridge's Unix socket. Then exits 0.

FAIL-OPEN by design: if the bridge isn't running or the socket is gone, this
silently no-ops within a short timeout so Claude Code is never blocked or
slowed. Uses only the Python stdlib (no bleak) so it runs under system python3.

DATA MINIMIZATION: only the session id, a coarse event, the notification type,
and the cwd *basename* (project label) ever leave this process. No prompt text,
tool input, file contents, or transcript is sent.

Usage in settings.json (command):
    python3 /abs/path/bridge/hooks/buddy_hook.py
"""
import json
import os
import socket
import sys

SOCK_PATH = os.path.expanduser("~/.claude-buddy/bridge.sock")


def post(obj: dict):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.4)
        s.connect(SOCK_PATH)
        s.sendall((json.dumps(obj) + "\n").encode())
        s.close()
    except Exception:
        pass  # fail-open: bridge down → no-op


def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        sys.exit(0)

    sid = data.get("session_id", "?")
    event = data.get("hook_event_name", "")

    if event == "SessionStart":
        post({"evt": "start", "sid": sid,
              "cwd": os.path.basename(data.get("cwd", "") or "")})
    elif event == "UserPromptSubmit":
        post({"evt": "run", "sid": sid,
              "cwd": os.path.basename(data.get("cwd", "") or "")})
    elif event == "Stop":
        post({"evt": "idle", "sid": sid})
    elif event == "Notification":
        post({"evt": "notify", "sid": sid,
              "ntype": data.get("notification_type", "")})
    elif event == "SessionEnd":
        post({"evt": "end", "sid": sid})

    sys.exit(0)  # never block Claude Code


if __name__ == "__main__":
    main()
