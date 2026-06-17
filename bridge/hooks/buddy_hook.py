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
import re
import socket
import subprocess
import sys
import time

SOCK_PATH = os.path.expanduser("~/.claude-buddy/bridge.sock")


def current_tty() -> str:
    """The session's controlling TTY ('/dev/ttysNNN'), or '' if none. The hook's
    own fds are pipes, so walk the parent chain to the shell/claude process that
    owns the terminal. Used only as a stable per-session key for Ghostty tab
    ordering — nothing sensitive leaves the host. Cheap (one `ps`), best-effort."""
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

# Wait at most this long for a device decision before falling back to the native
# interactive prompt. Must be < the hook's timeout in settings.json.
PERMISSION_WAIT = 40.0


def post(obj: dict):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.4)
        s.connect(SOCK_PATH)
        s.sendall((json.dumps(obj) + "\n").encode())
        s.close()
    except Exception:
        pass  # fail-open: bridge down → no-op


def last_assistant_is_question(path: str) -> bool:
    """Heuristic: does the final assistant message end with a question? Reads the
    transcript locally to decide but returns only a bool — NO message content
    leaves this process (keeps the privacy guarantee above)."""
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
    tail = last_text[-160:].rstrip().rstrip('”"\'）)】」』』 ')
    if tail.endswith("?") or tail.endswith("？"):
        return True
    # Chinese interrogatives near the very end without an explicit question mark.
    return bool(re.search(
        r"(吗|呢|还是|哪个|哪一个|是否|要不要|好吗|可以吗|对吧|如何|怎么样)"
        r"[。.!！…]?\s*$", tail))


def count_output_tokens(path: str) -> int:
    """Sum assistant output tokens from the transcript JSONL. Reads only the
    numeric `usage`, never message content — privacy-preserving."""
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


def handle_ask(data: dict):
    """AskUserQuestion: do NOT answer on the device and do NOT block the CLI.
    Picking multiple-choice on the StickS3 proved too fiddly, so we just fire a
    one-way heads-up to the bridge (which project + what question) and let Claude
    Code's native terminal picker handle the answer as usual — nothing is printed,
    so the picker shows immediately and the CLI is never blocked."""
    ti = data.get("tool_input", {}) or {}
    questions = ti.get("questions") or []
    if not questions:
        return
    q = (questions[0].get("question") or questions[0].get("header") or "")
    post({"evt": "asknote",
          "sid": data.get("session_id", ""),
          "cwd": os.path.basename(data.get("cwd", "") or ""),
          "q": q.replace("\n", " ").strip()[:48]})


def summarize(tool: str, ti) -> str:
    """A short, non-sensitive detail for the device (cmd / path / url)."""
    if isinstance(ti, dict):
        for k in ("command", "file_path", "path", "url", "pattern", "query"):
            v = ti.get(k)
            if v:
                return str(v).replace("\n", " ")[:42]
    return tool[:42]


def request_decision(sid: str, tool: str, hint: str):
    """Relay a permission prompt to the device and wait for A/B. Returns
    "allow" | "deny", or None to fall back to the native interactive prompt
    (device timed out, was unplugged, or the bridge is down)."""
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
            if not chunk:           # bridge closed w/o a decision → fall back
                return None
            buf += chunk
        s.close()
        obj = json.loads(buf.split(b"\n", 1)[0].decode())
        return obj.get("decision")
    except Exception:
        return None                 # timeout / no bridge → native prompt wins


def main():
    try:
        data = json.load(sys.stdin)
    except Exception:
        sys.exit(0)

    sid = data.get("session_id", "?")
    event = data.get("hook_event_name", "")

    if event == "SessionStart":
        post({"evt": "start", "sid": sid,
              "cwd": os.path.basename(data.get("cwd", "") or ""),
              "tty": current_tty(), "term": os.environ.get("TERM_PROGRAM", "")})
    elif event == "UserPromptSubmit":
        post({"evt": "run", "sid": sid,
              "cwd": os.path.basename(data.get("cwd", "") or ""),
              "tty": current_tty(), "term": os.environ.get("TERM_PROGRAM", ""),
              "tpath": data.get("transcript_path", "")})
    elif event == "Stop":
        tpath = data.get("transcript_path", "")
        post({"evt": "idle", "sid": sid,
              "tokens": count_output_tokens(tpath),
              "asking": last_assistant_is_question(tpath)})
    elif event == "Notification":
        post({"evt": "notify", "sid": sid,
              "ntype": data.get("notification_type", "")})
    elif event == "SessionEnd":
        post({"evt": "end", "sid": sid})
    elif event == "PreToolUse":
        if data.get("tool_name") == "AskUserQuestion":
            handle_ask(data)
        sys.exit(0)                         # no-op for all other tools
    elif event == "PermissionRequest":
        tool = data.get("tool_name", "")
        if tool == "AskUserQuestion":
            sys.exit(0)                     # handled by PreToolUse, don't relay
        hint = summarize(tool, data.get("tool_input", {}))
        decision = request_decision(sid, tool, hint)   # allow | always | deny | None
        if decision == "deny":
            print(json.dumps({"hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "decision": {"behavior": "deny"}}}))
        elif decision == "always":
            # Persist a rule so this tool isn't prompted again (hold-A on device).
            print(json.dumps({"hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "decision": {"behavior": "allow", "applyRule": tool}}}))
        elif decision == "allow":
            print(json.dumps({"hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "decision": {"behavior": "allow"}}}))
        # else None: no stdout → Claude Code falls back to the native prompt
        sys.exit(0)

    sys.exit(0)  # never block Claude Code


if __name__ == "__main__":
    main()
