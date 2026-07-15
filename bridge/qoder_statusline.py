#!/usr/bin/env python3
"""Qoder CLI statusLine: print a compact model/project status line.

Wire it in ~/.qoder/settings.json:
    "ui": {
      "statusLine": {
        "type": "command",
        "command": "python3 /abs/path/bridge/qoder_statusline.py",
        "refreshInterval": 3000
      }
    }
"""
import json
import os
import sys


def main():
    try:
        d = json.load(sys.stdin)
    except Exception:
        sys.exit(0)

    model = (d.get("model") or {}).get("display_name") or ""
    cwd = d.get("workspace", {}).get("current_dir", "") or d.get("cwd", "")
    cwd_base = os.path.basename(cwd) if cwd else ""

    bits = []
    if model:
        bits.append(model)
    if cwd_base:
        bits.append(cwd_base)
    sys.stdout.write("  ".join(bits))


if __name__ == "__main__":
    main()
