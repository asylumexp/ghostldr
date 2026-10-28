ghostldr @VERSION@
==================

ghostldr runs PS4 game dumps and OpenOrbis homebrew inside Star Wars Racer
Revenge (SWRR) on a stock PS5, through Luac0re: no jailbreak, no Python, no
program on a computer.

Supported: every PS5 firmware from 13.60, with SWRR US (CUSA03474) or EU
(CUSA03492) and Luac0re 2.4b. A PS4 runs homebrew and small titles only (SWRR
gets 2.5 GiB of memory there). Jak X with UltraC0re is experimental.

The files
  ghostldr.lua          send it to Luac0re's loader, port 9026
  ghostldr-ext.bin      the games: send it when ghostldr.lua asks, port 9021
  ghostldr-elfldr.bin   the ELF loader instead of the games (PS5)
  manager.lua           install a game over the network, without ghostldr
  SHA256SUMS.txt        checks them: shasum -a 256 -c SHA256SUMS.txt

1. Your game: copy its dump's folder (the one with eboot.bin in it) to an
   exFAT USB drive, in a folder of any name, and plug the drive into the PS5.
   No drive? Install the game over the network instead (step 4).
2. Start SWRR, click the right half of the touchpad at PRESS START, then
   OPTIONS > HALL OF FAME, until Luac0re's loader listens on port 9026.
3. Send the two files from a computer (CONSOLE is the PS5's IP address):
       nc CONSOLE 9026 < ghostldr.lua
       nc CONSOLE 9021 < ghostldr-ext.bin
   On Linux use nc -N for the first. On a phone or Windows, use a
   payload-sender app. Next time, send ghostldr.lua alone: it keeps the second
   file in SWRR's saved data.
4. The game selector shows your games: Cross starts one. Square opens
   Settings: what is wrong with a game or a drive, Install over network (an
   address to open in a browser on a phone or computer), the logs, Advanced,
   and Close ghostldr.

Every failure is explained on the TV. Each boot's log is a file in
ghostldr/logs on the USB drive.

The guide: https://github.com/asylumexp/ghostldr/blob/@VERSION@/docs/using-the-release.md
