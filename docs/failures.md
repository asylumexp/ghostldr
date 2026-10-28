# When something goes wrong

ghostldr says every failure on the TV: what went wrong, and what to do. The details go to the log ([logs.md](logs.md)), and each message names where that log is.

- **Before a game runs**, a boot that stops shows its message in the system's message dialog, the one Luac0re's loader shows its own text in. Press OK (Cross): ghostldr goes back to Luac0re's loader, which says the boot stopped, and you can send `ghostldr.lua` again. Don't press OK on Luac0re's "The boot stopped" dialog too: in the final 1.0 test that left the TV black, and the next `ghostldr.lua` wasn't taken (Luac0re's loader had ended). If that happens, close SWRR from the PS button menu and start it again. Booted from a computer (`run.py`, `ghostldr.py boot`), whose terminal shows the log too, the message closes by itself after a minute.
- **While a game runs**, a crash, or the game quitting, replaces the game's picture with a page of ghostldr's own. After a crash the page stays until you close SWRR. After the game quits, SWRR closes: a PS4 goes back to its home screen, and a PS5 starts SWRR again.
- **A game that stops drawing** gets a notification, and its picture stays. It may come back by itself.
- If ghostldr can't show its own message, a system notification says the game stopped and where the log is.

## Before the core: the bundle (ghostldr.lua)

`ghostldr.lua` shows these in Luac0re's dialog itself, and sends each to the sender's connection and to a notification ([architecture.md](architecture.md#two-file-boot)).

| What happened | On the TV |
|---|---|
| Nothing was sent within 5 minutes | "Timed out: nothing came on port 9021 in 5 minutes. Back to Luac0re's loader" |
| Something that isn't a bundle was sent | Refused, with what it started with and what the port takes |
| A bundle from another release, or a damaged one | "*file* is damaged: it does not match this ghostldr.lua (build …). Send both from the same release" |
| The bundle came only in part | Refused, with how many bytes came |
| No memory for the bundle | "no memory for its … bytes: restart SWRR and try again" |
| The cached bundle is damaged | "The cached bundle is damaged: deleted. Send the bundle again." |
| The cached bundle is another release's | It says so above the prompt; the next bundle replaces it |
| The extension fails its checksums (`-7`) | A cached bundle: deleted, and the prompt asks again. A sent one: "The boot stopped" |

## Starting the game

Each message has the headline, what happened, "What to do", and "The log: …". `%s` below is the file, folder or game the message names.

| Failure | Headline | What happened | What to do |
|---|---|---|---|
| No drive (`usb:` asked for) | No USB drive found. | No USB drive is plugged into the console. | Plug in the drive with your games, wait a few seconds, then send ghostldr.lua again. |
| The drive refused | The USB drive can't be opened. | The console didn't give the drive to SWRR. | Plug the drive in again and send ghostldr.lua again, or install the game into SWRR's saved data. |
| `usb:/FOLDER` missing | The folder isn't on the USB drive. | No USB drive has the folder %s. | Check the folder's name, or leave it out to search the whole drive. |
| No games on the drive | No games on the USB drive. | A game is its dump's folder, with eboot.bin in it, and the drive has none. | Copy the game's whole dump folder onto the drive (at most 4 folders down). |
| The game asked for isn't on the drive | The game isn't on the USB drive. | %s isn't among the games on the drive. | Copy it onto the drive, or start a game that is there. |
| A folder with no title ID | The game's folder doesn't say which game it is. | %s has no sce_sys/param.sfo, and its folder name has no game ID. | Name the folder like "My Game (CUSA12345)", or copy sce_sys/param.sfo from the dump. |
| No game anywhere (`auto`) | No games found. | No USB drive is plugged in (or: the USB drive has no game ghostldr can start), and no game is installed in SWRR's saved data. | Copy a game's dump folder onto a USB drive and plug it in, or install a game over the network. |
| The game asked for isn't installed | The game isn't installed. | %s isn't installed in SWRR's saved data. | Install it, or start it from a USB drive. |
| An install cut short | The game's install isn't finished. | %s isn't completely installed. | Install it again: it goes on where it stopped. |
| An install's index can't be read | The installed game can't be read. | %s's install is damaged. | Remove it and install it again. |
| eboot.bin, libc.prx or libSceFios2.prx missing | A file of the game is missing. | %s isn't there. | Copy the game's whole dump folder again. |
| …unreadable | A file of the game can't be read. | %s couldn't be read. | Plug the drive in again, or copy the game again. |
| …encrypted | The game's files are encrypted. | %s is encrypted: ghostldr needs a decrypted dump. | Dump the game again with a dumper that decrypts eboot.bin and sce_module. |
| …not an executable | A file of the game can't be used. | %s isn't a PS4 program ghostldr can load (damaged or cut short?). | Copy the game's whole dump folder again. |
| The memory plan refuses (`-5`) | The game needs more memory than SWRR has. | %s doesn't fit in the memory ghostldr can give it. | This game can't run with ghostldr yet. |
| No executable memory (`-3`) | There's no room for the game's code. | ghostldr ran out of memory it can run code from. | Close SWRR, start it again and send ghostldr.lua again. If it keeps happening, the game is too big for now. |
| No executable memory, and the big-executable planner says why (`bx_why()`) | There's no room for the game's code. | Its sentence, e.g. "eboot.bin won't fit: its code needs … KiB of JIT memory …" | Close SWRR, start it again and send ghostldr.lua again. If it keeps happening, the game is too big for now. |
| Relocation failed (`-4`) | The game's code couldn't be prepared. | Linking it failed. | This game may not work with ghostldr yet: the log says which part. |
| Writing the code failed (`-5`) | The game's code couldn't be loaded. | Writing it into memory failed. | Close SWRR, start it again and send ghostldr.lua again. |
| The main thread wasn't created | The game couldn't be started. | Its main thread couldn't be created. | Close SWRR, start it again and send ghostldr.lua again. |
| The extension failed to start (`-8`) | (a notification) | "ghostldr: the game stopped. Its log says why: ghostldr/logs on the USB drive, or /temp0/ghostldr.log" | |
| ghostldr itself crashed while starting the game | ghostldr crashed while starting the game. | | Close SWRR, start it again and send ghostldr.lua again. |
| Anything else | The game didn't start. | Something went wrong while ghostldr was starting it. | Close SWRR, start it again and send ghostldr.lua again. If it happens again, the log says why. |

The two that need a code to say more show it under the log's place ("Code: -9").

## While the game runs

| Failure | How it shows | Headline | What happened | What to do |
|---|---|---|---|---|
| The game crashed | A notification, then ghostldr's page, kept until SWRR is closed. Under the log's place: "Crash: signal 11 at eboot.bin+1234" for a bug report | The game crashed. | It stopped working and can't go on. | To play again, close SWRR from the PS button's menu and start it again. If it crashes at the same place each time, this game may not work with ghostldr yet. |
| It quit (`_exit`, `exit`) | ghostldr's page, then SWRR closes (Cross, or after 15 seconds) as an app closes, with no error screen | The game has quit. | It ended itself (with an error, code N). | SWRR closes now (a PS5 starts it again): press Cross, or wait. |
| Its program ended (its entry returned) | The same | The game has ended. | Its program finished. | The same |
| No new picture for a minute | A notification | The game seems stuck. | | If nothing changes, close SWRR from the PS button's menu. |
| No picture two minutes after it started | A notification | The game hasn't shown anything yet. | | It may still be loading. If the screen stays black, close SWRR from the PS button's menu. |

The stuck checks count only while nothing else explains a still screen: not while one of ghostldr's drawn dialogs waits for an answer, nor while the game waits for its USB drive to come back. They count seconds the game is running, so rest mode doesn't set them off.

## When ghostldr can't say anything

Some ends of a game happen where ghostldr gets no chance to run: the console ends SWRR itself, without the signal ghostldr's crash report waits for. That happens on a GPU fault, when the system runs out of memory, or when a call goes where the system doesn't allow it. The screen is then the system's own:

- PS5: "Game or App Error. Something went wrong with this game or app." (CE-108255-1), then the home screen.
- PS4: "An error has occurred in the following application." (CE-34878-0).

A game can also take the crash signals over with handlers of its own (Unity's Mono runtime does). Its crashes then end as its handler decides, without ghostldr's report or page. The log says so when the game installs them: "the title installs its own handler for signal 11".

What to do: press OK, start SWRR again, and send `ghostldr.lua` again. If it happens again at the same place, this game may not work with ghostldr yet. The log on the USB drive has every line up to the end, because each line is written as it comes. Its last lines say what the game was doing, so attach it to a bug report. For example, PS4-Xplorer from a PS5's drive ended there right after its `sceKernelGetFsSandboxRandomWord` lookup. Without a drive, the log in `/temp0` goes with SWRR's session.

## How it works

- **The loader's notes.** While booting, the core notes the last title file it couldn't use (`fail_note`: missing, unreadable, encrypted, not an executable, too big for the memory plan) and why an installed title can't be served (`ghostfs.c`); the extension's app roots note why a drive's games can't be (`approot.c`). `ghost_boot`'s result and the note pick the message (`ext/src/failure.c`).
- **Where it's shown.** `main.c` passes `ghost_boot`'s result to `fail_result` (`src/fail.c`). It asks the extension to show it (`ext.c ext_show`), placing the extension's code just for that when the boot stopped before the code was placed, and giving its JIT block back afterwards. A failure among the drive's games is said right where it happens (`failure.c` wraps the extension's early entry), since the core gives the code back right after.
- **The page.** It's drawn with the drawn dialogs' font (`dlg_overlay.c ov_page`). For a title that draws with the CPU (linear frames), it goes into the title's own frame on screen. Otherwise the extension allocates one 1920×1080 linear buffer (8 MiB of direct memory) and registers it on the title's VideoOut handle at a free index (15 down to 2). When there's no free slot (`0x8029000F`, as on a PS4), it first unregisters the title's own buffer sets, since the title can't draw any more. Then it flips the buffer, and flips it again whenever something else was flipped since. A title that never opened VideoOut gets the core's override of `sceVideoOutOpen`. If no page can be drawn, a crash's notification comes again every 30 seconds.
- **Closing SWRR** after a title's exit: `sceSystemServiceLoadExec("exit")`, as an app closes itself. A plain `_exit` is a crash to the system (CE-34878-0 on a PS4).
- **The monitor.** A thread of the extension's (`ghostldr-monitor`), from the title's start: once a second it syncs the drive's log, attaches the log again after its drive came back, and reads the title's flip count (`sceVideoOutGetFlipStatus`) for the stuck checks.
- **A crash** is reported by the core's signal handler (`crash.c`, signals 4, 6, 8, 10 and 11): the log first, a notification, then the page on the crashing thread, which stays parked.

## Tested

- **PS5 13.60** (stock, US SWRR, `run.py`, two drives in):
  - "The folder isn't on the USB drive." (`usb:/` with a folder that isn't there), with its log on the drive.
  - A homebrew that crashes over CPU-drawn (linear) frames: the page over its frame. The log is on the drive with the crash report, fetched over the crashed title's FTP server.
  - The same over tiled frames, as a GPU title's are: the page in a buffer of ghostldr's own.
  - A call through a null pointer is caught too (signal 11 at address 0): the page.
  - A homebrew that calls `exit(0)`: the page, then SWRR starts again.
  - A homebrew that stops drawing: "The game seems stuck." after 64 s. One that never draws: "The game hasn't shown anything yet." after 120 s.
- **PS4 12.50** (EU SWRR): "The game isn't installed." The crash and exit pages: SWRR then goes back to the home screen with no error.
- **Offline** (`tests/test_diag.py`): every boot failure's message, the crash, exit and still-screen paths, and the page's drawing.
- **Not yet on hardware:** a commercial GPU title's crash (the tiled-buffer homebrew stands in for it), and the ELF loader's "waiting for N payloads" dialog beside the selector (its parameters fixed as this file's dialog's were; a notification when it still can't open).
