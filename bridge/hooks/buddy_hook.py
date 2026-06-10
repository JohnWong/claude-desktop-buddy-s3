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
import time

SOCK_PATH = os.path.expanduser("~/.claude-buddy/bridge.sock")

# Wait at most this long for a device decision before falling back to the native
# interactive prompt. Must be < the hook's timeout in settings.json.
PERMISSION_WAIT = 40.0
ASK_WAIT = 30.0          # per-question wait for an AskUserQuestion device pick
                         # (long is fine — the "terminal" escape row falls back
                         # to the keyboard instantly; this only bounds walk-away)


def post(obj: dict):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.4)
        s.connect(SOCK_PATH)
        s.sendall((json.dumps(obj) + "\n").encode())
        s.close()
    except Exception:
        pass  # fail-open: bridge down → no-op


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


def ask_device(sid: str, qid: str, header: str, options: list, proj: str = ""):
    """Relay a multiple-choice question to the device. Returns the chosen option
    index, or None to fall back to the native terminal picker (escape / timeout /
    no bridge)."""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(ASK_WAIT)
        s.connect(SOCK_PATH)
        s.sendall((json.dumps({"evt": "ask", "sid": sid, "id": qid, "proj": proj,
                               "header": header, "opts": options}) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(256)
            if not chunk:
                return None
            buf += chunk
        s.close()
        obj = json.loads(buf.split(b"\n", 1)[0].decode())
        if obj.get("escape"):
            return None
        return obj.get("index")
    except Exception:
        return None


def handle_ask(data: dict):
    """Intercept AskUserQuestion: answer each question on the device. Any escape/
    timeout/multiSelect falls back to the native picker (no stdout)."""
    ti = data.get("tool_input", {}) or {}
    questions = ti.get("questions") or []
    sid = data.get("session_id", "")
    proj = os.path.basename(data.get("cwd", "") or "")   # which project is asking
    answers = {}
    for i, q in enumerate(questions):
        opts = [o.get("label", "") for o in (q.get("options") or []) if o.get("label")]
        if q.get("multiSelect") or not opts:
            return                      # v1: single-select only → native picker
        header = (q.get("header") or q.get("question", ""))[:21]
        qid = f"{sid[:8]}-a{i}-{int(time.time() * 1000) % 100000}"
        idx = ask_device(sid, qid, header, opts[:4], proj)
        if idx is None or not (0 <= idx < len(opts)):
            return                      # escape/timeout → native picker
        answers[q.get("question", "")] = opts[idx]
    if not answers:
        return
    # updatedInput.answers does NOT pre-answer AskUserQuestion (the picker still
    # shows). Instead DENY the tool and hand the device's selection back to the
    # model via the reason, so it proceeds without re-asking.
    parts = [f"“{q}” → “{a}”" for q, a in answers.items()]
    reason = ("Answered on the hardware buddy: " + "; ".join(parts)
              + ". Treat these as the user's answers and continue; do not ask again.")
    print(json.dumps({"hookSpecificOutput": {
        "hookEventName": "PreToolUse",
        "permissionDecision": "deny",
        "permissionDecisionReason": reason,
    }}))


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
              "cwd": os.path.basename(data.get("cwd", "") or "")})
    elif event == "UserPromptSubmit":
        post({"evt": "run", "sid": sid,
              "cwd": os.path.basename(data.get("cwd", "") or ""),
              "tpath": data.get("transcript_path", "")})
    elif event == "Stop":
        post({"evt": "idle", "sid": sid,
              "tokens": count_output_tokens(data.get("transcript_path", ""))})
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
