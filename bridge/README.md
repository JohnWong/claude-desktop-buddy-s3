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
Only counts, the coarse event, the notification type, and the **cwd basename**
ever leave the host. No prompt text, tool input, file contents, diffs, or
transcript is sent to the device.
