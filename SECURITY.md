# Security policy

This is a hobby project, so support is best effort. I will answer as soon as I can, but I cannot promise a deadline.

## Reporting a vulnerability

Please do **not** open a public issue for security problems. Use GitHub's private reporting: open the **Security**
tab of this repository and choose **Report a vulnerability**. Include what you found, how to reproduce it and the
affected version or commit.

## What is in scope

This project is responsible for its own code:

- the Qt app in `qt/src` (window handling, the evdev input reader, the local HTTP server, the WebSocket client, the
  deep-link and single-instance handling)
- the page-side shims in `qt/js` (`tauri.js`, `ws.js`, `pet.js`)
- the setup scripts and patches

Out of scope: Kuro's frontend code, servers and accounts. Report problems with those to Kuro.

## What you should know as a user

- The official frontend keeps its session token in the app's browser storage (QtWebEngine cookies and
  `localStorage`, origin `http://127.0.0.1:<port>`), under `~/.local/share/com.kuro.wuwatappo.global/`. Anyone or any
  program that can read your user profile can read it. The frontend sends it to Kuro's own endpoints.
- The app reads `/dev/input/event*` read-only to count key presses and clicks. It never grabs devices or writes to
  them, forwards key names only to the frontend and stores counters, not typed text. Read `qt/src/inputmonitor.cpp` if
  you want to confirm that. Reading `/dev/input` needs membership of the `input` group (or equivalent), which is itself
  a privilege you should weigh.
- On KDE the app loads a small temporary KWin script (for the pointer position and shortcuts). It unloads it on exit
  and the script stops itself a few seconds after the app dies.
- Never post tokens, cookies, `store.json`, full logs or `wuwa-buddy://` URLs in public. Redact them first.
- The app starts a **local HTTP server on 127.0.0.1** to serve the unpacked frontend (preferred port 47821, a random
  one if taken). It serves only GET/HEAD, only files below `dist/`, and only for the `Host` header `127.0.0.1:<port>`.
  Other local users and local programs can still reach that port, and can read the (non-secret, Kuro-owned) frontend
  files through it; the session token is not served by it.
- The app is a **WebSocket client** for Kuro's multiplayer socket. It connects only to one allowed host and path over
  `wss`, sends no `Origin` or cookie, and limits message size and socket count. Messages come from Kuro's server and
  from other players; they are passed to the frontend.
- The `wuwa-buddy://` deep link and the single-instance socket (a per-user local socket) accept a URL from another
  process of the same user and hand it to the frontend. Treat such links like any login callback: do not open ones
  you did not start.
- Do not run QtWebEngine remote debugging or the `scripts/nest/cdp.py` DevTools client against an instance that holds
  your real login.

## Supported versions

Only the latest commit on `main`.

## Dependencies

The app links against your system's Qt 6 (including QtWebEngine, which is Chromium). Security fixes for those come
from your distribution's updates, not from this repository; keep them current, because the frontend loads remote
content from Kuro's servers inside that browser engine.
