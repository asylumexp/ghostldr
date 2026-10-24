"""The app's settings: a small JSON file in the user's config directory.

Never in the repo: the last dump folder and console are the user's own.

    macOS    ~/Library/Application Support/ghostldr/settings.json
    Windows  %APPDATA%\\ghostldr\\settings.json
    Linux    $XDG_CONFIG_HOME/ghostldr/settings.json (~/.config/ghostldr)

GHOSTLDR_APP_CONFIG overrides the directory (tests).
"""

from __future__ import annotations

import copy
import json
import os
import sys
import tempfile
import threading
from pathlib import Path

DEFAULTS: dict = {
    "dump": "",                 # last game folder (an app0)
    "console": {"ip": "", "host_id": "", "name": "", "kind": ""},
    "title": "",                # last packed / chosen title ID
    "fetch_art": False,         # also ask the PlayStation Store for name and art when preparing
    "mode": "",                 # how games reach the PS5: "saves" (installed into SWRR's saved data) or "usb"
    "usb_where": "computer",    # the USB drive is plugged into this "computer" or the "console"
    "usb_drive": "",            # its mount point on this computer
    "usb_url": "",              # in the PS5: an FTP address other than ghostldr's own (a jailbroken PS5's ftpsrv)
}


def config_dir() -> Path:
    override = os.environ.get("GHOSTLDR_APP_CONFIG")
    if override:
        return Path(override)
    home = Path.home()
    if sys.platform == "win32":
        base = Path(os.environ.get("APPDATA") or home / "AppData" / "Roaming")
    elif sys.platform == "darwin":
        base = home / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME") or home / ".config")
    return base / "ghostldr"


def _clean(value, default):
    """`value` if it has the default's shape, else the default."""
    if isinstance(default, dict):
        value = value if isinstance(value, dict) else {}
        return {k: _clean(value.get(k, d), d) for k, d in default.items()}
    if isinstance(default, bool):
        return value if isinstance(value, bool) else default
    if isinstance(default, str):
        return value[:4096] if isinstance(value, str) else default
    return default


class Settings:
    def __init__(self, path: Path | None = None) -> None:
        self.path = path or config_dir() / "settings.json"
        self.lock = threading.Lock()
        self.data = copy.deepcopy(DEFAULTS)
        try:
            self.data = _clean(json.loads(self.path.read_text(encoding="utf-8")), DEFAULTS)
        except (OSError, ValueError):
            pass

    def get(self, key: str):
        with self.lock:
            return copy.deepcopy(self.data[key])

    def snapshot(self) -> dict:
        with self.lock:
            return copy.deepcopy(self.data)

    def update(self, **changes) -> dict:
        """Merge known keys (unknown ones are ignored) and save."""
        with self.lock:
            merged = copy.deepcopy(self.data)
            for k, v in changes.items():
                if k not in DEFAULTS:
                    continue
                if isinstance(DEFAULTS[k], dict) and isinstance(v, dict):
                    merged[k] = {**merged[k], **v}
                else:
                    merged[k] = v
            self.data = _clean(merged, DEFAULTS)
            self._save()
            return copy.deepcopy(self.data)

    def _save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix=".settings-", dir=self.path.parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(self.data, f, indent=1)
            os.replace(tmp, self.path)
        except OSError:
            try:
                os.unlink(tmp)
            except OSError:
                pass
