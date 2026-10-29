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
styles, Windows APIs). This project keeps the frontend and **replaces the backend** with a new Linux one.

```
 your installer (.exe)                         this repository
        |                                              |
  scripts/extract_frontend.py  ----->  dist/  <----  patches/frontend.json
  (unpack, nothing decrypted)       (local only,     (small text edits,
                                     git-ignored)     applied by scripts/patch_frontend.py)
                                          |
                                 embedded by Tauri at build time
                                          |
                                    src-tauri/ (Rust)  ->  wuwatappo-linux
```

## Getting the frontend without shipping it

Tauri embeds the web files inside the Windows executable, compressed with Brotli (compression, not encryption).
`scripts/extract_frontend.py` unpacks the installer with `7z`, finds the table of embedded files in the executable
and decompresses each one. It refuses to run on any installer other than the exact version it was written for,
checked by SHA-256.

The frontend then needs a handful of changes to behave on Linux. They are stored as tiny "find this, replace with
that" patches in `patches/frontend.json`, each with a few characters of context so that it fails loudly if the input
is not what we expect. After patching, every file is checked against a recorded hash. Two small font files are
generated locally from the app's own fonts (subsets with exactly the same outlines, using the open-source
`fonttools`), so a Latin text does not have to load a 6 MB font.

The panel's menu collapse is one example of working around a Linux-only rendering problem. The original app resizes the panel window when the menu is collapsed. Doing that on WebKitGTK showed stale or stretched pixels for a few frames, so the window keeps its size, a CSS patch narrows the card with a short transition, and `src-tauri/src/main.rs` makes the empty strip beyond the card click-through.

The patches do things like: let the frontend ask the Linux backend where the screen origin is, turn off the
Windows-only updater, make a hidden animation loop stop when idle, wait until fonts are ready before showing a
window, and force a redraw of the Synthesis slots (a WebKitGTK painting bug left stale pixels there).

## The Linux backend (`src-tauri/src`)

| File | What it does |
|---|---|
| `main.rs` | Creates the windows and implements about sixty commands the frontend calls (`invoke`). The list was recovered by reading the frontend and the Windows executable (static analysis only; the executable was never run). |
| `shim.js` | Runs in the pet window. Turns pointer events that land on the pet into the `mouse-move` and `mouse-button` events the frontend expects. |
| `input.rs` | Reads `/dev/input/event*` read-only to count key presses and clicks everywhere, detects touchpad taps, and finds the pointer position on KDE Wayland (through a small KWin script) or X11. |
| `monitors.rs`, `platform.js` | Chooses the output for the pet, handles F8 (switch monitor) and the coordinate maths for mixed scaling. |
| `desktop_integration.rs` | Reads Plasma's panel and fullscreen state, and keeps the overlay layer correct. |
| `service_status.rs` | The maintenance-notice handshake the frontend expects. |
| `native_ws.rs`, `ws.js` | The multiplayer socket. Kuro's socket server answers a WebSocket handshake with HTTP 403 unless the `Origin` header is missing or exactly `http(s)://tauri.localhost`; WebKitGTK sends `tauri://localhost`. `ws.js` redirects that one `wss://` URL to a native client that sends no `Origin`. The client only connects to the one allowed host and path, limits message size and the number of sockets, and closes a window's sockets when it closes. |
| `cookies.js` | The frontend keeps its session in `document.cookie`, which WebKitGTK does not persist for the `tauri://` scheme; this stores it in `localStorage` instead. |

### The overlay trick

The Windows app uses one huge transparent window that ignores the mouse, plus a global mouse hook to decide what to
do. Wayland does not allow global mouse hooks, so this project does it the other way round:

- On compositors that support the `wlr-layer-shell` protocol (KDE, Sway, Hyprland, and others; not GNOME), the pet
  window is a **layer-shell surface** that covers the whole output, always on top, takes no keyboard focus and has
  no taskbar entry.
- The window's **input region** is shrunk to just the rectangles the frontend reports for the pet and its buttons.
  Everything outside is click-through, so your other apps keep working.
- When the pointer is over the pet, the browser gets real pointer events, which `shim.js` forwards in the format
  the frontend expects. Clicks and keys elsewhere are counted separately from `/dev/input`.

There is also a second layer-shell surface, the "toast" overlay for small messages. It covers the whole screen too,
so it must never accept input. Getting that wrong once made it swallow every click on the desktop; see the notes in
[compatibility.md](compatibility.md).

### Login

Login opens your browser. When it finishes, the website redirects to a `wuwa-buddy://auth/callback?...` link. The
app registers itself as the handler for that scheme (via the Tauri deep-link and single-instance plugins), so the
link reaches the already-running app, which hands it to the frontend.

## What it does not do

It does not touch game files, memory or network traffic of any game, does not automate anything, and does not
change how often or how fast counters grow: it only counts real input events.
