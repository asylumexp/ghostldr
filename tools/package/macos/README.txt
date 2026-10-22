ghostldr for macOS
==================

1. Drag ghostldr to Applications.
2. Open it. It opens in your web browser.

This copy is not signed with an Apple Developer ID or notarized, so the
first time macOS stops it ("can't be opened" / "Apple could not verify"):

  - macOS 15 and later: open it once, then System Settings > Privacy &
    Security > "ghostldr was blocked..." > Open Anyway.
  - macOS 14 and earlier: right-click (Control-click) ghostldr in
    Applications, choose Open, then Open.
  - Or in Terminal: xattr -dr com.apple.quarantine /Applications/ghostldr.app

The app runs in the background while its page is open: quit it with
"Quit the app" (Logs & Help), or it stops by itself after 30 minutes with
no page open. Opening ghostldr again brings the page back.

Its files: ~/Library/Application Support/ghostldr (settings, logs and the
working copy of the tools) and ~/Library/Logs/ghostldr/app.log.

ghostldr is MIT licensed. It includes Python (PSF License) and the
Noto Sans font (SIL Open Font License 1.1); see the files inside the app
(Contents/Resources).
