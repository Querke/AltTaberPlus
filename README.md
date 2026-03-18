Fork of [AltTaber](https://github.com/MrBeanCpp/AltTaber) with some fixes and improvements:

Features:
* Multi-Window list: Apps with multiple windows now display a clean list of all window names below the app icon.
* Using keyboard layout agnostic keybind instead of hardcoded (always use the key to the left of 1, no matter what keyboard layout is used)
* Chrome/Edge PWA Separation: Installed Web Apps (e.g., Google Gemini, Messenger) are now treated as entirely separate apps from the main browser, complete with their own favicons.

Fixes:
* Some apps are shuffled to the back of the order in alt tab list when openened (eg uwp apps)
* Alt+` not working on startup (sometimes)
