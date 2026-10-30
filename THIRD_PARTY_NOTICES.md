# Third-party notices

This repository's own code is MIT licensed (see [LICENSE](LICENSE)). It builds on the following, all taken
from your system rather than copied into this repository.

| Component | License | How it is used |
|---|---|---|
| [Qt 6](https://www.qt.io) (Core, Gui, Network, Qml, Quick, DBus, Widgets; Test for the unit tests) | LGPL-3.0 / GPL / commercial (Qt is multi-licensed; the modules used here are available under LGPL-3.0) | Dynamically linked from the system |
| Qt WebEngine (built on Chromium) and Qt WebChannel | LGPL-3.0 / GPL (Qt modules); Chromium and its bundled components carry their own licenses (mostly BSD-style, see your distribution's package) | Loaded at run time from the system; runs the frontend |
| [LayerShellQt](https://invent.kde.org/plasma/layer-shell-qt) | LGPL-3.0-or-later / GPL (see its package) | Optional, loaded at run time from the system for the layer-shell overlay |
| wayland-client, libxcb (xcb-shape) | MIT | Optional, dynamically linked if found at build time |
| [fonttools](https://github.com/fonttools/fonttools) 4.68.0 | MIT | Used only by `scripts/setup.sh` in a temporary virtual environment, to build two font files locally |
| [Contributor Covenant](https://www.contributor-covenant.org) 2.1 | CC BY 4.0 | `CODE_OF_CONDUCT.md` |
| SIL Open Font License 1.1 text in `patches/assets/` | OFL-1.1 | Accompanies the two Latin font subsets that setup generates on your computer from the app's own open-licensed fonts |

## Not included and not licensed here

The official Wuwa Tappo application, its installer and everything extracted from them (code, artwork, fonts, icons,
sounds) belong to Kuro Games and their licensors. "Wuthering Waves", "Wuwa Tappo", "Kuro Games" and "Bongo Cat" are
names of their respective owners and are used only to describe what this software works with. This project is not
affiliated with or endorsed by any of them.
