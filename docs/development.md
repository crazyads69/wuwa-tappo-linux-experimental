# Development guide

## Layout

```
README.md, LICENSE, CODE_OF_CONDUCT.md, CONTRIBUTING.md, SECURITY.md, THIRD_PARTY_NOTICES.md
scripts/   setup.sh, extract_frontend.py, patch_frontend.py   (turn your installer into dist/)
patches/   frontend.json (the text patches), assets/ (one license text)
tools/     checks and helpers (font subsetting, patch checks, input-region tracer, paint fixture)
src-tauri/ the Rust app: src/, tests/ (JavaScript tests), capabilities/, tauri.conf.json
dist/      LOCAL ONLY, git-ignored: the patched frontend produced by setup.sh
docs/      these pages
```

`dist/` and `src-tauri/icons/` are created by `scripts/setup.sh`. A fresh clone cannot be built until you ran it.

## Build and test

```sh
scripts/setup.sh /path/to/WuwaTappo-global_1.0.1_x64-setup.exe   # once
cd src-tauri
cargo build --release --locked
cargo test --release --locked            # Rust tests
node --test tests/*.test.cjs             # JavaScript tests (they read dist/, so run setup first)
node ../tools/check-panel-patches.cjs    # checks the panel patches
```

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

## Checking that an overlay does not swallow clicks

This is the most important check for any change that touches windows, layer-shell or input regions.

```sh
WAYLAND_DEBUG=client ./src-tauri/target/release/wuwatappo-linux 2> wayland.log   # use the app, then quit
python3 tools/trace_input_regions.py wayland.log
```

Every full-screen surface (the pet, the toast) must end up with an input region of a few small rectangles or none.
A region whose area is roughly the whole screen means clicks are being swallowed.

## Testing without disturbing your own desktop

Opening windows, shortcuts and monitors can interfere with your session. The author's automated runs used:

- a private D-Bus session: `dbus-run-session -- ...` (also avoids the single-instance guard of an already running
  copy)
- a virtual compositor: `kwin_wayland --virtual --output-count 2 --socket NAME` and pointing `WAYLAND_DISPLAY` at it
- `WUWA_NO_GLOBAL_INPUT=1` so the test app does not read your real keyboard

These flags exist in KWin 6.7 (checked with `--help`). The exact recipes were not kept as scripts, so expect to
adjust them. Contributions that turn them into a script are welcome.

Note that starting any build registers that executable as the `wuwa-buddy://` handler, so after testing a
temporary build, start the build you actually use once to put the registration back.

## Releasing

There are no binary releases and there should not be any: a binary embeds the frontend, which is not ours to
distribute. Source only.
