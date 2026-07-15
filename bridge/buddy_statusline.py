#!/usr/bin/env python3
"""Claude Code statusLine → buddy bridge: relay /usage rate-limit data.

Claude Code >=2.1.x passes `rate_limits.five_hour` / `.seven_day`
(used_percentage + resets_at epoch) on the statusLine stdin for Pro/Max
subscribers. Hooks do NOT get this — only the statusLine does — so this small
command forwards it to the bridge socket and also prints a useful status line.

Wire it in settings.json:
    "statusLine": { "type": "command",
                    "command": "python3 /abs/path/bridge/buddy_statusline.py" }
"""
import json
import os
import socket
import sys
import time

SOCK_PATH = os.path.expanduser("~/.claude-buddy/bridge.sock")


def post(obj):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.3)
        s.connect(SOCK_PATH)
        s.sendall((json.dumps(obj) + "\n").encode())
        s.close()
    except Exception:
        pass  # fail-open: bridge down → just print the status line


def is_default_oauth():
    """True only for a vanilla Claude.ai OAuth session (Pro/Max).

    The device's usage gauge is meant to show the device owner's real Anthropic
    5h/7d quota. A session pointed at a relay/gateway (ANTHROPIC_BASE_URL away
    from api.anthropic.com) or authed with a token/API key reports a DIFFERENT
    identity whose rate_limits aren't that quota — and since the statusLine keeps
    firing on the TUI refresh timer even when the session is idle, such a session
    would otherwise clobber the device's gauge indefinitely. Gate those out: only
    the default OAuth identity may push usage."""
    if os.environ.get("ANTHROPIC_AUTH_TOKEN") or os.environ.get("ANTHROPIC_API_KEY"):
        return False
    base = (os.environ.get("ANTHROPIC_BASE_URL") or "").strip()
    if base and "api.anthropic.com" not in base:
        return False
    return True


def main():
    try:
        d = json.load(sys.stdin)
    except Exception:
        sys.exit(0)

    rl = d.get("rate_limits") or {}
    s5 = rl.get("five_hour") or {}
    w7 = rl.get("seven_day") or {}
    s5p = s5.get("used_percentage")
    w7p = w7.get("used_percentage")

    if (s5p is not None or w7p is not None) and is_default_oauth():
        post({"evt": "usage",
              "s5": s5p, "s5_reset": s5.get("resets_at"),
              "w7": w7p, "w7_reset": w7.get("resets_at")})

    # Print a compact, useful status line (the user had none before).
    def pct(v):
        # used_percentage arrives as a long float (e.g. 12.3456789); show 1dp,
        # and drop a trailing ".0" so whole numbers read cleanly.
        try:
            return f"{float(v):.1f}".rstrip("0").rstrip(".")
        except (TypeError, ValueError):
            return str(v)

    def reset_in(ts):
        # epoch → compact "Xh Ym" (or "Ym") until the limit resets; None if past/unknown.
        try:
            rem = int(ts) - int(time.time())
        except (TypeError, ValueError):
            return None
        if rem <= 0:
            return None
        h, m = divmod(rem // 60, 60)
        return f"{h}h{m:02d}m" if h else f"{m}m"

    model = (d.get("model") or {}).get("display_name") or (d.get("model") or {}).get("id", "")
    cwd = os.path.basename(d.get("cwd", "") or "")
    bits = []
    if model:
        bits.append(model)
    if s5p is not None:
        seg = f"5h {pct(s5p)}%"
        r = reset_in(s5.get("resets_at"))
        if r:
            seg += f" (↻{r})"
        bits.append(seg)
    if w7p is not None:
        bits.append(f"7d {pct(w7p)}%")
    if cwd:
        bits.append(cwd)
    sys.stdout.write("  ".join(bits))


if __name__ == "__main__":
    main()
