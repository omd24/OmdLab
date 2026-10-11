This repository's own source code (`src/`, `build/`) is licensed under the
MIT License — see `LICENSE`.

That grant does not extend to everything else in the repository:

## Third-party vendored source (`thirdParty/`)

Each library keeps its own license, included alongside it:

- `thirdParty/cgltf/` — MIT (`cgltf/LICENSE`)
- `thirdParty/imgui/` — MIT (`imgui/LICENSE.txt`)
- `thirdParty/miniaudio/` — public domain / MIT-0 (`miniaudio/LICENSE`)
- `data/audio/ko_yell2.wav`, `attack_yell.wav`, `unused/*` — derived from "Male Grunt/Yelling sounds" by HaelD (https://opengameart.org/content/male-gruntyelling-sounds), CC0 1.0
- `data/audio/whiff_cc0_*.wav` — derived from "Swishes Sound Pack" by artisticdude (https://opengameart.org/content/swishes-sound-pack), CC0 1.0
- `data/audio/hit_cc0_*.wav` — derived from Kenney "Impact Sounds" (https://kenney.nl/assets/impact-sounds), CC0 1.0
- `thirdParty/sharpmake/` — Apache License 2.0 (`sharpmake/LICENSE.md`)

The DirectX Agility SDK and DirectX Shader Compiler are pulled in via NuGet
at build time (not vendored as source) and redistributed under Microsoft's
own SDK license terms.

## Data assets (`data/`)

Not everything under `data/` is original work. Third-party-sourced assets
carry their own license and required attribution in a `CREDIT.txt` file next
to them — check that file before reusing an asset outside this project.
Currently:

- `data/characters/polyone_stick_man/` — model by PolyOne Studio, licensed
  CC-BY-4.0; see `CREDIT.txt` in that folder for the required attribution
  text.

Everything else under `data/` (shaders, etc.) is original and covered by
the top-level MIT `LICENSE`.
