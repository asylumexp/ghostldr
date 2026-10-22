"""A native "choose folder" dialog, with whatever the system has.

pywebview's own dialog when the app runs in its window; else AppleScript
(macOS), PowerShell (Windows), zenity or kdialog (Linux), else Tk.  Every
command is a fixed argument list: nothing the user typed reaches it.
Returns (available, path): available is False when no dialog could be
shown, and the UI falls back to its own folder browser.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys

PROMPT = "Choose the game folder (the one that contains eboot.bin)"
TIMEOUT = 15 * 60

_window = None          # the pywebview window, when there is one


def set_window(window) -> None:
    global _window
    _window = window


def _run(argv: list[str], cancelled=lambda r: r.returncode == 0) -> tuple[bool, str | None]:
    """(ran, chosen path or None if cancelled).  `cancelled(result)`: whether a
    run without a path was the user pressing Cancel (else the dialog failed)."""
    try:
        r = subprocess.run(argv, capture_output=True, timeout=TIMEOUT, stdin=subprocess.DEVNULL)
    except (OSError, subprocess.TimeoutExpired):
        return False, None
    out = r.stdout.decode("utf-8", "replace").strip()
    if r.returncode == 0 and out:
        return True, out
    return bool(cancelled(r)), None


def _webview() -> tuple[bool, str | None]:
    import webview  # type: ignore
    kind = getattr(webview, "FOLDER_DIALOG", None)
    if kind is None:
        kind = webview.FileDialog.FOLDER  # pywebview 5+
    got = _window.create_file_dialog(kind)
    if not got:
        return True, None
    return True, got[0] if isinstance(got, (list, tuple)) else str(got)


def _mac() -> tuple[bool, str | None]:
    script = f'POSIX path of (choose folder with prompt "{PROMPT}")'
    return _run(["osascript", "-e", script], lambda r: r.returncode == 1 and b"-128" in r.stderr)


def _windows() -> tuple[bool, str | None]:
    ps = ("Add-Type -AssemblyName System.Windows.Forms;"
          "[Console]::OutputEncoding = [System.Text.Encoding]::UTF8;"
          "$owner = New-Object System.Windows.Forms.Form -Property @{TopMost = $true};"
          "$d = New-Object System.Windows.Forms.FolderBrowserDialog;"
          f"$d.Description = '{PROMPT}';"
          "$d.ShowNewFolderButton = $false;"
          "if ($d.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) "
          "{ [Console]::Out.Write($d.SelectedPath) }")
    exe = shutil.which("powershell") or shutil.which("pwsh")
    if not exe:
        return False, None
    return _run([exe, "-NoProfile", "-NonInteractive", "-STA", "-Command", ps])


def _linux() -> tuple[bool, str | None]:
    if shutil.which("zenity"):
        return _run(["zenity", "--file-selection", "--directory", f"--title={PROMPT}"],
                    lambda r: r.returncode == 1)
    if shutil.which("kdialog"):
        return _run(["kdialog", "--getexistingdirectory", os.path.expanduser("~"), "--title", PROMPT],
                    lambda r: r.returncode == 1)
    return False, None


def _tk() -> tuple[bool, str | None]:
    code = ("import sys, tkinter, tkinter.filedialog as fd\n"
            "r = tkinter.Tk(); r.withdraw()\n"
            "try: r.attributes('-topmost', True)\n"
            "except Exception: pass\n"
            f"p = fd.askdirectory(title={PROMPT!r}, mustexist=True)\n"
            "sys.stdout.write(p or '')\n")
    return _run([sys.executable, "-c", code])


def choose_folder() -> tuple[bool, str | None]:
    tries = []
    if _window is not None:
        tries.append(_webview)
    if sys.platform == "darwin":
        tries.append(_mac)
    elif os.name == "nt":
        tries.append(_windows)
    else:
        tries.append(_linux)
    tries.append(_tk)
    for t in tries:
        try:
            ran, path = t()
        except Exception:
            continue
        if ran:
            return True, path
    return False, None
