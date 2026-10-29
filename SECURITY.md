# Security policy

This is a hobby project, so support is best effort. I will answer as soon as I can, but I cannot promise a deadline.

## Reporting a vulnerability

Please do **not** open a public issue for security problems. Use GitHub's private reporting: open the **Security**
tab of this repository and choose **Report a vulnerability**. Include what you found, how to reproduce it and the
affected version or commit.

## What is in scope

This project is responsible for its own code:

- the Rust app in `src-tauri/src` (window handling, the input reader, the deep-link and single-instance handling)
- the small JavaScript shims (`shim.js`, `cookies.js`, `platform.js`)
- the setup scripts and patches

Out of scope: Kuro's frontend code, servers and accounts. Report problems with those to Kuro.

## What you should know as a user

- The official frontend keeps its session token in the app's browser storage (`localStorage`, under the `tauri://`
  origin), under `~/.local/share/com.kuro.wuwatappo.global/`. Anyone or any program that can read your user profile
  can read it. The app only sends it to Kuro's own endpoints.
- The app reads `/dev/input/event*` read-only to count key presses and clicks. It never grabs devices or writes to
  them, forwards key names only to the frontend and stores counters, not typed text. Read `src-tauri/src/input.rs` if
  you want to confirm that.
- On KDE the app loads a small temporary KWin script (for the pointer position and shortcuts). It unloads it on exit
  and the script stops itself a few seconds after the app dies.
- Never post tokens, cookies, `store.json`, full logs or `wuwa-buddy://` URLs in public. Redact them first.
- Do not run a WebKit remote inspector on an app instance that holds your real login.

## Supported versions

Only the latest commit on `main`.
