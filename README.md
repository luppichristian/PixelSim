# PixelSim

A small **Noita-style cellular-material sandbox** for Windows, implemented in C with D3D11 and built through [bbs](https://github.com/luppichristian/bbs).

![GPU pipeline](https://img.shields.io/badge/simulation-D3D11%20compute%20shader-2ea44f) ![Language](https://img.shields.io/badge/language-C-555555)

## Run

```bash
bbs run -t pixelsim_app
```

Run from the repository root: the executable compiles `assets/shaders/pixelsim.hlsl` at startup.

- Click a colored **material swatch** in the two-row D3D palette, or press **1–9** / **A–D**, to select a material. The selected swatch has a white border.
- Click once to submit one bounded brush command; hold and move to paint continuously. Input is mapped through the centered 16:9 simulation viewport, including after a window resize.
- The window title reports the selected material and live presentation FPS (updated once per second).

## Design

The simulation is a 640 × 360 pair of `R32_UINT` GPU textures. Physics advances at an exact fixed 120 updates per second, independently of presentation FPS, with at most four catch-up updates after a stall. The low byte of each cell stores its material, and falling powders and liquids carry vertical velocity in a packed metadata byte.

Each update uses five conflict-free compute dispatches. A 2 × 2 checkerboard material pass handles diagonal powder rolls, gases, displacement, and reactions, and a column-owned pass applies vertical acceleration to powders and liquids. Three in-place row-color dispatches then transfer liquid pressure; active rows are separated by three cells, so their two-row support footprints never overlap. Every active row is loaded cooperatively into group-shared memory, obstacle segments are identified in parallel, and shared atomics select at most one conservative source/destination swap per segment. All six color orders rotate across updates to balance ordering bias. Airborne liquid falls straight instead of treating other falling liquid as stable support, while a pool whose column depths differ by at most one cell is a stable discrete equilibrium. Vertical movement ray-marches every crossed cell and stops before obstacles, horizontal pressure never crosses a solid wall, and lava stops at water so accelerated motion cannot skip the reaction. The pixel shader masks the packed state and samples the resulting texture directly in a full-screen-triangle draw.

The CPU never uploads the world texture. Mouse events enqueue 20-byte `sim_brush_command` values; an in-place GPU `ApplyBrush` dispatch touches only the clipped brush rectangle before the usual physics passes. Holding the mouse still does not repeatedly deposit material.

| Family | Materials | Behavior |
| --- | --- | --- |
| Solid | wood, iron, bedrock | Iron is immovable until adjacent rust or acid erodes it. Bedrock is permanent and resists heat, rust, and acid. |
| Powder | sand, rust, salt | Powders accelerate, roll, and sink through less-dense liquids. Rust propagates into iron; salt grains gradually dissolve into adjacent water and turn it into visibly lighter brine. |
| Liquid | water, lava, acid, oil | Liquids accelerate and level under pressure. Water cools lava and extinguishes fire, green acid dissolves ordinary materials, and buoyant oil floats on water and burns. |
| Gas | smoke, steam, fire | Gases rise and drift. Steam can condense; fire spreads through wood and oil before aging into smoke. |

## Validate

```bash
bbs test -t sim_core_test
bbs build -t pixelsim_app
build/default-windows-x86_64/bin/Debug/PixelSim.exe --smoke
```

`PixelSim.exe --smoke` creates the D3D11 pipeline, compiles all runtime shaders, submits two separate radius-five sand brushes, advances fixed physics updates, and verifies material conservation. Exact-cell GPU scenarios cover sand/water displacement, matched powder/liquid acceleration, liquid-column conservation, both powder diagonals, resting-liquid stability, rust and acid erosion, fire/oil ignition, gradual salt dissolution, acid/salt non-reaction, bedrock resistance, oil buoyancy, accelerated lava/water contact, vertical collision clipping, pressure containment, and bulk-water leveling. The basin fixture requires every interior column to be filled, limits surface variation to one cell, rejects internal holes, and hashes the material grid across 64 additional updates to catch visible resting-state jitter. A D3D11 timestamp query then measures 120 complete five-dispatch updates and fails if their average exceeds the 8.33 ms budget for 120 Hz.

## Layout

- `src/main.c` — Win32/D3D11 app, compute dispatches, direct draw, input.
- `src/sim_core.*` — material metadata, viewport mapping, input queue, and the CPU-to-GPU brush command contract.
- `assets/shaders/pixelsim.hlsl` — brush, simulation, and render shaders.
- `tests/SimCoreTest.c` — deterministic material, viewport, palette, queue, and brush-footprint tests.
