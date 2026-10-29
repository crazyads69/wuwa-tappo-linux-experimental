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
attempt to run it natively on Linux by re-implementing the Windows-only part of the app and reusing its web
frontend, which stays Kuro's property.

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

Everything below was checked by the author on one machine: **Fedora Asahi Remix 44 (aarch64, Apple Silicon), KDE
Plasma 6.7 on Wayland, WebKitGTK 2.54.2.** "Tested" means that on that machine, and nothing more. Other desktops,
distributions and hardware are unknown territory. The full table, with how each item was checked, is in
[docs/compatibility.md](docs/compatibility.md).

| Area | State |
|---|---|
| Pet shown as a transparent, always-on-top overlay on KDE Wayland | Works. It stayed visible above a fullscreen window in the author's session. |
| Clicks outside the pet go to the apps underneath | Works as designed, verified by inspecting what the app tells the compositor (see [How it works](docs/how-it-works.md)). A bug that made a hidden full-screen overlay swallow clicks was found and fixed on 2026-10-30. Please report anything that still blocks clicks. |
| Counting clicks and key presses anywhere on screen | Works on the author's machine (needs read access to `/dev/input`). Touchpad tap-to-click detection exists but is only unit-tested, not confirmed on hardware. |
| Panel (Backpack, Gallery, Synthesis, Multiplayer, Settings) | Opens and renders. Chest, redeem-code, exchange and multiplayer actions that change server data were **not** exercised. |
| Login through the browser (`wuwa-buddy://` callback) | Worked once for the author on 2026-10-28. Not re-verified since. |
| Multiplayer connection | The socket now connects (`[SOCKET] connected` in the log; it used to fail with HTTP 403 because of the browser origin). Creating or joining rooms and chatting with other players were **not** tested. |
| Switching the pet between monitors with F8 | Tested only in a virtual two-monitor KDE session, never on real external monitors. |
| F7/F8/F9 hotkeys | Implemented through KDE's shortcut system, tested in an isolated session. **While the app runs, other apps no longer receive these three keys.** |
| Automatic updates | Not supported, on purpose. The app never downloads or runs a Windows installer. |
| GNOME, X11, Hyprland, Sway, other distributions | **Untested.** GNOME has no layer-shell support, so only a fallback window mode could work there. |
| Languages other than English | Fonts and layout were checked by script, not by eye. Unverified. |

Known problems: memory use is high (the full-screen overlay alone holds a lot of graphics memory), and some
flashing during window changes may remain. See [docs/compatibility.md](docs/compatibility.md) for the honest list.

## Quick start

You need the **Wuwa Tappo global 1.0.1** Windows installer (`WuwaTappo-global_1.0.1_x64-setup.exe`). The setup
script checks its SHA-256 and refuses any other version, because the patches are written for that exact build.

Requirements (tested on Fedora; package names on other distributions are not tested):

- Build packages: `rust cargo webkit2gtk4.1-devel gtk3-devel gtk-layer-shell-devel libappindicator-gtk3-devel
  librsvg2-devel openssl-devel libxdo-devel`
- Tools: `python3`, `7zip` (provides the `7z` command), `brotli`
- Runtime: `xdg-utils`, `desktop-file-utils`
- Optional: `nodejs` (only to run the frontend tests)

```sh
git clone https://github.com/crazyads69/wuwa-tappo-linux-experimental.git
cd wuwa-tappo-linux-experimental

# 1. Unpack and patch the frontend from your own installer (needs network once, for the font tool)
scripts/setup.sh /path/to/WuwaTappo-global_1.0.1_x64-setup.exe

# 2. Build and run
cd src-tauri
cargo build --release --locked
./target/release/wuwatappo-linux
```

Keep `Cargo.lock`: it pins one crate (`rust-ini` 0.21.1) because 0.21.3 breaks the build of the deep-link plugin.

The first start registers this executable as the handler for `wuwa-buddy://` links, which is how the browser login
returns to the app. If you move the binary, start it once from its new location.

Where things are stored: `~/.local/share/com.kuro.wuwatappo.global/` (WebKit data including the login session,
counters in `store.json`, a log in `logs/`).

## Risks, please read

- **Account risk.** Kuro's Terms of Use (effective 29 September 2025,
  [text](https://sdk-global.kurogames.com/pro/agreement/common/language_en/terms_of_service)) ban "unauthorized
  third-party programs" and let Kuro suspend or terminate accounts "without notice". Whether a Linux shell for this
  companion app counts, and whether Kuro can even tell, is unknown. I found no reports of bans for tools like this,
  and I cannot promise there will be none. The app identifies itself to Kuro's servers as running on Linux.
- **Multiplayer connection.** Kuro's socket server refuses browser connections from this app's origin, so the app
  opens that one connection natively: it connects only to Kuro's own socket host, sends no `Origin` header and no
  cookie, and identifies itself as `wuwatappo-linux/<version> (unofficial)`. The same session token the official
  app would send is used. See [docs/how-it-works.md](docs/how-it-works.md).
- **Your keystrokes.** To count global key presses and clicks like the Windows app does, the app reads your input
  devices through `/dev/input` (read-only, it never grabs or changes them). It forwards only key names to the
  frontend and stores only counters, never typed text. You can switch this off with `WUWA_NO_GLOBAL_INPUT=1`, and
  you should read [`src-tauri/src/input.rs`](src-tauri/src/input.rs) before trusting that claim.
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
| `WUWA_NO_GLOBAL_INPUT=1` | Do not read `/dev/input` and do not track the pointer outside the pet. |
| `WUWA_NO_TAP=1` | Turn off touchpad tap detection. |

Do **not** set `WEBKIT_DISABLE_DMABUF_RENDERER=1`. On the tested WebKitGTK it disabled compositing and made
transparent windows lose parts of their content after scrolling.

## Documentation

- [How it works](docs/how-it-works.md): the architecture in plain words
- [Compatibility and test notes](docs/compatibility.md): what was tested, how, and what was not
- [Development guide](docs/development.md): tests, patches, how to test without touching your own desktop
- [Related projects](docs/related-projects.md): other ports and similar tools. **Another Linux port exists** that
  runs the original Windows build under Wine; it is a different approach and may suit you better.

## Contributing

Bug reports, test results from other systems and patches are very welcome, and more so than features. Read
[CONTRIBUTING.md](CONTRIBUTING.md) first; it explains what must never be added to the repository. Everyone taking
part is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).

## License and notices

MIT, see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). No warranty of any kind.
