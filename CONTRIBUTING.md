# Contributing

Thanks for looking at this project. It is a small, experimental, hobby project and every kind of help is useful:
test reports from other systems, bug reports, documentation fixes and code.

Please read the [Code of Conduct](CODE_OF_CONDUCT.md). Be kind; people here are learning.

## The one hard rule: no Kuro material in the repository

This repository must never contain anything that comes from the official app or its installer. That keeps the
project safe to host and honest about what it is. **Do not commit, attach to issues or pull requests, or paste:**

- the installer, `WuwaTappo.exe`, or any binary built from this repository (binaries embed the frontend)
- anything under `dist/` (the extracted frontend: JavaScript, CSS, HTML, images, fonts, icons, sounds)
- the app icon, character art, or screenshots that show the app's characters or artwork
- large pieces of the official JavaScript. Patches must stay small find-and-replace edits with a few characters of
  context (`scripts/patch_frontend.py make` does this for you; check the result)
- login tokens, cookies, session data, `store.json`, logs that contain them, or `wuwa-buddy://` URLs with
  parameters

`.gitignore` already blocks most of this. If you are unsure, ask in an issue before committing. If something slipped
in, tell a maintainer right away so it can be removed from the history too.

## Useful contributions, roughly in order of value

1. **Test reports.** Ran it on another distribution, desktop, GPU or CPU architecture? Tell us what worked and what
   did not, using the bug report form. Even "it started and the pet showed up" helps.
2. **Bugs with evidence.** Logs, the output of `tools/trace_input_regions.py`, your versions (see the form).
3. **Fixes.** Small, focused pull requests with a clear description of the problem and how you checked the fix.
4. **Docs.** Anything that confused you.
5. **New features** last. Please open an issue to discuss before writing a lot of code.

## Before you open a pull request

- Run the checks from the [development guide](docs/development.md): `cargo test --release --locked` and
  `node --test tests/*.test.cjs` from `src-tauri/`.
- If you touched windows, layer-shell or input regions, run the click-swallowing check described there and say so.
- Say what you tested **on what system**. "Tested on KDE Wayland only" is a perfectly good statement.
- Do not state things you did not verify. This project prefers "not tested" over a guess.
- Keep the change small. Unrelated clean-ups go in separate pull requests.
- Do not add new dependencies without a reason in the description.

`main` is protected: changes go through pull requests, force pushes are blocked and history stays linear.

## Reporting security problems

Not here in public. See [SECURITY.md](SECURITY.md).

## License

By contributing you agree that your contribution is licensed under the repository's MIT license.
