"""Known failures of the host scripts, mapped to a plain-language fix.

The list follows docs/using-the-release.md (Troubleshooting) and the
messages the scripts raise.  `diagnose` looks at a job's last lines, newest
first, and returns the first match: {id, title, fix, action}.  `action` is
what the UI offers next (a button), or "" for none.
"""

from __future__ import annotations

import re

# (id, regex searched in a line, title, fix, action)
KNOWN: list[tuple[str, str, str, str, str]] = [
    ("luacore", r"OOB_ARRAY_POINTER|restart the application to retry",
     "Luac0re didn't start this time",
     "This happens now and then. Close SWRR (PS button → SWRR's card → Options → Close Game), start it again, "
     "click the right half of the touchpad at PRESS START and open OPTIONS → HALL OF FAME.", "wait_loader"),
    ("loader", r"receiver did not come up|is SWRR at the Luac0re loader|cannot reach Luac0re's loader|"
               r"nothing at \S+:9026|SWRR up with Luac0re's loader",
     "The PS5's loader isn't listening",
     "Open OPTIONS → HALL OF FAME in SWRR again (restart SWRR first if it was used since), wait until the loader "
     "says it is listening on port 9026, then try again.", "wait_loader"),
    ("loader_send", r"socket\.timeout|TimeoutError|timed out|ConnectionRefusedError|Connection refused",
     "The PS5 didn't answer",
     "Close SWRR, start it again, open HALL OF FAME and try again. If it keeps happening, check that the PS5 and "
     "this computer are on the same network.", "wait_loader"),
    ("no_console", r"no PS4 or PS5 answered discovery|no PS4 or PS5 answered",
     "No PlayStation found on the network",
     "Turn the PS5 on (not in rest mode), make sure it is on the same network as this computer, then search "
     "again. You can also type its IP address (Settings → Network → Connection Status on the PS5).", "discover"),
    ("pick_console", r"cannot pick a console|no console chosen",
     "Several consoles answered",
     "Choose your PS5 from the list on the Console step.", "discover"),
    ("space", r"not enough space on the console|no space left on the console's storage",
     "Not enough free space on the PS5",
     "Nothing was changed. Free up space on the PS5 (Settings → Storage), or remove a title in the Library, "
     "then press Install again.", ""),
    ("nofit", r"its code does not fit the executable memory",
     "This game is too big for ghostldr to run",
     "Its code doesn't fit the memory ghostldr can use inside SWRR, so it could not boot. Nothing was sent. "
     "You can still install it from More options → Install anyway, but it won't start.", "install_anyway"),
    ("dropped", r"receiver closed the connection|ConnectionResetError|Connection reset by peer|BrokenPipeError|"
                r"Broken pipe|receiver status 0x",
     "The connection to the PS5 dropped",
     "What was already sent is kept. Check the PS5 is still on and SWRR still open, then restart SWRR, open HALL "
     "OF FAME and press Install again: it continues where it stopped.", "wait_loader"),
    ("not_packed", r"is not packed: run gfs_pack|nothing packed in",
     "The game isn't prepared yet",
     "Go to the Game step and press Prepare game first.", "goto_game"),
    ("no_eboot", r"no eboot\.bin \(point --src",
     "That folder isn't a game dump",
     "Choose the folder that contains eboot.bin (often called app0, or named after the title ID).", "goto_game"),
    ("need_title", r"pass --title \(e\.g\. CUSA12345\)",
     "Couldn't tell which game this is",
     "Enter its title ID (like CUSA12345) under More options on the Game step, then prepare it again.", "goto_game"),
    ("prefix", r"the prefix \S+ belongs to|is installed with the prefix",
     "Another installed title uses the same container names",
     "Remove the other copy first in the Library, or ask for help: this needs the command line "
     "(gfs_pack.py --prefix).", ""),
    ("ext_firewall", r"extension: the console did not fetch it|no extension from the computer",
     "The PS5 couldn't fetch the extension from this computer",
     "Your firewall is blocking TCP port 18195. Allow Python to accept incoming connections (see Logs & Help → "
     "Firewall), restart SWRR, open HALL OF FAME and press Play again.", "firewall"),
    ("ports_busy", r"cannot serve the (extension|selector) on TCP|Address already in use|address already in use|"
                   r"Only one usage of each socket address",
     "Another ghostldr session is using the network ports",
     "Something on this computer already uses UDP 18194 or TCP 18195/18196 — probably another run.py or a "
     "second copy of this app. Close it and try again.", ""),
    ("preflight", r"preflight: no terminal to ask on",
     "Some functions this game uses aren't implemented yet",
     "It may still play, or it may misbehave. You can boot it anyway.", "force_preflight"),
    ("prebuilt", r"--no-build needs|building the core failed|missing core\.bin|missing .*ext\.bin",
     "The prebuilt loader files are missing",
     "This folder lacks build/ghostldr/core.bin, core.json or ext.bin. Unpack the release zip again and start the "
     "app from inside it.", ""),
    ("manager_firewall", r"the manager did not report back",
     "The save manager didn't report back",
     "Read the address and token from the PS5's notification, or allow UDP port 18196 in this computer's firewall.",
     "firewall"),
    ("not_installed", r"is not installed \(installed:",
     "That title isn't installed",
     "Refresh the Library to see what is installed.", "refresh_library"),
    ("store", r"PlayStation Store.*nothing found|nothing found: name",
     "The PlayStation Store had no art for this title",
     "The game selector draws a tile with the name instead. You can try again later.", ""),
]

_COMPILED = [(i, re.compile(rx), t, f, a) for i, rx, t, f, a in KNOWN]


def diagnose(lines: list[str], depth: int = 60) -> dict | None:
    """The known problem behind a failed job, from its last `depth` lines."""
    for line in reversed(lines[-depth:]):
        for pid, rx, title, fix, action in _COMPILED:
            if rx.search(line):
                return {"id": pid, "title": title, "fix": fix, "action": action, "line": line.strip()[:400]}
    return None


def last_error(lines: list[str]) -> str:
    """The script's own last words (a SystemExit message or an exception)."""
    for line in reversed(lines[-30:]):
        s = line.strip()
        if s and not s.startswith(("Traceback", "File ", "^", "~")):
            return s[:400]
    return ""


# Problems no script reports, only the person in front of the TV sees: (id, title, fix).
SEEN_ONLY: list[tuple[str, str, str]] = [
    ("black", "Black screen, and no log lines in the app",
     "The firewall is blocking UDP 18194, or the app is talking to another device. Allow Python in the firewall "
     "(Logs & Help → Firewall) and check the console on the Console step."),
    ("disc", "The PS5 asks for the disc", "The SWRR disc must be in the console that runs it."),
    ("selector", "The game picker doesn't appear",
     "It shows when more than one game is installed, or when you choose “Pick on the PS5”. This computer's "
     "firewall must allow TCP 18196."),
    ("slow", "The install is slow",
     "A wired connection for the computer or the PS5 helps. You can pause it any time: it continues where it "
     "stopped."),
]


def catalogue() -> list[dict]:
    """Every known problem and its fix, for the Troubleshooting page."""
    seen = [{"id": i, "title": t, "fix": f} for i, t, f in SEEN_ONLY]
    return [{"id": i, "title": t, "fix": f} for i, _, t, f, _ in KNOWN] + seen
