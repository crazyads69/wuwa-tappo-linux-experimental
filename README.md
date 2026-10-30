# Wuwa Tappo Linux (experimental)

> [!WARNING]
> **Unofficial, experimental and a learning project.** It is not made, endorsed or supported by Kuro Games, Irox
> Games or the creators of Bongo Cat. It is **not a replacement** for the official app.
>
> **Using it can get your game account limited or banned.** Kuro's Terms of Use prohibit "unauthorized third-party
> programs" and allow account action without notice. Nobody (including me) knows how Kuro treats a non-official
> client of this app. If you try it, use a throwaway account, never your main one, and accept the risk yourself.
> Details in [Risks](#risks-please-read).

Wuwa Tappo is a desktop pet app from Kuro Games. Its official build only runs on Windows. This project is an
attempt to run it natively on Linux by re-implementing the Windows-only part of the app (as a Qt 6 application with
QtWebEngine) and reusing its web frontend, which stays Kuro's property.

It started as a personal experiment in reverse engineering for interoperability, and it is published so other people
can learn from it, test it on other systems and help fix what is broken. It is a hobby project: expect rough edges.

## What this repository contains, and what it does not

This repository contains **only original code and documentation**. It does **not** contain the official installer,
the app's frontend, its artwork, its fonts or its icon. You bring your own installer (downloaded from Kuro's
official source), and `scripts/setup.sh` unpacks the frontend from it **on your own computer** and applies a few
small text patches. The unpacked files stay in `dist/`, which git ignores.

The license in this repository (MIT) covers only the files here. It grants no rights to anything that comes from the
official app.

## Status

Everything below was checked by the author on **one machine: Fedora Asahi Remix 44 (aarch64, Apple Silicon), KDE
Plasma 6.7 on Wayland, Qt 6.11**, mostly inside a throwaway nested KWin session (virtual outputs, software
rendering, no network). "Tested" means that, and nothing more. Other desktops, distributions, GPUs and x86_64 are
unknown territory. The Qt app is a rewrite of an earlier Rust/Tauri shell (removed from the tree, still in git
history), so older test results for that shell do not carry over. The full table, with how each item was checked, is
in [docs/qt-port/VERIFICATION.md](docs/qt-port/VERIFICATION.md) and [docs/compatibility.md](docs/compatibility.md).

| Area | State |
|---|---|
| Pet shown as a transparent, always-on-top layer-shell overlay on KDE Wayland | Renders in the nested KWin session (software rendering). **Not yet confirmed on the author's real GPU/desktop**, where an earlier probe showed no pixels; see [UNKNOWNS.md](docs/qt-port/UNKNOWNS.md) (U0). |
| Clicks outside the pet go to the apps underneath | Works in the nested session (a click outside the pet reached a window beneath). Please report anything that still blocks clicks. |
| Pet click counting | Works in the nested session. |
| Counting key presses and clicks anywhere on screen (`/dev/input`) | Implemented; **not verified end to end** (no injectable device in the test session). Touchpad tap detection is unit-tested only. |
| Panel (Backpack, Gallery, Synthesis, Multiplayer, Settings) | Opens and closes via its button and renders. Closing by clicking outside it, the dock menu's submenus and any action that changes server data were **not** verified. |
| Login through the browser (`wuwa-buddy://` callback) | The second-instance hand-off of the URL works in the nested session. A full login with the Qt app was **not** done. |
| Multiplayer connection | Native WebSocket client with unit tests against a local server. **No end-to-end run** (needs Kuro's server). |
| Switching the pet between monitors with F8 | Works in a virtual two-output KWin; never on real monitors. |
| F7/F8/F9 hotkeys | F8 and F9 work in the nested session via KDE's shortcut system; F9 is suspected to fail on a second launch in the same session; F7 not pressed. **While the app runs, other apps no longer receive these keys.** |
| Tray icon | Unverified (no StatusNotifier host in the test session). |
| Automatic updates | Not supported, on purpose. The app never downloads or runs a Windows installer. |
| GNOME, Hyprland, Sway, other distributions | **Untested.** GNOME has no layer-shell, so only a fallback window mode could work there. X11 (Xvfb, no compositor) renders; click-through is unverified. |
| Languages other than English | Unverified. |

Known problems: memory use is expected to be high (a full-output transparent web view); the frontend's WebKit
workaround patches have not been re-validated under Chromium. See [docs/compatibility.md](docs/compatibility.md).

## Quick start

You need the **Wuwa Tappo global 1.0.1** Windows installer (`WuwaTappo-global_1.0.1_x64-setup.exe`). The setup
script checks its SHA-256 and refuses any other version, because the patches are written for that exact build.

Requirements (only Fedora 44 was used; other distributions' package names are not tested):

- Build: a C++17 compiler, `cmake`, `ninja`, Qt 6.5 or newer development files (Core, Gui, Network, Qml, Quick,
  DBus, Widgets, Test). On Fedora: `cmake ninja-build gcc-c++ qt6-qtbase-devel qt6-qtdeclarative-devel`. Optional:
  `wayland-devel`, `libxcb-devel` (used if found).
- Run time: QtWebEngine and QtWebChannel for QML (Fedora: `qt6-qtwebengine qt6-qtwebchannel`), and for the overlay
  on Wayland compositors with wlr-layer-shell, LayerShellQt (Fedora: `layer-shell-qt`). Without LayerShellQt the app
  falls back to a plain window.
- Setup tools: `python3`, `7zip` (provides `7z`), `brotli`; network once for `fonttools`
- Runtime extras: `xdg-utils`, `desktop-file-utils`

```sh
git clone https://github.com/crazyads69/wuwa-tappo-linux-experimental.git
cd wuwa-tappo-linux-experimental

# 1. Unpack and patch the frontend from your own installer (needs network once, for the font tool)
scripts/setup.sh /path/to/WuwaTappo-global_1.0.1_x64-setup.exe

# 2. Build, test and run
cmake -S qt -B qt/build -G Ninja && ninja -C qt/build
ctest --test-dir qt/build --output-on-failure
qt/build/wuwatappo-qt                      # finds ./dist, or set WUWA_DIST=/path/to/dist
```

The browser login returns through a `wuwa-buddy://` link. The app does not register itself as the handler; the
desktop entry in `qt/data/` does that once it is installed (`cmake --install qt/build --prefix ~/.local`, then
`update-desktop-database ~/.local/share/applications`; that exact sequence was not tested). If the app is already
running, a second start only forwards the link to it.

Where things are stored: `~/.local/share/com.kuro.wuwatappo.global/` (QtWebEngine data including the login
session, counters in `store.json`). The directory name is the original app's identifier and is shared with it.

## Risks, please read

- **Account risk.** Kuro's Terms of Use (effective 29 September 2025,
  [text](https://sdk-global.kurogames.com/pro/agreement/common/language_en/terms_of_service)) ban "unauthorized
  third-party programs" and let Kuro suspend or terminate accounts "without notice". Whether a Linux shell for this
  companion app counts, and whether Kuro can even tell, is unknown. I found no reports of bans for tools like this,
  and I cannot promise there will be none.
- **Multiplayer connection.** Kuro's socket server refuses browser connections from this app's origin, so the app
  opens that one connection natively: it connects only to Kuro's own socket host, sends no `Origin` header and no
  cookie, and identifies itself as `wuwatappo-qt/<version> (unofficial)`. The same session token the official
  app would send is used. See [docs/how-it-works.md](docs/how-it-works.md).
- **Your keystrokes.** To count global key presses and clicks like the Windows app does, the app reads your input
  devices through `/dev/input` (read-only, it never grabs or changes them). It forwards only key names to the
  frontend and stores only counters, never typed text. You can switch this off with `WUWA_NO_GLOBAL_INPUT=1`, and
  you should read [`qt/src/inputmonitor.cpp`](qt/src/inputmonitor.cpp) before trusting that claim.
- **Your login.** The frontend keeps its session token in the browser storage of the app, as the official one does.
  Anyone who can read your user profile can read it. Do not paste tokens, cookies or `wuwa-buddy://` URLs into
  issues. See [SECURITY.md](SECURITY.md).
- **Telemetry.** The official frontend contacts Kuro's servers and includes its own analytics. This project does not
  remove them.
- **Trademarks.** "Wuthering Waves", "Wuwa Tappo", "Kuro Games" and "Bongo Cat" belong to their owners. They are
  used here only to say what this software works with.

## Environment switches

| Variable | Effect |
|---|---|
| `WW_NO_LAYER=1` | Use a maximized always-on-top window instead of a layer-shell overlay. |
| `WW_DEBUG_WINDOW=1` | Use an ordinary 900x700 pet window (safe on a live desktop). |
| `WUWA_NO_GLOBAL_INPUT=1` | Do not read `/dev/input` and do not track the pointer outside the pet. |
| `WUWA_NO_TAP=1` | Turn off touchpad tap detection. |

## Documentation

- [How it works](docs/how-it-works.md): the architecture in plain words
- [Qt port notes](docs/qt-port/FEATURES.md): feature list, [verification matrix](docs/qt-port/VERIFICATION.md) and [open unknowns](docs/qt-port/UNKNOWNS.md)
- [Compatibility and test notes](docs/compatibility.md): what was tested, how, and what was not
- [Development guide](docs/development.md): build, tests, patches, how to test without touching your own desktop
- [Related projects](docs/related-projects.md): other ports and similar tools. **Another Linux port exists** that
  runs the original Windows build under Wine; it is a different approach and may suit you better.

## Contributing

Bug reports, test results from other systems and patches are very welcome, and more so than features. Read
[CONTRIBUTING.md](CONTRIBUTING.md) first; it explains what must never be added to the repository. Everyone taking
part is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).

## License and notices

MIT, see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No warranty of any kind.
