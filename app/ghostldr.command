#!/bin/bash
# ghostldr app for macOS: double-click this file.
# The first time, macOS may refuse to open it: right-click it, choose Open, then Open.
cd "$(dirname "$0")" || exit 1

ok() { "$1" -c 'import sys; sys.exit(sys.version_info < (3, 11))' >/dev/null 2>&1; }

PY=""
for c in python3.14 python3.13 python3.12 python3.11 \
         /Library/Frameworks/Python.framework/Versions/Current/bin/python3 \
         /opt/homebrew/bin/python3 /usr/local/bin/python3 python3; do
  if command -v "$c" >/dev/null 2>&1 && ok "$c"; then PY="$c"; break; fi
done

if [ -z "$PY" ]; then
  echo "ghostldr needs Python 3.11 or newer. Get it from https://www.python.org/downloads/ then double-click this file again."
  osascript -e 'display alert "ghostldr needs Python 3.11 or newer" message "Install it from python.org (the download page opens now), then double-click ghostldr.command again."' >/dev/null 2>&1
  open "https://www.python.org/downloads/macos/" >/dev/null 2>&1
  exit 1
fi

exec "$PY" launch.py "$@"
