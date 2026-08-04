This repository's own source code (`src/`, `build/`) is licensed under the
MIT License — see `LICENSE`.

That grant does not extend to everything else in the repository:

## Third-party vendored source (`thirdParty/`)

Each library keeps its own license, included alongside it:

- `thirdParty/cgltf/` — MIT (`cgltf/LICENSE`)
- `thirdParty/imgui/` — MIT (`imgui/LICENSE.txt`)
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
