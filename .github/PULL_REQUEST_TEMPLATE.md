## What and why

<!-- What problem does this solve? Link the issue if there is one. -->

## How I checked it

<!-- Commands you ran and the system you tested on. "Not tested on X" is fine to say. -->

- [ ] `ctest --test-dir qt/build --output-on-failure` passes
- [ ] If I touched windows, layer-shell or input regions: I ran `tools/trace_input_regions.py` and no full-screen
      surface keeps a large input region

## Safety checklist

- [ ] No installer, extracted frontend, artwork, fonts, icons or screenshots of the app's art are included
- [ ] No tokens, cookies, logs with personal data or `wuwa-buddy://` URLs are included
- [ ] Changes to `patches/frontend.json` are small find-and-replace edits (no large pieces of official code)
