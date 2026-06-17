# Claude Code → StickS3 bridge (Phase 2)

Drive the buddy directly from the `claude` CLI — **no desktop app**.

```
claude CLI ──hooks──► buddy_hook.py ──unix socket──► buddy_bridge.py ──BLE──► StickS3
```

## Components
- `buddy_bridge.py` — long-running daemon: Unix-socket server + persistent BLE
  link, aggregates all sessions into the firmware's `{total,running,waiting,msg}`
  schema, pushes every 2.5s, auto-reconnects. Needs `bleak` → run with the venv:
  `~/.pio-venv/bin/python bridge/buddy_bridge.py`
- `hooks/buddy_hook.py` — tiny stdlib-only hook client. Fail-open (bridge down →
  no-op, Claude Code unaffected). Runs under system `python3`.

## State mapping (M2)
| Hook | → session state |
|------|-----------------|
| SessionStart | idle (registered, total +1) |
| UserPromptSubmit | **running** (whole turn = busy) |
| Stop | idle (turn ended, awaiting you) |
| Notification(idle_prompt) | idle/awaiting |
| SessionEnd | removed (total −1) |
| **PermissionRequest** (M3) | relays prompt to device; **A=approve, B=deny**; `waiting` = pending approvals |

## Permissions (M3)
`PermissionRequest` fires only when a tool actually needs approval. The hook
relays `{tool, hint}` to the bridge and blocks; the device shows the approval
screen (red flash + beep). Press **A** to approve / **B** to deny — the bridge
returns the decision to Claude Code as
`hookSpecificOutput.decision.behavior = "allow"|"deny"`.

**Dual-path / fail-open:** if you answer at the keyboard instead, or the device
times out / is unplugged / the bridge is down, the hook closes without output and
Claude Code falls back to its native interactive prompt. The device prompt is
auto-cancelled when the hook closes. The firmware needs **no changes** — it
already renders the prompt and emits `{"cmd":"permission","id","decision"}`.

## Ghostty tab ordering (auto, opt-in)
When sessions run under **Ghostty**, the 3-light strip (and home screen) orders
the shown sessions by **Ghostty tab position** instead of seating order — tab 1
before tab 2 …, and within a tab (splits) by a fixed split index.

How it works: the hook reports each session's controlling `tty` + `TERM_PROGRAM`
on start/run. While any Ghostty session exists, the bridge maps `tty → (window,
tab, split)` via Ghostty's AppleScript dictionary (`osascript`, refreshed every
3s) and sorts by that. (`GHOSTTY_SURFACE_ID` is *not* usable — it doesn't match
the scripting IDs; `tty` is the stable join key.)

- **Non-Ghostty is unaffected:** with no `ghostty` session the bridge never calls
  `osascript` and keeps the original fixed-slot behavior.
- **Graceful fallback:** if Automation permission isn't granted (or Ghostty is
  too old to have the dictionary), the map is empty and it falls back silently.
- **Permission:** `osascript` controlling Ghostty needs macOS **Automation**
  permission. A launchd background agent may not get the prompt, so grant it once
  by running the bridge in the foreground from Ghostty —
  `~/.pio-venv/bin/python bridge/buddy_bridge.py` — and approving the "control
  Ghostty" dialog (recorded in System Settings → Privacy & Security → Automation;
  the same python path is then honored under launchd).

## Setup
1. Install the bridge as a launchd agent (auto-start + restart on crash):
   ```
   cp bridge/launchd/com.claude-buddy.bridge.plist ~/Library/LaunchAgents/
   launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.claude-buddy.bridge.plist
   ```
   Logs: `~/.claude-buddy/bridge.log` / `bridge.err`. To stop/reload:
   `launchctl bootout gui/$(id -u)/com.claude-buddy.bridge`.
   (For ad-hoc runs without launchd: `~/.pio-venv/bin/python bridge/buddy_bridge.py`.)
2. Add the hooks to `~/.claude/settings.json` (merge into existing `hooks`).
   Status hooks (no timeout needed):
   ```json
   {
     "hooks": {
       "SessionStart":      [{ "hooks": [{ "type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/buddy_hook.py" }] }],
       "UserPromptSubmit":  [{ "hooks": [{ "type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/buddy_hook.py" }] }],
       "Stop":              [{ "hooks": [{ "type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/buddy_hook.py" }] }],
       "Notification":      [{ "hooks": [{ "type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/buddy_hook.py" }] }],
       "SessionEnd":        [{ "hooks": [{ "type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/buddy_hook.py" }] }],
       "PermissionRequest": [{ "matcher": "*", "timeout": 60, "hooks": [{ "type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/buddy_hook.py" }] }]
     }
   }
   ```
   The `PermissionRequest` hook **must** carry a `timeout` (≥ the hook's internal
   40s wait) so it can block for the device decision.
3. Open a new `claude` session → device `total` goes to 1; submit a prompt →
   `running` ticks; trigger a tool that needs approval → device shows the prompt,
   press **A/B** to decide; the session ends → `total` drops.

## Privacy
Only counts, the coarse event, the notification type, the **cwd basename**, and
(for tab ordering) the session's **tty + `TERM_PROGRAM`** ever leave the hook —
and the tty/term go only to the local bridge, never to the device. No prompt
text, tool input, file contents, diffs, or transcript is sent.
