#!/usr/bin/env python3
"""Qoder CLI hook client → buddy bridge.

Wired into every status hook (SessionStart, UserPromptSubmit, Stop,
Notification, SessionEnd, PreToolUse, PostToolUse, SubagentStop,
PermissionRequest). Reads the hook JSON on stdin, maps it to a compact
event, and posts one line to the bridge's Unix socket. Then exits 0.

FAIL-OPEN by design: if the bridge isn't running or the socket is gone, this
silently no-ops within a short timeout so Qoder CLI is never blocked or
slowed. Uses only the Python stdlib (no bleak) so it runs under system python3.

DATA MINIMIZATION: only the session id, a coarse event, the notification type,
and the cwd *basename* (project label) ever leave this process. No prompt text,
tool input, file contents, or transcript is sent.

Usage in ~/.qoder/settings.json:
    "hooks": {
      "SessionStart": [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /abs/path/bridge/hooks/qoder_hook.py"}]}],
      "UserPromptSubmit": [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /abs/path/bridge/hooks/qoder_hook.py"}]}],
      "Stop": [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /abs/path/bridge/hooks/qoder_hook.py"}]}],
      "Notification": [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /abs/path/bridge/hooks/qoder_hook.py"}]}],
      "SessionEnd": [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /abs/path/bridge/hooks/qoder_hook.py"}]}],
      "PreToolUse": [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /abs/path/bridge/hooks/qoder_hook.py"}]}],
      "PostToolUse": [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /abs/path/bridge/hooks/qoder_hook.py"}]}],
      "SubagentStop": [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /abs/path/bridge/hooks/qoder_hook.py"}]}],
      "PermissionRequest": [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /abs/path/bridge/hooks/qoder_hook.py", "timeout": 60}]}]
    }
"""
import json
import os
import re
import socket
import subprocess
import sys
import time

SOCK_PATH = os.path.expanduser("~/.claude-buddy/bridge.sock")


def current_tty() -> str:
    """The session's controlling TTY ('/dev/ttysNNN'), or '' if none."""
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


PERMISSION_WAIT = 40.0


def post(obj: dict):
    obj["src"] = "qoder"
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.4)
        s.connect(SOCK_PATH)
        s.sendall((json.dumps(obj) + "\n").encode())
        s.close()
    except Exception:
        pass


def last_assistant_is_question(path: str) -> bool:
    """Heuristic: does the final assistant message end with a question?"""
    if not path or not os.path.exists(path):
        return False
    last_text = ""
    try:
        with open(path, "r") as f:
            for line in f:
                try:
                    obj = json.loads(line)
                except Exception:
                    continue
                msg = obj.get("message") or {}
                if msg.get("role") != "assistant":
                    continue
                content = msg.get("content")
                if isinstance(content, str):
                    if content.strip():
                        last_text = content.strip()
                elif isinstance(content, list):
                    for b in content:
                        if isinstance(b, dict) and b.get("type") == "text":
                            t = (b.get("text") or "").strip()
                            if t:
                                last_text = t
    except Exception:
        return False
    if not last_text:
        return False
    tail = last_text[-160:].rstrip().rstrip('""\'）)】」』』 ')
    if tail.endswith("?") or tail.endswith("？"):
        return True
    return bool(re.search(
        r"(吗|呢|还是|哪个|哪一个|是否|要不要|好吗|可以吗|对吧|如何|怎么样)"
        r"[。.!！…]?\s*$", tail))


def count_output_tokens(path: str) -> int:
    """Sum assistant output tokens from the transcript JSONL."""
    if not path or not os.path.exists(path):
        return 0
    total = 0
    try:
        with open(path, "r") as f:
            for line in f:
                try:
                    obj = json.loads(line)
                except Exception:
                    continue
                usage = (obj.get("message") or {}).get("usage") or {}
                total += int(usage.get("output_tokens", 0) or 0)
    except Exception:
        return 0
    return total


def handle_ask(data: dict, tty: str = "", term: str = ""):
    """AskUserQuestion: fire a one-way heads-up to the bridge."""
    ti = data.get("tool_input", {}) or {}
    questions = ti.get("questions") or []
    if not questions:
        return
    q = (questions[0].get("question") or questions[0].get("header") or "")
    post({"evt": "asknote",
          "sid": data.get("session_id", ""),
          "cwd": os.path.basename(data.get("cwd", "") or ""),
          "tty": tty, "term": term,
          "q": q.replace("\n", " ").strip()[:48]})


def summarize(tool: str, ti) -> str:
    """A short, non-sensitive detail for the device."""
    if isinstance(ti, dict):
        for k in ("command", "file_path", "path", "url", "pattern", "query"):
            v = ti.get(k)
            if v:
                return str(v).replace("\n", " ")[:42]
    return tool[:42]


def request_decision(sid: str, tool: str, hint: str):
    """Relay a permission prompt to the device and wait for A/B."""
    pid = f"{(sid or '')[:8]}-{int(time.time() * 1000) % 1000000}"
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(PERMISSION_WAIT)
        s.connect(SOCK_PATH)
        s.sendall((json.dumps({"evt": "prompt", "sid": sid, "id": pid,
                               "tool": tool, "hint": hint}) + "\n").encode())
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

    if event == "SessionStart":
        post({"evt": "start", "sid": sid,
              "cwd": os.path.basename(data.get("cwd", "") or ""),
              "tty": tty, "term": term})
    elif event == "UserPromptSubmit":
        post({"evt": "run", "sid": sid,
              "cwd": os.path.basename(data.get("cwd", "") or ""),
              "tty": tty, "term": term,
              "tpath": data.get("transcript_path", "")})
    elif event == "Stop":
        tpath = data.get("transcript_path", "")
        post({"evt": "idle", "sid": sid, "tty": tty, "term": term,
              "tokens": count_output_tokens(tpath),
              "asking": last_assistant_is_question(tpath)})
    elif event == "Notification":
        post({"evt": "notify", "sid": sid, "tty": tty, "term": term,
              "ntype": data.get("notification_type", "")})
    elif event == "SessionEnd":
        post({"evt": "end", "sid": sid})
    elif event == "PreToolUse":
        if data.get("tool_name") == "AskUserQuestion":
            handle_ask(data, tty, term)
        else:
            post({"evt": "tick", "sid": sid, "tty": tty, "term": term})
        sys.exit(0)
    elif event in ("PostToolUse", "SubagentStop"):
        post({"evt": "live", "sid": sid, "tty": tty, "term": term})
        sys.exit(0)
    elif event == "PermissionRequest":
        tool = data.get("tool_name", "")
        if tool == "AskUserQuestion":
            sys.exit(0)
        hint = summarize(tool, data.get("tool_input", {}))
        decision = request_decision(sid, tool, hint)
        if decision == "deny":
            print(json.dumps({"hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "behavior": "deny"}}))
        elif decision == "always":
            print(json.dumps({"hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "behavior": "allow",
                "updatedPermissions": [{"tool": tool, "permission": "allow"}]}}))
        elif decision == "allow":
            print(json.dumps({"hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "behavior": "allow"}}))
        sys.exit(0)

    sys.exit(0)


if __name__ == "__main__":
    main()
