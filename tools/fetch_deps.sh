#!/bin/sh
# Fetch the public inputs ghostldr builds against (into build/, which git ignores):
#   build/nid/known_names.txt  PS4 symbol names for NID resolution (idc/ps4libdoc)
#   build/luac0re-src          Luac0re 2.4b (Gezine, MIT): Lua bundle + PS2 save
# The game dump (build/ghost/app0) is your own and is never fetched.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/nid
[ -s build/nid/known_names.txt ] ||
    curl -fsSL -o build/nid/known_names.txt https://raw.githubusercontent.com/idc/ps4libdoc/doc/known_names.txt
[ -d build/luac0re-src ] ||
    git clone --depth 1 --branch 2.4b https://github.com/Gezine/Luac0re build/luac0re-src
echo "known_names.txt: $(wc -l < build/nid/known_names.txt) names; Luac0re: build/luac0re-src"
