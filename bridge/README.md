# Claude Code / Qoder CLI / Codex CLI → StickS3 bridge (Phase 2)

Drive the buddy directly from the `claude`, `qoder`, or `codex` CLI — **no desktop app**.

```
claude CLI ──hooks──► buddy_hook.py  ──┐
qoder CLI  ──hooks──► qoder_hook.py  ──┼── unix socket ──► buddy_bridge.py ──BLE──► StickS3
codex CLI  ──hooks──► codex_hook.py  ──┘
```

## Components
- `buddy_bridge.py` — long-running daemon: Unix-socket server + persistent BLE
  link, aggregates all sessions into the firmware's `{total,running,waiting,msg}`
  schema, pushes every 2.5s, auto-reconnects. Needs `bleak` → run with the venv:
  `~/.pio-venv/bin/python bridge/buddy_bridge.py`
- `hooks/buddy_hook.py` — Claude Code hook client. Fail-open (bridge down →
  no-op, Claude Code unaffected). Runs under system `python3`.
- `hooks/qoder_hook.py` — Qoder CLI hook client. Same IPC protocol, adapted for
  Qoder's `hookSpecificOutput` format. Fail-open, stdlib-only.
- `hooks/codex_hook.py` — Codex CLI hook client. Uses Codex lifecycle hooks,
  relays permission prompts, and reads Codex `token_count` transcript events for
  output-token counters plus 5h/7d rate-limit percentages when present.
- `qoder_statusline.py` — Qoder CLI statusLine adapter. Prints a compact
  model/project status line.

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

## Qoder CLI Setup
The bridge daemon is shared — one process serves both Claude Code and Qoder CLI
sessions simultaneously via the same Unix socket.

1. **Bridge daemon**: same as above (launchd agent or manual run). No changes needed.

2. **Hooks**: add to `~/.qoder/settings.json`:
   ```json
   {
     "hooks": {
       "SessionStart":      [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/qoder_hook.py"}]}],
       "UserPromptSubmit":  [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/qoder_hook.py"}]}],
       "Stop":              [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/qoder_hook.py"}]}],
       "Notification":      [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/qoder_hook.py"}]}],
       "SessionEnd":        [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/qoder_hook.py"}]}],
       "PreToolUse":        [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/qoder_hook.py"}]}],
       "PostToolUse":       [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/qoder_hook.py"}]}],
       "SubagentStop":      [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/qoder_hook.py"}]}],
       "PermissionRequest": [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/qoder_hook.py", "timeout": 60}]}]
     }
   }
   ```

3. **StatusLine** (optional): add to `~/.qoder/settings.json`:
   ```json
   {
     "ui": {
       "statusLine": {
         "type": "command",
         "command": "python3 /Users/john/alibaba/s3stick/bridge/qoder_statusline.py",
         "refreshInterval": 3000
       }
     }
   }
   ```

4. Open a `qoder` session → the device lights up just like with Claude Code.
   Both CLIs can run simultaneously; the bridge aggregates all sessions.

## Codex CLI Setup
The bridge daemon is shared — the same socket accepts Claude, Qoder, and Codex
events. Session ids are namespaced internally so identical ids from different
CLIs cannot collide.

1. **Bridge daemon**: same as above.

2. **Hooks**: add to `~/.codex/hooks.json` (merge into existing hooks). After
   changing hooks, open Codex and run `/hooks` once to review/trust the new
   command hook.
   ```json
   {
     "hooks": {
       "SessionStart":      [{"matcher": "startup|resume|clear|compact", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/codex_hook.py", "timeout": 15}]}],
       "UserPromptSubmit":  [{"hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/codex_hook.py", "timeout": 15}]}],
       "PreToolUse":        [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/codex_hook.py", "timeout": 15}]}],
       "PostToolUse":       [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/codex_hook.py", "timeout": 15}]}],
       "SubagentStop":      [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/codex_hook.py", "timeout": 15}]}],
       "Stop":              [{"hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/codex_hook.py", "timeout": 15}]}],
       "PermissionRequest": [{"matcher": "*", "hooks": [{"type": "command", "command": "python3 /Users/john/alibaba/s3stick/bridge/hooks/codex_hook.py", "timeout": 60}]}]
     }
   }
   ```

3. **Status line**: Codex does not use an external command-backed statusLine in
   this repo. Use Codex's built-in `/statusline` picker and enable items such as
   model, context remaining, token counters, and rate limits. The device gets
   token/rate-limit data from the hook's local transcript scan.
