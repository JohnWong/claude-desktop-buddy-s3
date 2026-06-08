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

`waiting` and permission round-trip arrive in **M3**.

## Setup
1. Start the bridge (keep it running; later we'll make it a launchd agent):
   ```
   ~/.pio-venv/bin/python bridge/buddy_bridge.py
   ```
2. Add the hooks to `~/.claude/settings.json` (merge into existing `hooks`):
   ```json
   {
     "hooks": {
       "SessionStart":     [{ "hooks": [{ "type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/buddy_hook.py" }] }],
       "UserPromptSubmit": [{ "hooks": [{ "type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/buddy_hook.py" }] }],
       "Stop":             [{ "hooks": [{ "type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/buddy_hook.py" }] }],
       "Notification":     [{ "hooks": [{ "type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/buddy_hook.py" }] }],
       "SessionEnd":       [{ "hooks": [{ "type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/buddy_hook.py" }] }]
     }
   }
   ```
3. Open a new `claude` session → device `total` goes to 1; submit a prompt →
   `running` ticks; the session ends → `total` drops.

## Privacy
Only counts, the coarse event, the notification type, and the **cwd basename**
ever leave the host. No prompt text, tool input, file contents, diffs, or
transcript is sent to the device.
