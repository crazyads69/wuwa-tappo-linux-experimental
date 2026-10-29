# Compatibility and test notes

This is the honest version of "does it work". Every statement says how it was checked. If something is not listed,
assume it was **not** tested.

## The one machine everything was tested on

- Fedora Asahi Remix 44, aarch64 (Apple Silicon laptop)
- KDE Plasma 6.7.5 on Wayland, KWin 6.7.5, one built-in display (3414x2134 physical, fractional scale 1.5)
- WebKitGTK 2.54.2, GTK 3.24.52, gtk-layer-shell 0.10.0
- Wuwa Tappo global 1.0.1 installer, a real account used by the author for the login test

Nothing here says the project works on x86_64, other distributions, other compositors or other hardware. That
needs your help to find out. Test reports are welcome (see [CONTRIBUTING.md](../CONTRIBUTING.md)).

## Checked methods

- **Author's own use**: running the app on the machine above.
- **Automated tests**: `cargo test` (33 Rust tests) and `node --test` (21 tests of the JavaScript shims and
  patched frontend code). They test logic, not the real desktop.
- **Protocol inspection**: running the app with `WAYLAND_DEBUG=client` and reading what it tells the compositor
  (`tools/trace_input_regions.py`). This proves what the app *requests*, not what every compositor does with it.
- **Isolated sessions**: a private D-Bus session and a virtual KWin (`kwin_wayland --virtual`) for multi-monitor
  and window tests, so the author's real desktop was not touched.
- **Synthetic input**: a temporary kernel `uinput` device for key events. It is not a certification of real
  keyboards or touchpads.

## Results

| Area | Result | Checked by |
|---|---|---|
| Overlay always on top, no focus, no taskbar entry | Works | Author's use; layer-shell surface seen in protocol log |
| Pet visible over a fullscreen window | Works once observed | Author's session (a fullscreen video was on screen) |
| Click-through outside the pet | Works as designed | Protocol log shows an input region of a few small rectangles. Not verified with every compositor. |
| Toast overlay never blocks clicks | Fixed 2026-10-30 | Protocol log. Before the fix a re-shown toast kept a full-screen input region for several seconds. |
| Click and key counting everywhere | Works | Author's use (counters rise). Needs readable `/dev/input/event*`. |
| Key repeat (holding a key) | Counted once per key press, like the original frontend expects | Synthetic input; about 117 auto-repeats gave one press and one release |
| Touchpad tap-to-click counting | Implemented, **unconfirmed on hardware** | Unit tests only |
| Login via browser and `wuwa-buddy://` | Worked once (2026-10-28) | Author's real account; deep-link path also tested with a local fake callback |
| Multiplayer socket connects | Works | Author's session: the app log shows `[SOCKET] connected`. Before the native client it failed with `connect_error`; direct handshake tests showed HTTP 403 for `Origin: tauri://localhost` and 101 for no Origin or `http://tauri.localhost`. The native client has unit tests against a local server. **Room create/join and chat were not tested.** |
| Touchpad tap plays the pet animation | Implemented, **unconfirmed on hardware** | A tap now holds the press for 100 ms before the release (a back-to-back press and release counted but never showed the pose). Unit-tested only. |
| Panel tabs render | Works | Author's use; DOM measurements |
| Synthesis tab stale pixels | Fixed | A screenshot comparison fixture (`tools/check-synthesis-paint.py`) failed before the fix and passes after |
| F8 monitor switch | Works in a virtual 2-output session | Isolated KWin. **No real external monitor tested.** |
| F7/F8/F9 as system-wide KDE shortcuts | Works in an isolated session | Synthetic key injection into a private virtual KWin. While the app runs on KDE, **F7, F8 and F9 are reserved by it and other apps do not receive them**, as with the Windows app. Keys already owned by another shortcut are left alone. |
| Panel auto-hide detection, fullscreen detection | Implemented | Plasma scripting queries; limited real-world testing |
| Maintenance notice | Implemented | Triggered locally in an isolated session. A real maintenance response from Kuro's servers was never captured. |
| Automatic update | Not supported | By design |
| Rewards, exchange, redeem codes, multiplayer rooms | **Not tested** | These change server data |
| Languages other than English | **Not verified visually** | Font selection checked by script |
| GNOME, X11, Hyprland, Sway, x86_64 | **Not tested** | |

## Known problems and limits

- **Memory use is high.** The pet overlay covers the whole screen and holds a large amount of graphics memory (about
  200 MB in one early measurement, which does not show in normal RSS numbers). One snapshot with the panel open
  showed about 400 MB across the app and its WebKit processes. Opening extra windows adds more.
- **Some flashing may remain** when windows open or tabs change. Windows are now hidden until the page reports it is
  ready, and fonts are preloaded, but this was judged by measurements and screenshots, not by a camera.
- **Same kind of painting bug elsewhere is possible.** The stale pixels in Synthesis were caused by reusing list
  elements in WebKitGTK; other lists may behave the same.
- **F7, F8 and F9 are taken from other apps** while this app runs on KDE (they are common keys in browsers and IDEs). Quit the app to get them back.
- **Layer-shell surfaces have no taskbar entry**, so the frontend's "show taskbar icon" setting has no effect.
- **Normal windows (panel, About, login) cannot be forced above fullscreen apps** on Wayland.
- **Pointer position outside the pet** is only known on KDE Wayland and X11. Elsewhere the app only sees the pointer
  over the pet.
- **GNOME** has no layer-shell. A maximized-window fallback exists (`WW_NO_LAYER=1` selects it) but it was not
  tested.
- **Counters are not compatible with the Windows app's saved files.**
- **The About window opens at every start** until you tick "Do not show again" (this is the frontend's behaviour).
- **The first ~100-300 ms after start**, before the app narrows its input region, the main surface may cover the
  screen. It is brief, but it is real.

## A bug worth knowing about

A full-screen layer-shell surface that is meant to be click-through can accidentally capture every click. On
2026-10-30 the author found that the toast overlay, once re-shown, kept the compositor's default "whole surface
accepts input" region for several seconds, because the compositor only applies a new input region on the next
redraw and an idle page does not redraw. The toast surface is now created once and never unmapped, and the app
clears its input region when it is first mapped. If you ever find that clicks do not reach other apps while this app
runs, please open an issue and include the output of `tools/trace_input_regions.py` (see
[development.md](development.md)).
