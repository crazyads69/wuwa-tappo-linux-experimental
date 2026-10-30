# Wuwa Tappo, native Qt 6 shell (experimental)

See [docs/qt-port/FEATURES.md](../docs/qt-port/FEATURES.md) and [UNKNOWNS.md](../docs/qt-port/UNKNOWNS.md) first:
**the frontend has not been seen rendering on the dev machine (native Wayland delivers no frames there).**

```sh
scripts/setup.sh /path/to/WuwaTappo-global_1.0.1_x64-setup.exe     # unpacks dist/ locally 
cmake -S qt -B qt/build -G Ninja && ninja -C qt/build
ctest --test-dir qt/build --output-on-failure                       # 6 QtTest suites, no display needed
qt/build/wuwatappo-qt                                               # finds ../dist, or set WUWA_DIST=...
```

Build needs Qt 6.5+ (Core Gui Network Qml Quick DBus Widgets Test); at run time QtWebEngine (QML), QtWebChannel (QML)
and, on Wayland compositors with wlr-layer-shell, LayerShellQt. Optional: `wayland-client`, `xcb`/`xcb-shape` dev files.
The `-devel` packages of WebEngine/LayerShellQt are not required to build.

Environment knobs: `WW_NO_LAYER=1` plain window instead of layer-shell, `WW_DEBUG_WINDOW=1` ordinary 900x700 pet window
(safe on a live desktop), `WUWA_NO_GLOBAL_INPUT=1`, `WUWA_NO_TAP=1`, `WW_TRACE_IPC=1` (command names), `--debug-js`
(page console + DOM/frame probe; needs `QT_LOGGING_RULES="qml.debug=true"`), `WW_GRAB_DIR`/`WW_GRAB_MS`, `WW_DEV_OPEN=<command>`.
