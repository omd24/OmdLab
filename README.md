# OmdLab

A hobby game engine built on DirectX 12 (AI-assisted programming), driving a 2D-camera fighting game with minimalistic 3D characters.

[![Prototype video](https://img.youtube.com/vi/jhIUb1tT0B0/sddefault.jpg)](https://youtu.be/jhIUb1tT0B0)

*Prototype showcase video (click to watch on YouTube).*

## Highlights

- **Custom tech, minimal dependencies**: own DX12 renderer, skeletal animation runtime, collision, input and fixed-timestep simulation. Third-party code is limited to EnTT, Dear ImGui, cgltf, stb and miniaudio.
- **Data-driven design**: a hand-written combat DSL (`.combat`) defines states, moves, hitboxes, cancels and frame data; key bindings are plain text. Both hot-reload while running.
- **Animation tooling**: an in-engine animation editor (move preview and frame scrubbing, hitbox placement, clip-timing curves, all saved back to the `.combat` files), on top of a runtime with crossfades, blend-space locomotion and bone-attached hitboxes. A headless capture harness provides deterministic regression checks and metrics.
- **Gameplay layers**: SFX and music, CPU particle VFX, an in-game HUD and menus, and a rule-based AI opponent (including an AI-vs-AI mode with a seeded, repeatable fight director).

## Architecture

```mermaid
graph BT
    Foundation["Foundation<br/>logging · memory · threading · containers · platform/window"]
    Renderer["Renderer<br/>DX12 backend · GPU resources · render passes"]
    Asset["Asset<br/>glTF import · engine-agnostic mesh/skeleton/clip data"]
    Engine["Engine<br/>ECS · animation · input · collision · audio · particles · resource binding"]
    Game["Game<br/>fighting-game logic, not reusable"]

    Foundation --> Renderer
    Foundation --> Asset
    Renderer --> Engine
    Asset --> Engine
    Engine --> Game

    classDef foundation fill:#2b6cb0,color:#ffffff,stroke:#1a365d,stroke-width:1px;
    classDef mid fill:#2f855a,color:#ffffff,stroke:#22543d,stroke-width:1px;
    classDef engine fill:#b7791f,color:#ffffff,stroke:#975a16,stroke-width:1px;
    classDef game fill:#c53030,color:#ffffff,stroke:#822727,stroke-width:1px;

    class Foundation foundation;
    class Renderer mid;
    class Asset mid;
    class Engine engine;
    class Game game;
```

| Project | Role | Depends on |
|---|---|---|
| `Foundation` | Platform layer, logging, memory tracking, threading, core containers. | — |
| `Renderer` | Generic DX12 abstraction: device, GPU resources, render passes, pipelines. No knowledge of game or asset concepts. | `Foundation` |
| `Asset` | Import-format-agnostic mesh/skeleton/animation data structures, populated by a glTF importer. | `Foundation` |
| `Engine` | ECS, animation runtime, input, collision, audio, particles, fixed-timestep simulation, and the resource layer connecting `Asset` data to `Renderer` GPU resources. | `Foundation`, `Renderer`, `Asset` |
| `Game` | Fighting-game-specific logic and content: combat DSL, fighter state machine, AI, HUD, VFX/SFX cues. Not reusable. | `Engine` |

## Building

1. Run `GenerateSolution.bat` from the repo root to generate the Visual
   Studio solution/project files under `projects/` (regenerate any time
   `build/*.sharpmake.cs` changes).
2. Open `projects/omdlab_win64_vs2022.sln` and build. The first build
   requires internet access: it restores the DirectX Agility SDK via NuGet
   and copies its runtime DLLs next to the built executable.

## Status

Playable prototype: two fighters with combos, cancels, aerials, blocking, a fireball special, HUD, hit effects, sound, and a CPU opponent. Work in progress; next steps are not yet scheduled.

## License

Source code is licensed under the [MIT License](LICENSE). This does not
cover everything in the repository — see [NOTICE.md](NOTICE.md) for
vendored third-party code and data assets under their own separate
licenses.
