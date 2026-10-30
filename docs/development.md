# Development guide

## Layout

```
README.md, LICENSE, CODE_OF_CONDUCT.md, CONTRIBUTING.md, SECURITY.md, THIRD_PARTY_NOTICES.md
scripts/   setup.sh, extract_frontend.py, patch_frontend.py   (turn your installer into dist/)
           re/ (Ghidra/REA helper scripts), nest/ (nested-session test tooling: fake input, DevTools client)
patches/   frontend.json (the text patches), assets/ (one license text)
qt/        the Qt 6 app: src/ (C++), qml/, js/ (page-side shims), tests/ (QtTest), data/ and packaging/
tools/     checks and helpers (font subsetting, patch checks, input-region tracer, paint fixture)
dist/      LOCAL ONLY, git-ignored: the patched frontend produced by setup.sh
docs/      these pages; docs/qt-port holds the reverse-engineering notes and the verification matrix
```

`dist/` is created by `scripts/setup.sh`. A fresh clone cannot run until you ran it (the unit tests do not need it).

## Build and test

```sh
scripts/setup.sh /path/to/WuwaTappo-global_1.0.1_x64-setup.exe   # once
cmake -S qt -B qt/build -G Ninja && ninja -C qt/build
ctest --test-dir qt/build --output-on-failure                    # 6 QtTest suites, no display needed
node tools/check-panel-patches.cjs                               # checks the panel patches (reads dist/; needs nodejs)
```

Build needs Qt 6.5+ (Core Gui Network Qml Quick DBus Widgets Test). QtWebEngine and LayerShellQt are linked at run
time; their `-devel` packages are optional (see `qt/CMakeLists.txt`). Requirements are listed in the README.

Setup is deterministic: running it on a clean checkout reproduces `dist/` exactly (the author compared the result
byte for byte against a working tree, 134 files). `python3 scripts/patch_frontend.py verify dist` re-checks it.

## Changing the frontend patches

Never edit files in `dist/` and commit them; `dist/` is git-ignored on purpose. To change a patch:

1. Run `scripts/setup.sh` to get a patched `dist/`.
2. Extract a pristine copy: `python3 scripts/extract_frontend.py INSTALLER.exe /tmp/pristine`
3. Edit the file in `dist/`.
4. Regenerate the patch file: `python3 scripts/patch_frontend.py make /tmp/pristine dist`
5. Review `git diff patches/frontend.json`: it should contain only your small change, with short context. If you see
   large chunks of the original code there, something is wrong; do not commit it.

Several patches were written for WebKitGTK (the earlier shell). They have not all been re-validated under
QtWebEngine (Chromium); see U4 in [qt-port/UNKNOWNS.md](qt-port/UNKNOWNS.md).

## Checking that an overlay does not swallow clicks

This is the most important check for any change that touches windows, layer-shell or input regions.

```sh
WAYLAND_DEBUG=client qt/build/wuwatappo-qt 2> wayland.log   # use the app, then quit
python3 tools/trace_input_regions.py wayland.log
```

Every full-screen surface (the pet, the toast) must end up with an input region of a few small rectangles or none.
A region whose area is roughly the whole screen means clicks are being swallowed.

## Debug switches

`WW_DEBUG_WINDOW=1` opens an ordinary 900x700 pet window (safe on a live desktop). `--debug-js` prints the page
console and a frame probe (needs `QT_LOGGING_RULES="qml.debug=true"`). `WW_TRACE_IPC=1` logs command names.
`WW_GRAB_DIR`/`WW_GRAB_MS` and `WW_DEV_OPEN=<command>` are described in [qt/README.md](../qt/README.md).

## Testing without disturbing your own desktop

Opening windows, shortcuts and monitors can interfere with your session. The verification matrix in
[qt-port/VERIFICATION.md](qt-port/VERIFICATION.md) was produced in a throwaway session:

- `unshare -rn` (loopback only, no network) with a private `HOME`/`XDG_RUNTIME_DIR` and `dbus-run-session`
- `kwin_wayland --virtual --output-count 2 --width 1280 --height 720`, with `/dev/input` masked
- input injected with `scripts/nest/fake_input.c` (KWin's fake-input protocol, see `fake_input.xml`) and page state
  read with `scripts/nest/cdp.py` (DevTools on loopback)

These are helpers, not a finished harness: expect to adapt them. Do not leave a debugging port open on an instance
that holds your real login.

## Releasing

There are no binary releases and there should not be any: a binary or package that includes `dist/` carries the
frontend, which is not ours to distribute. Source only. (`qt/CMakeLists.txt` installs `dist/` if present; do that
only for your own machine. The Flatpak manifest in `qt/packaging/` is an untested sketch.)
