# PixelSim tiled GPU simulation redesign

> **Superseded:** This exploratory all-tiled design is retained for reference,
> but it is not the recommended production path. The canonical plan is
> `.hermes/plans/2026-09-28-conservative-multipass-gpu-simulation.md`, which
> incorporates the later audit findings for reactive tunneling and movable
> liquid support.

## Objective

Replace the current mixed 2x2/serial-axis simulation with a conservative, deterministic, GPU-parallel algorithm that:

- keeps the world in the existing two ping-ponged `R32_UINT` textures;
- preserves the current material set, reactions, fixed-step behavior, brush ABI, and direct rendering path;
- prevents races, duplication, deletion, tunneling, wall leakage, parity locks, and resting-liquid jitter;
- removes full-row and full-column serial mutation from the hot path;
- has measured GPU-time and behavior acceptance gates rather than relying on presentation FPS alone.

This document chooses one implementation path: **two offset, tile-owned compute passes per fixed update, with multiple conservative 2x2 microsteps executed in group-shared memory**. It deliberately does not introduce atomics, a CPU world mirror, an intent texture, or a parallel build system.

## Executive summary

The current implementation is unusually strong on safety for a small sandbox. Every pass has an explicit owner, ping-pong state is swapped after every dispatch, vertical movement inspects its complete path, and the smoke suite checks conservation, reactions, diagonal reachability, collision clipping, liquid equilibrium, and delayed resting stability.

The performance problem is architectural rather than a missing micro-optimization. `MoveFallingMaterialsVertical` assigns an entire 360-cell column to one invocation, while `MoveLiquidsHorizontal` assigns an entire 640-cell row to one invocation. The latter also scans each obstacle-delimited row for both water and lava and may reverse-scan a segment. At 640x360 this creates only 640 useful vertical invocations and 360 useful horizontal invocations, with long, divergent serial loops. The algorithm is bounded and correct, but it underuses the GPU and scales poorly with larger worlds or more liquid types.

The replacement uses 16x16 thread groups. Every thread loads one cell into a 1 KiB `groupshared uint` tile. A single thread owns each active 2x2 block during a microstep, applies only local swaps or documented reactions, and synchronizes before the next microstep. The group writes the tile once after several on-chip movement phases. A second dispatch shifts tile origins by 8 cells on both axes, so cells separated by the first pass's tile seams interact without overlapping writers. This keeps exact conservation by construction while replacing serial row/column traversal with roughly 920–943 well-occupied groups per pass.

The new hot path performs two global state reads/writes per update rather than three global passes plus repeated row/column scans and ray reads. At 640x360 and 120 updates/s, two ideal full-grid read/write passes represent about 0.442 GB/s of logical state traffic; three already represent 0.664 GB/s before counting the current vertical ray checks and repeated horizontal scans. These are workload estimates, not measured hardware bandwidth. D3D11 timestamp queries must establish the actual baseline and prove the gain.

## Grounded review

### Project map

| Surface | Size | Responsibility |
| --- | ---: | --- |
| `src/main.c` | 1,383 lines | Win32/D3D11 host, resource binding, scheduling, rendering, and GPU smoke scenarios |
| `assets/shaders/pixelsim.hlsl` | 507 lines | Brush, local material rules, serial vertical/horizontal simulation, and rendering |
| `src/sim_core.c` / `.h` | 283 lines | Material metadata, brush ABI, viewport/input helpers, exact fixed-step scheduler |
| `tests/SimCoreTest.c` | 153 lines | CPU helper and scheduler tests |
| `project.bbs` | 26 lines | `pixelsim_app` and `sim_core_test` targets |

The world is 640x360 (`src/sim_core.h:8-9`), runs at 120 fixed updates/s with four-step catch-up (`src/sim_core.h:17-19`), and stores material IDs in the low byte (`src/sim_core.h:20`, `assets/shaders/pixelsim.hlsl:10`). The CPU/HLSL brush payload is correctly pinned to five 32-bit fields (`src/sim_core.h:50-59`, `assets/shaders/pixelsim.hlsl:15-22`).

### Current update pipeline

`RunSimulationUpdate` dispatches three shaders in order (`src/main.c:238-253`):

1. `Simulate`: one invocation owns an offset 2x2 block and handles reactions, powder diagonal rolls, gas motion, and steam condensation (`assets/shaders/pixelsim.hlsl:214-292`).
2. `MoveFallingMaterialsVertical`: one invocation copies and mutates one complete column bottom-up, ray-checking up to eight cells for powders and liquids (`assets/shaders/pixelsim.hlsl:294-334`).
3. `MoveLiquidsHorizontal`: one invocation copies one complete row and performs obstacle-segment pressure scans for water and lava (`assets/shaders/pixelsim.hlsl:336-449`).

`Dispatch` binds current SRV/next UAV, unbinds both, and swaps `read_state` after every compute dispatch (`src/main.c:204-236`). Rendering samples the latest state (`src/main.c:299-315`). Fixed-step accumulation is exact and overflow-capped (`src/sim_core.c:144-167`, `src/main.c:284-298`).

### Strengths to preserve

1. **Single-writer safety.** The local pass owns disjoint 2x2 blocks; axis passes own complete rows or columns. No unordered scatter race is present.
2. **Conservative movement.** Movement is implemented as swaps, including density displacement. Vertical travel checks every crossed cell (`assets/shaders/pixelsim.hlsl:311-333`).
3. **Correct cadence.** The scaled-tick accumulator avoids integer-rate truncation and caps catch-up work (`src/sim_core.c:144-167`).
4. **Reachability awareness.** The four local partitions vary X and Y independently (`assets/shaders/pixelsim.hlsl:221-227`), and smoke tests exercise both powder diagonals (`src/main.c:1013-1053`).
5. **Meaningful GPU verification.** The runtime smoke validates exact movement, conservation, deep airborne liquid, reactions, erosion, wall clipping, basin geometry, and 64-update resting stability (`src/main.c:754-1344`).
6. **Stable two-texture contract.** State remains GPU-resident; brush commands are the only world mutation uploaded from the CPU.

### Weaknesses and risks

#### P0 — serial row/column kernels underuse the GPU

- Vertical work is only 640 useful invocations, each copying 360 cells and then scanning the column (`assets/shaders/pixelsim.hlsl:294-333`; dispatch shape in `src/main.c:220-222`).
- Horizontal work is only 360 useful invocations, each copying 640 cells and scanning row segments for two liquid types (`assets/shaders/pixelsim.hlsl:407-449`; dispatch shape in `src/main.c:223-225`).
- The horizontal fallback can traverse a segment again (`assets/shaders/pixelsim.hlsl:374-395`). This remains O(width), but it is serial and divergent inside each invocation.
- The present grid has 230,400 cells. Before vertical ray checks, the current local pass, two full copies, and typical two-material row scans account for about 1.154 million cell visits per update, or 138.5 million at 120 updates/s. This is a source-derived upper/lower workload estimate, not a GPU profiler result.

#### P0 — pressure transfer is conservative but visibly nonlocal

`TransferPressureInSegment` swaps one elevated source directly with a potentially distant destination (`assets/shaders/pixelsim.hlsl:349-405`). It produces a level pool and passes the stability test, but the transient can behave like instantaneous teleportation rather than finite-speed flow. Archived captures show that the final basin is level, while earlier captures retain a broad central mound. A replacement should preserve equilibrium without retaining whole-row teleportation.

#### P1 — performance is not measured at the simulation boundary

The window title reports presentation FPS (`src/main.c:327-337`), but there are no D3D11 timestamp/disjoint queries around simulation dispatches. The smoke executable took about 2.048 seconds in the reviewed environment, but that run includes many staging allocations, copies, maps, and hundreds of correctness updates, so it is not a simulation benchmark.

#### P1 — verification and production hosting are coupled

Roughly half of `src/main.c` is smoke-only staging/readback/scenario code (`src/main.c:402-668`, `src/main.c:754-1344`). This does not make the simulation incorrect, but it makes architectural changes harder to review and encourages repetitive staging texture creation. Extracting a small D3D test harness is useful after the algorithm is stable; it is not a prerequisite for the first shader spike.

#### P2 — dispatch policy is encoded by shader pointer identity

Group dimensions are selected by comparing shader pointers (`src/main.c:215-225`). That is workable for three passes but becomes brittle when adding tile offsets or benchmark variants. The replacement should use an explicit pass descriptor containing shader and dispatch dimensions.

#### P2 — every compute dispatch binds the brush SRV

`Dispatch` binds `brush_srv` even for physics shaders (`src/main.c:206-211`). This is minor compared with the serial kernels, but a pass descriptor should bind only resources used by that pass.

## Chosen architecture: offset tile-owned local transactions

### 1. Tile geometry and ownership

Use `[numthreads(16, 16, 1)]` and one `groupshared uint TileState[16][16]` array. A 16x16 tile holds 256 cells / 1,024 bytes, leaving ample Shader Model 5 group-shared capacity for small flags or counters.

Each fixed update dispatches two tile kernels:

- **Tile A:** origin `group_id.xy * 16`, covering approximately 40x23 = 920 groups.
- **Tile B:** origin `group_id.xy * 16 - 8`, covering approximately 41x23 = 943 groups and clipping out-of-bounds lanes to an immutable iron boundary.

Within one dispatch, tile interiors do not overlap, so every valid output cell has exactly one writer. The half-tile offset changes the seam set; an adjacent or diagonal pair split by Tile A is interior to Tile B. World edges remain solid and are never written.

Each pass follows this exact structure:

1. Each lane loads one valid input cell, or the synthetic boundary value for an invalid coordinate.
2. Group barrier.
3. A fixed sequence of conservative local microsteps mutates only group-shared state.
4. Group barrier after every microstep.
5. Each valid lane writes its own final shared cell to `StateOut` exactly once.
6. Host unbinds resources and swaps ping-pong indices.

There is no global scatter, atomics, or in-place SRV/UAV aliasing.

### 2. Local transaction primitive

A microstep partitions the tile into disjoint 2x2 blocks. One lane owns all four shared words in its block, executes a pure transition function, and writes all four words back before the barrier. The phase order varies X and Y independently (`00, 01, 10, 11`) so a stationary cell eventually occupies every block slot and both powder diagonals remain reachable.

The transition function must preserve four explicit invariants:

- every input word produces exactly one output word, except documented reactions that transform types;
- movement is a swap, never a destination-only write;
- material classification always masks `SIM_PIXEL_TYPE_MASK` / `TYPE_MASK`;
- transient bits are cleared before the second tile pass writes the renderable final state.

### 3. Per-update microstep order

The exact order is a decision, not an implementation option menu:

#### Tile A — integrate and begin transport

1. Initialize a transient per-particle fall budget from persistent vertical velocity; increment velocity once per fixed update up to the existing cap.
2. Run one reaction phase over disjoint horizontal/vertical block edges. Reaction probability is sampled once per fixed update so two tile passes do not double ignition, erosion, condensation, or water/lava cadence.
3. Run two gravity microsteps with Y ownership toggled each time. Powders and liquids spend one fall-budget unit per successful downward swap.
4. When direct fall is blocked, allow one deterministic powder diagonal candidate. Candidate choice uses the hash bit to select an actual direction, not merely evaluation order.
5. Run one gas-rise/drift microstep.
6. Mark particles that moved vertically during this fixed update so airborne liquid cannot flow laterally.

#### Tile B — cross seams and settle

1. Continue up to two gravity microsteps using the remaining transient fall budget.
2. Retry blocked powder diagonal movement under the shifted seam and opposite X priority.
3. Run four alternating horizontal liquid-relaxation microsteps in group-shared memory.
4. Permit a horizontal liquid swap only when the source did not move vertically this update, has stable support, and is elevated over same-type liquid; reject transfers into a column that already has same-type liquid directly below the destination. This retains the current discrete equilibrium rule and prevents a settled one-layer pool from translating.
5. Run one shifted gas-rise/drift microstep.
6. Reset persistent vertical velocity only for particles still blocked after both tile passes; clear all transient budget/moved bits before global writeback.

Four gravity microsteps per update cap direct fall at four cells/update in the first implementation. Do not recreate the current eight-cell speed until timestamp and visual tests show it is needed; terminal speed is a behavior parameter, while path safety and cadence are correctness requirements. If a higher terminal speed is required, add on-chip gravity microsteps before adding another global pass.

### 4. Packed cell ABI

Keep material type in bits 0–7 and existing persistent vertical velocity in bits 16–23. Reserve explicit currently-unused bits for:

- fall budget used only between Tile A and Tile B;
- moved-vertically-this-update;
- optional one-bit lateral tie-break/momentum if visual testing proves stateless hash direction insufficient.

Define all masks and shifts together at the top of `pixelsim.hlsl` and mirror only stable public masks needed by C in `sim_core.h`. Brush and reaction products initialize all metadata deliberately. `DrawPixels`, readback helpers, and every classifier continue masking the low-byte type.

### 5. Why this architecture is preferred

- **Correct by ownership:** one tile owns every write; one block lane owns every local transaction.
- **Conservative by construction:** movement is a local permutation/swap.
- **GPU-shaped work:** hundreds of 256-lane groups replace a few hundred long serial invocations.
- **Bounded cost:** work per cell is fixed and independent of row width, pool width, or number of sources in a segment.
- **Low global traffic:** repeated movement happens in group-shared memory; only two global read/write pairs are needed per update.
- **No extra world resource:** the existing two `R32_UINT` textures remain the complete world state.
- **Finite-speed flow:** liquid pressure propagates through repeated local relaxation instead of teleporting across an entire connected row.
- **Scalable material rules:** adding a liquid type changes local classification, not an additional complete row scan.

### 6. Explicit non-goals

Do not include these in the first cut:

- sparse active-tile lists or indirect dispatch;
- atomics or destination claim buffers;
- a third state/velocity texture;
- a CPU simulation mirror;
- per-frame world uploads;
- a CMake build file;
- simultaneous visual redesign or new materials.

Sparse dispatch is only justified after timestamp data shows empty-world cost matters more than tile-kernel cost. It requires GPU activity propagation, indirect arguments, and extra buffers, which materially expands the correctness surface.

## Implementation plan

### Phase 0 — establish measurement and freeze behavior

**Files:** `src/main.c`, `src/sim_core.h`, `tests/SimCoreTest.c`

1. Add a smoke/benchmark mode that uses D3D11 timestamp and timestamp-disjoint queries around only the three simulation passes.
2. Record median and p95 GPU time for at least these deterministic scenes after warm-up: empty world, sparse falling particles, dense mixed materials, and the existing 256-cell closed basin.
3. Keep presentation FPS separate from simulation GPU time.
4. Give each existing smoke failure a named diagnostic printed to stderr before exit. Preserve current exit codes if external scripts rely on them.
5. Add a small pass descriptor in `main.c`: shader, group counts, and resource mask. Remove pointer-identity dispatch sizing and avoid binding `brush_srv` to physics passes.

**Gate:** baseline timings and checksums are captured from the current implementation; existing `bbs test -t sim_core_test` and `PixelSim.exe --smoke` still pass.

### Phase 1 — add a tile-kernel spike behind an internal switch

**Files:** `assets/shaders/pixelsim.hlsl`, `src/main.c`

1. Add shared helpers for packed state, masked classification, swaps, stable support, reactions, and 2x2 transition ownership.
2. Implement `SimulateTilesA` and `SimulateTilesB` as separate shader entry points calling a common tile routine with compile-time/hardcoded pass policy. Do not map the constant buffer between the two dispatches.
3. Compile both shaders at startup and add them to explicit pass descriptors.
4. Keep the old three shaders selectable only as a temporary A/B oracle during this phase.
5. Dispatch Tile A, swap, dispatch Tile B, swap. Verify rendering samples the second result.

**Gate:** both tile entry points compile as `cs_5_0`; an identity-only version reproduces an unchanged state bit-for-bit, including dimensions not divisible by 16.

### Phase 2 — move gravity and powder transport into tiles

**Files:** `assets/shaders/pixelsim.hlsl`, `src/main.c`

1. Implement the transient fall budget and moved-vertical marker.
2. Add conservative vertical swaps for powder and liquid.
3. Add blocked-only powder diagonals with real hash-based direction selection.
4. Preserve density displacement: sand/rust through water where allowed, rust/lava and lava/water according to the current rule table.
5. Remove `MoveFallingMaterialsVertical` from the production pass list only after its exact tests pass under the tile path.

**Gate:** conservation, matched powder/liquid fall cadence, source clearing, path clipping, sinking/displacement, both diagonals, mirrored parity, seam crossings at x/y 7–8 and 15–16, and all world boundaries pass.

### Phase 3 — move gas and reactions into tiles

**Files:** `assets/shaders/pixelsim.hlsl`

1. Port gas rise and horizontal drift to disjoint local transactions.
2. Port water/lava, wood/lava, rust/iron, and steam condensation rules.
3. Ensure reaction cadence is once per fixed update, not once per tile pass or microstep.
4. Reset metadata for reaction products.
5. Cover both pair orientations and tile-seam orientations.

**Gate:** documented products, occupied/material counts, mirrored neighborhoods, and deterministic seeded outcomes match the intended rules. No reaction is skipped permanently at either tile offset.

### Phase 4 — replace whole-row liquid pressure

**Files:** `assets/shaders/pixelsim.hlsl`, `src/main.c`

1. Implement four alternating horizontal relaxation microsteps in Tile B.
2. Gate flow on the authoritative moved-vertical marker and stable support.
3. Require explicit column disequilibrium; do not seed movement for a solitary supported liquid or an equilibrium layer.
4. Stop every local transfer at solids and tile/world boundaries; Tile A/B offset alternation supplies eventual seam crossing.
5. Remove `MoveLiquidsHorizontal` from the production pass list only after basin acceptance passes.

**Gate:** deep airborne columns stay centered, pressure does not cross walls, an isolated supported cell remains fixed, a closed basin reaches full-column coverage with depth variation <= 1 and no holes, and the masked material checksum remains unchanged for at least 64 more updates.

### Phase 5 — remove the old hot path and consolidate tests

**Files:** `src/main.c`, `assets/shaders/pixelsim.hlsl`, optionally new `src/sim_gpu_test.c` / `.h`, `project.bbs`

1. Delete the obsolete serial vertical/horizontal shaders and old local `Simulate` entry point.
2. Remove old shader fields, compilation, pointer comparisons, dispatch branches, and teardown calls.
3. Keep Win32/D3D11 APIs in `src/main.c` or a D3D-specific GPU test module; do not leak platform APIs into `sim_core`.
4. Reuse one staging texture for smoke readback instead of allocating one per pixel/measurement call.
5. If smoke extraction is performed, preserve `PixelSim.exe --smoke` as the repository-mandated command; extraction must not create an unasked standalone artifact.
6. Update `SIM_PASSES_PER_UPDATE`, its CPU test, README design text, and pass-count static assertions together.

**Gate:** searches find no obsolete shader names or release paths; clean build and all verification commands pass.

### Phase 6 — optimize only from profiler evidence

1. Compare new median/p95 GPU time against Phase 0 on identical states and update counts.
2. Inspect occupancy/barrier cost with 16x16 tiles. Try 8x8 only if timestamps improve; retain the simpler/faster measured choice.
3. Tune the number and order of on-chip microsteps, keeping reaction cadence fixed and rerunning symmetry/equilibrium checks after every change.
4. Consider sparse active tiles only as a separately reviewed phase if empty-world GPU time remains material.

**Performance acceptance:**

- dense basin and dense mixed-material p95 simulation GPU time is at least 25% below the old pipeline on the same adapter and debug/release configuration;
- p95 cost of one fixed update remains below the 8.33 ms 120 Hz budget;
- no measured scene regresses by more than 10% without a documented behavior gain;
- performance results report adapter, build configuration, scene seed, warm-up count, sample count, and GPU timestamp values.

## Validation matrix

### CPU tests (`bbs test -t sim_core_test`)

- fixed scheduler exactness, catch-up cap, and overflow behavior;
- brush ABI size/offset and material mask constants;
- pass count and any host-side tile dispatch dimension helper;
- tile-origin coverage for 640x360: every cell exactly once in each pass, including shifted partial tiles.

### Runtime GPU smoke (`PixelSim.exe --smoke`)

Retain all existing scenarios and add:

1. exact source and destination plus material counts for every movement;
2. one-cell and multi-cell fall path clipping;
3. all Tile A and Tile B seams, four corners, and every world edge;
4. opposite coordinate parities and X-mirrored powder/liquid fixtures;
5. deep airborne liquid of at least four cells with no lateral spread;
6. powder displacement through every allowed liquid;
7. water/lava, wood/lava, rust/iron, and condensation on both pair orientations;
8. long-run per-material conservation where no reaction is enabled;
9. deterministic checksum replay for the same initial state and seed;
10. basin width/depth/no-hole acceptance plus delayed masked-grid checksum;
11. mixed dense scene containing every material for thousands of updates.

### Visual acceptance

Capture equal-simulation-time frames for:

- free-falling powder and liquid;
- powder sinking through water;
- open-platform liquid spread and runoff;
- closed-basin settling at early, middle, and final times;
- gas rise and drift;
- a mirrored pair of scenes.

Reject persistent tile seams, checkerboard gaps, one-sided drift, mid-air liquid sheets, wall leakage, teleporting bands, tall long-lived mounds, and visible settled-state jitter. Final screenshots alone are insufficient; inspect transient motion interactively.

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Tile seams delay movement | Half-tile second pass plus exact seam fixtures at both seam sets |
| Group-shared phase order introduces directional bias | Independent X/Y phase cycle, mirrored fixtures, alternate phase order by frame hash |
| Two tile passes accidentally double reactions | Run stochastic reactions only in Tile A and test exact seeded cadence |
| Liquid relaxation becomes powder-like and settles too slowly | Four on-chip horizontal phases, measured intermediate basin geometry, tune on-chip count before adding global passes |
| Deep airborne liquid spreads | Persist moved-vertical marker across A→B and cover a four-cell airborne column |
| Metadata leaks into classification/rendering | Central masks, brush/reaction initialization, masked readback and renderer tests |
| Barrier-heavy 16x16 groups underperform | Timestamp 16x16, then 8x8 as the only planned geometry comparison |
| Existing smoke becomes difficult to maintain | Named scenarios and one reusable staging texture after algorithm correctness is established |
| Behavior improves but optimization claim is unproven | GPU timestamp acceptance gate on identical deterministic scenes |

## Definition of done

- The production update path consists of two offset tile compute passes using only the existing two state textures.
- No production shader assigns an entire row or column to one invocation.
- Every output cell has one writer in every pass; every movement is a swap or documented reaction.
- All current CPU and GPU smoke cases pass, including conservation and delayed basin stability.
- New seam, mirror, parity, boundary, deterministic replay, and dense mixed-material cases pass.
- D3D11 timestamps show the performance acceptance targets on the same adapter/configuration.
- Interactive equal-time scenes show finite-speed liquid flow without seams, bias, leakage, airborne spread, or resting jitter.
- README and constants describe the final pass structure accurately.
