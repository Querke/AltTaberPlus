# AltTaberPlus

## Build

- "build" = run `deploy.ps1`. Never just compile — it builds Release, deploys, and restarts the app.
- Steps it does: build Release → stop app → copy exe to `C:\dev\AltTaberPlus` → start app.
- `cmake` is not on PATH. Full path is in `deploy.ps1`.
- App runs elevated, so `Stop-Process` is denied. Only its scheduled task `AltTaber Startup` can stop/start it, and that works without a UAC prompt.
- Qt DLLs in `build/Release` and `C:\dev\AltTaberPlus` are already deployed. Only re-run `windeployqt` if the Qt version changes.

## Debugging window behaviour

- Window styles, class names, AppUserModelIds and shortcut data are all readable from PowerShell via P/Invoke. Probe the live window before guessing why an app behaves oddly — guesses about games and overlays have been wrong more than once.
- Ask before probing something that needs the user to set it up (a screen share, a game running).
