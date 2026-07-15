#!/usr/bin/env python3
"""Codex CLI hook client -> buddy bridge.

Wire into ~/.codex/hooks.json or ~/.codex/config.toml. The hook sends compact,
local-only session state to bridge/buddy_bridge.py:
  - session lifecycle: idle / running / waiting
  - permission prompts: device A/B can allow/deny
  - Codex token_count transcript events: output tokens + rate-limit percentage

Fail-open: if the bridge is down, Codex continues normally.
"""
import json
import os
import re
import socket
import subprocess
import sys
import time

SOCK_PATH = os.path.expanduser("~/.claude-buddy/bridge.sock")
PERMISSION_WAIT = 40.0


def current_tty() -> str:
    try:
        out = subprocess.run(["ps", "-Ao", "pid=,ppid=,tty="],
                             capture_output=True, text=True, timeout=1.0).stdout
    except Exception:
        return ""
    pp, tt = {}, {}
    for ln in out.splitlines():
        f = ln.split(None, 2)
        if len(f) == 3:
            pp[f[0]], tt[f[0]] = f[1], f[2].strip()
    pid = str(os.getpid())
    for _ in range(16):
        dev = tt.get(pid, "")
        if dev.startswith("ttys"):
            return "/dev/" + dev
        pid = pp.get(pid, "")
        if not pid or pid == "0":
            break
    return ""


def post(obj: dict):
    obj["src"] = "codex"
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.4)
        s.connect(SOCK_PATH)
        s.sendall((json.dumps(obj) + "\n").encode())
        s.close()
    except Exception:
        pass


def last_assistant_is_question(data: dict) -> bool:
    last_text = (data.get("last_assistant_message") or "").strip()
    if not last_text:
        return False
    tail = last_text[-160:].rstrip().rstrip('""\'）)】」』』 ')
    if tail.endswith("?") or tail.endswith("？"):
        return True
    return bool(re.search(
        r"(吗|呢|还是|哪个|哪一个|是否|要不要|好吗|可以吗|对吧|如何|怎么样)"
        r"[。.!！…]?\s*$", tail))


def transcript_stats(path: str) -> dict:
    """Return latest cumulative output tokens and rate-limit snapshot.

    Codex transcript JSONL has event_msg/token_count records with:
      payload.info.total_token_usage.output_tokens
      payload.rate_limits.primary.used_percent/window_minutes/resets_at
    The transcript format is documented as unstable, so this is defensive and
    best-effort.
    """
    out = {"tokens": None, "usage": {}}
    if not path or not os.path.exists(path):
        return out
    try:
        with open(path, "r") as f:
            for line in f:
                try:
                    obj = json.loads(line)
                except Exception:
                    continue
                pl = obj.get("payload") or {}
                if not isinstance(pl, dict) or pl.get("type") != "token_count":
                    continue
                usage = ((pl.get("info") or {}).get("total_token_usage") or {})
                if "output_tokens" in usage:
                    try:
                        out["tokens"] = int(usage.get("output_tokens") or 0)
                    except (TypeError, ValueError):
                        pass
                rl = pl.get("rate_limits") or {}
                for bucket in ("primary", "secondary"):
                    b = rl.get(bucket) or {}
                    pct = b.get("used_percent")
                    reset = b.get("resets_at")
                    mins = b.get("window_minutes")
                    if pct is None:
                        continue
                    try:
                        mins_i = int(mins)
                    except (TypeError, ValueError):
                        mins_i = 0
                    if mins_i == 300:
                        out["usage"].update({"s5": pct, "s5_reset": reset})
                    elif mins_i == 10080:
                        out["usage"].update({"w7": pct, "w7_reset": reset})
    except Exception:
        pass
    return out


def summarize(tool: str, ti) -> str:
    if isinstance(ti, dict):
        for k in ("description", "command", "file_path", "path", "url", "pattern", "query"):
            v = ti.get(k)
            if v:
                return str(v).replace("\n", " ")[:42]
    return tool[:42]


def request_decision(sid: str, tool: str, hint: str):
    pid = f"{(sid or '')[:8]}-{int(time.time() * 1000) % 1000000}"
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(PERMISSION_WAIT)
        s.connect(SOCK_PATH)
        s.sendall((json.dumps({"src": "codex", "evt": "prompt", "sid": sid,
                               "id": pid, "tool": tool, "hint": hint}) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(256)
            if not chunk:
                return None
            buf += chunk
        s.close()
        obj = json.loads(buf.split(b"\n", 1)[0].decode())
        return obj.get("decision")
    except Exception:
        return None


def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        sys.exit(0)

    sid = data.get("session_id", "?")
    event = data.get("hook_event_name", "")
    tty = current_tty()
    term = os.environ.get("TERM_PROGRAM", "")
    cwd = os.path.basename(data.get("cwd", "") or "")

    if event == "SessionStart":
        post({"evt": "start", "sid": sid, "cwd": cwd, "tty": tty, "term": term})
    elif event == "UserPromptSubmit":
        post({"evt": "run", "sid": sid, "cwd": cwd, "tty": tty, "term": term})
    elif event == "PreToolUse":
        post({"evt": "tick", "sid": sid, "tty": tty, "term": term})
    elif event in ("PostToolUse", "SubagentStop"):
        stats = transcript_stats(data.get("transcript_path", ""))
        ev = {"evt": "live", "sid": sid, "tty": tty, "term": term}
        if stats["tokens"] is not None:
            ev["tokens"] = stats["tokens"]
        post(ev)
        if stats["usage"]:
            post({"evt": "usage", **stats["usage"]})
    elif event == "Stop":
        stats = transcript_stats(data.get("transcript_path", ""))
        ev = {"evt": "idle", "sid": sid, "tty": tty, "term": term,
              "asking": last_assistant_is_question(data)}
        if stats["tokens"] is not None:
            ev["tokens"] = stats["tokens"]
        post(ev)
        if stats["usage"]:
            post({"evt": "usage", **stats["usage"]})
    elif event == "PermissionRequest":
        tool = data.get("tool_name", "")
        hint = summarize(tool, data.get("tool_input", {}))
        decision = request_decision(sid, tool, hint)
        if decision in ("allow", "deny"):
            print(json.dumps({"hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "decision": {"behavior": decision}}}))
        sys.exit(0)

    sys.exit(0)


if __name__ == "__main__":
    main()
