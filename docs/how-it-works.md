# How it works

This page explains the moving parts in plain words. For how to build it, see the [README](../README.md); for what
was tested, see [compatibility.md](compatibility.md).

## The big picture

The official Windows app has two halves:

1. a **web frontend** (Vue, running in a webview) that draws the pet, the panels and all the game-like logic, and
2. a small **native backend** written in Rust with Tauri, which does everything a web page cannot: it creates the
   windows, makes the big overlay click-through, listens to the keyboard and mouse globally, shows the tray menu,
   stores counters and handles the login callback.

The frontend is pure web code and runs fine on Linux. The backend is Windows-specific (global input hooks, window
styles, Windows APIs). This project keeps the frontend and **replaces the backend** with a native Qt 6 application:
the frontend runs in QtWebEngine (Chromium), and a C++ `Backend` object answers the same commands the Tauri
backend did.

```
 your installer (.exe)                         this repository
        |                                              |
  scripts/extract_frontend.py  ----->  dist/  <----  patches/frontend.json
  (unpack, nothing decrypted)       (local only,     (small text edits,
                                     git-ignored)     applied by scripts/patch_frontend.py)
                                          |
                          served from 127.0.0.1 by qt/src/httpserver.cpp
                                          |
                      QtWebEngine views  <-- QWebChannel -->  C++ Backend  ->  wuwatappo-qt
```

## Getting the frontend without shipping it

The original app is a Tauri app, and Tauri embeds the web files inside the Windows executable, compressed with
Brotli (compression, not encryption). `scripts/extract_frontend.py` unpacks the installer with `7z`, finds the table
of embedded files in the executable and decompresses each one. It refuses to run on any installer other than the
exact version it was written for, checked by SHA-256.

The frontend then needs a handful of changes. They are stored as tiny "find this, replace with that" patches in
`patches/frontend.json`, each with a few characters of context so that it fails loudly if the input is not what we
expect. After patching, every file is checked against a recorded hash. Two small font files are generated locally
from the app's own fonts (subsets with exactly the same outlines, using the open-source `fonttools`), so a Latin
text does not have to load a 6 MB font.

Most patches date from the earlier WebKitGTK-based shell (they work around WebKitGTK painting problems, turn off the
Windows-only updater, make a hidden animation loop stop when idle, wait until fonts are ready before showing a
window). They have not all been re-validated under Chromium; see [qt-port/UNKNOWNS.md](qt-port/UNKNOWNS.md) (U4).

## The Qt app (`qt/`)

| Part | What it does |
|---|---|
| `src/main.cpp` | Startup: finds `dist/`, starts the local HTTP server, enforces a single instance (a `QLocalServer`; a second start forwards its `wuwa-buddy://` URL and exits), creates the windows. |
| `src/httpserver.cpp` | Serves `dist/` read-only on `127.0.0.1` so the pages get a stable `http` origin (cookies and `localStorage` persist, and the ES modules load, which `file://` cannot do). |
| `js/tauri.js` | Injected into every page before the frontend runs. Provides the `window.__TAURI_INTERNALS__` object the frontend expects (`invoke`, events, channels) on top of a **QWebChannel**. |
| `src/backend.cpp` | The `Backend`: about sixty commands the frontend calls (`invoke`) plus the plugin calls (autostart, deep-link, opener, window, event). The list was recovered by reading the frontend and the Windows executable (static analysis only; the executable was never run). Replies and events travel back to the page as signals. |
| `src/windows.cpp`, `qml/` | One QML `Window` plus `WebEngineView` per original window label (`main`, `panel`, `login`, `about`, `toast`, ...). Sets up the layer-shell overlay, stacking, input regions and monitor placement. |
| `src/regions.cpp`, `src/inputlogic.cpp` | Pure logic (hit regions, monitor choice, key-press pairing, touchpad taps), unit-tested without a display. |
| `src/inputmonitor.cpp` | Reads `/dev/input/event*` read-only to count key presses and clicks everywhere, registers the F7/F8/F9 shortcuts with KDE, and gets the pointer position on KDE Wayland (a small KWin script) or X11. |
| `src/desktop.cpp` | Reads Plasma's panel and fullscreen state (asynchronous D-Bus calls). |
| `src/tray.cpp`, `src/platform.cpp` | Tray icon and dock menu; autostart entry (`~/.config/autostart`). |
| `src/wsclient.cpp`, `src/wsframe.cpp`, `js/ws.js` | The multiplayer socket. Kuro's socket server answers a WebSocket handshake with HTTP 403 unless the `Origin` header is missing or exactly `http(s)://tauri.localhost`, and a browser engine sends its own origin. `ws.js` redirects that one `wss://` URL to a native client that sends no `Origin`. The client only connects to the one allowed host and path, limits message size and the number of sockets, and closes a window's sockets when it closes. |
| `src/store.cpp` | Counters and settings in `store.json` (same directory and format as the original app). |
| `js/pet.js` | Runs in the pet window and turns pointer events that land on the pet into the events the frontend expects. |

### The overlay trick

The Windows app uses one huge transparent window that ignores the mouse, plus a global mouse hook to decide what to
do. Wayland does not allow global mouse hooks, so this project does it the other way round:

- On compositors that support the `wlr-layer-shell` protocol (KDE, Sway, Hyprland, and others; not GNOME) and with
  LayerShellQt installed, the pet window is a **layer-shell surface** that covers the whole output, always on top,
  takes no keyboard focus and has no taskbar entry. Otherwise a maximized always-on-top window is used.
- The window's **input region** is shrunk to just the rectangles the frontend reports for the pet and its buttons.
  Everything outside is click-through, so your other apps keep working.
- When the pointer is over the pet, the browser gets real pointer events, which `pet.js` forwards in the format the
  frontend expects. Clicks and keys elsewhere are counted separately from `/dev/input`.

There is also a second layer-shell surface, the "toast" overlay for small messages. It covers the whole screen too,
so it must never accept input. Getting that wrong once (in the earlier shell) made it swallow every click on the
desktop; see the notes in [compatibility.md](compatibility.md).

### Login

Login opens your browser. When it finishes, the website redirects to a `wuwa-buddy://auth/callback?...` link. The
installed desktop entry (`qt/data/`) registers the app as the handler for that scheme; the link starts a second
instance, which hands the URL to the running app through the single-instance socket, and the app passes it to the
frontend.

## What it does not do

It does not touch game files, memory or network traffic of any game, does not automate anything, and does not
change how often or how fast counters grow: it only counts real input events.
