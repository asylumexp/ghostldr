#!/bin/sh
# ghostldr app for Linux: double-click this file (choose "Run" if asked), or run ./ghostldr.sh
cd "$(dirname "$0")" || exit 1

ok() { "$1" -c 'import sys; sys.exit(sys.version_info < (3, 11))' >/dev/null 2>&1; }

PY=""
for c in python3.14 python3.13 python3.12 python3.11 python3; do
  if command -v "$c" >/dev/null 2>&1 && ok "$c"; then PY="$c"; break; fi
done

if [ -z "$PY" ]; then
  msg="ghostldr needs Python 3.11 or newer: install it with your package manager (for example: sudo apt install python3), then start this again."
  echo "$msg"
  if command -v zenity >/dev/null 2>&1; then zenity --error --text="$msg" >/dev/null 2>&1
  elif command -v kdialog >/dev/null 2>&1; then kdialog --error "$msg" >/dev/null 2>&1
  elif command -v notify-send >/dev/null 2>&1; then notify-send "ghostldr" "$msg"
  fi
  exit 1
fi

exec "$PY" launch.py "$@"
