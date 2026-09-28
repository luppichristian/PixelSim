# PixelSim conservative multi-pass GPU simulation redesign

## Status

**Implemented and verified on 2026-09-28.** This is the canonical design record. It supersedes the earlier all-tiled proposal because an independent audit identified two correctness requirements that are better served by preserving axis ownership:

1. accelerated lava must not tunnel through water without reacting;
2. a liquid pressure source must not rely on support that moves concurrently in another row.

The design below keeps the proven local and column ownership models, replaces the horizontal solver with a deterministic parallel segmented solver, and uses additional in-place row-color passes to make support snapshots stable.

The implementation made two conservative deviations from the initial pass-2 sketch below. The vertical pre-copy remains because a direct-output experiment failed material conservation when dense falling piles reached the floor. Also, lava no longer treats water as a traversable resident: accelerated lava stops immediately before water, and the existing phased local reaction converts the adjacent pair to iron and steam. This prevents tunneling without adding arbitrary writes to the column pass. The final D3D11 timestamp smoke measurement averaged 0.139 ms per complete five-dispatch update on the implementation machine, below the 8.33 ms 120 Hz budget.

## Objective

Build a measurably faster and more physically consistent GPU simulation while preserving:

- the two GPU-resident `R32_UINT` state textures;
- the 20-byte brush-command-only CPU upload contract;
- deterministic fixed 120 Hz scheduling;
- conservative swaps and documented reactions;
- accelerated powder/liquid gravity and path clipping;
- material behavior, rendering, palette, and input semantics;
- the `bbs` build and existing smoke entry point.

No CPU world mirror, per-frame state upload, arbitrary scatter writes, atomics against global state, third world texture, or parallel CMake build is introduced.

## Audit findings

### What is already correct

- The local `Simulate` pass owns disjoint 2x2 blocks (`assets/shaders/pixelsim.hlsl:214-292`).
- X/Y ownership varies independently, preserving eventual diagonal reachability (`assets/shaders/pixelsim.hlsl:221-227`).
- The vertical pass has one owner per column and scans bottom-up, preventing double movement (`assets/shaders/pixelsim.hlsl:294-334`).
- The horizontal pass has one owner per row, so the current shader has no output data race (`assets/shaders/pixelsim.hlsl:436-449`).
- `Dispatch` correctly unbinds resources and swaps ping-pong state after each current physics pass (`src/main.c:204-236`).
- The renderer samples the latest state (`src/main.c:302-310`).
- Movement uses swaps, packed-state classification masks the low byte, and out-of-bounds reads act as iron.
- The fixed scheduler is exact and overflow-capped (`src/sim_core.c:144-167`).

### P0 correctness defect: reactive tunneling

Water/lava reactions occur only inside the local 2x2 pass (`assets/shaders/pixelsim.hlsl:126-143`, `:239`). The later vertical pass allows lava to enter water (`assets/shaders/pixelsim.hlsl:78-89`) and ray-marches up to eight cells, but it swaps only with the final destination (`assets/shaders/pixelsim.hlsl:311-332`). Fast lava can therefore cross a thin water layer and land in empty space beyond it without ever becoming adjacent during a reaction phase.

This violates the documented behavior that water meeting lava becomes steam while lava solidifies (`README.md:31`). The vertical owner must resolve reactive contact while traversing; a performance redesign that preserves the current ray unchanged would preserve this defect.

### P1 consistency defect: movable support

`IsPressureSource` derives support from `StateIn` (`assets/shaders/pixelsim.hlsl:91-117`, `:336-341`), while all row invocations mutate their respective output rows concurrently. An upper row can decide that lower liquid is stable support while the lower row simultaneously transfers that support sideways. This is not a write race, but it is a stale simultaneous-update decision that can create unsupported lateral movement or transient holes.

The horizontal replacement must guarantee that every row read as support remains immutable for that dispatch.

### P1 performance bottlenecks

1. The vertical pass copies every column before scanning it (`assets/shaders/pixelsim.hlsl:301-302`).
2. The horizontal pass launches only six 64-thread groups, then each invocation serially scans a complete row, twice for water/lava and sometimes in reverse (`assets/shaders/pixelsim.hlsl:349-449`).
3. Every small brush command dispatches over all 230,400 cells and copies the complete state (`assets/shaders/pixelsim.hlsl:204-212`, `src/main.c:256-267`).
4. There are no D3D11 timestamp/disjoint measurements around individual passes; presentation FPS cannot identify the limiting shader.

### Verification gaps

The current smoke validates adjacent reactions but not a moving high-velocity lava/water contact. Broad conservation counts only occupied cells, so an incorrect material conversion can remain hidden. There is no per-material histogram, full-grid material-ID validity check, movable-support pressure fixture, or packed-state deterministic replay.

## Selected production architecture

A fixed update uses five compute dispatches but only three grid-equivalent state writes:

1. **Local 2x2 pass** — `StateIn -> StateOut`, then ping-pong swap.
2. **Column gravity/contact pass** — `StateIn -> StateOut`, then ping-pong swap.
3. **Horizontal row color A** — in-place UAV update of one-third of rows; no ping-pong swap.
4. **Horizontal row color B** — in-place UAV update of another third; no ping-pong swap.
5. **Horizontal row color C** — in-place UAV update of the final third; no ping-pong swap.

Brush commands use a separate bounded in-place UAV path and do not change `read_state`.

The two state textures remain the world. Local and vertical passes retain ping-pong ownership. In-place brush and horizontal passes bind the current texture only as a UAV, never simultaneously as an SRV.

## Pass 1: preserve the local 2x2 solver

Keep the current `Simulate` ownership and phase schedule. It is already the strongest part of the shader:

- one invocation owns four output cells;
- reactions are local and deterministic from `FrameIndex`/position;
- blocked powders eventually see both diagonals;
- gas movement is swap-based;
- steam condensation resets the cell to an un-packed material value.

Only make correctness-focused changes:

1. Centralize packed-state masks and product initialization.
2. Add helpers that explicitly reset velocity/temporary bits for reaction products.
3. Keep reaction probability at exactly once per fixed update.
4. Add mirrored and all-phase reaction fixtures.

Do not merge gravity or whole-row liquid pressure into this pass.

## Pass 2: column-owned gravity with contact resolution

Preserve one invocation per column and bottom-up traversal. This retains exact accelerated movement, density displacement, and complete path inspection without introducing destination races.

### Remove the initial column copy

Emit final output during the bottom-up scan:

- non-falling cells write their input word directly to `StateOut[source]`;
- immediately blocked falling cells write themselves with vertical velocity reset;
- a moving particle reads the already-produced destination resident from `StateOut[destination]`, writes the mover at destination, and writes that resident at source.

This ordering is valid because every downward destination has already been processed. Add an identity-heavy exact-state test before relying on the optimization.

### Resolve reactions encountered on the path

The ray traversal must distinguish three outcomes for each inspected resident:

1. **Enterable, nonreactive:** continue toward the requested destination.
2. **Reactive contact:** stop at the last cell before the resident and resolve the documented pair.
3. **Blocking, nonreactive:** stop before the blocker.

For lava descending toward water:

- move the lava state to the last legal cell immediately above the first contacted water;
- transform that lava product to iron;
- transform the contacted water to steam;
- move the pre-contact destination resident back to the original source;
- leave every other traversed cell unchanged.

For water descending directly onto lava, resolve the same pair orientation: water becomes steam and lava becomes iron. Contact at distance one reacts in place. Contact after empty/gas traversal clears the original source conservatively through the same resident swap used by movement.

The column invocation exclusively owns both cells, so this reaction is race-free. Reset packed metadata on iron/steam products. Stop after the first reactive contact; do not chain multiple reactions in one update.

### Vertical invariants

- Every crossed cell is inspected.
- A particle never crosses a solid, unsupported density barrier, or reactive contact.
- The mover is written once.
- The source is cleared or receives exactly the displaced destination resident.
- Reaction products exactly match `ReactPair` semantics.
- Occupied count remains two for water/lava conversion.
- Nonreactive per-material counts remain unchanged.

## Passes 3–5: parallel segmented liquid pressure

### Why three row colors

Pressure classification reads the source row plus support at `y + 1` and `y + 2`. Rows updated concurrently must therefore be separated by at least three. Partition rows by:

```text
row_color = y % 3
```

Each horizontal dispatch updates exactly one color. Its active rows are at least three rows apart, so no active row lies within another active row's two-row support footprint. Support is immutable for the complete dispatch.

Process all three colors once per fixed update. Rotate their order with `FrameIndex`:

```text
first_color = FrameIndex % 3
order = first_color, (first_color + 1) % 3, (first_color + 2) % 3
```

This removes permanent top-to-bottom ordering preference while giving every row exactly one pressure opportunity per update.

### In-place resource discipline

For each color pass:

- bind the current state texture as `RWTexture2D<uint>` only;
- do not bind that texture as an SRV;
- dispatch one group per active row;
- load the active row and two support rows before writing;
- mutate only the active row;
- unbind the UAV at dispatch completion;
- issue the next color dispatch in command order;
- do not change `read_state`.

D3D11 command ordering plus UAV unbind/rebind makes earlier color writes visible to later colors. The color separation prevents concurrent read/write overlap within one dispatch.

### One cooperative group per active row

Use `[numthreads(64, 1, 1)]`. Sixty-four lanes cooperatively load:

- 640 mutable row cells;
- 642 first-support cells including X halos;
- 642 second-support cells including X halos.

The base storage is approximately 7.7 KiB. Additional shared arrays for segment IDs and reductions must keep the total below D3D11's 32 KiB group-shared limit.

Do not let lane 0 reproduce the complete serial scan. That would improve memory locality but waste 63 lanes during the main algorithm. Use a parallel segmented selection procedure.

### Parallel segmented selection

Process water and lava sequentially to preserve the current material-order semantics. Reuse scratch arrays between types.

For one liquid type:

1. **Classify cells in parallel.** Each lane classifies its ten cells as:
   - traversable: same liquid or open;
   - obstacle: every other resident;
   - pressure source;
   - pressure destination.
2. **Build segment IDs.** Run a group-shared prefix scan over obstacle boundaries so every traversable cell knows its obstacle-delimited segment.
3. **Initialize per-segment reductions.** Cooperatively reset source/destination extrema.
4. **Select forward candidates.** Use group-shared `InterlockedMin` to find the first pressure source in scan direction for each segment. After a barrier, find the first valid destination beyond that source.
5. **Select reverse fallback.** In parallel, use `InterlockedMax` to find the opposite-direction source and nearest destination before it.
6. **Commit one transfer per segment.** One lane representing each segment chooses the forward transfer when available, otherwise the reverse fallback, and swaps source/destination in shared row memory.
7. **Barrier before the next liquid type.** Lava sees water's completed row result, matching the current sequential ordering.
8. **Cooperative writeback.** All lanes write the active row in-place.

All atomics are group-shared reductions, not global state arbitration. Segments are disjoint and commit at most one transfer per material, preserving bounded linear work and exact conservation.

### Equilibrium and movement rules

Retain these existing requirements:

- `VelocityY == 0` before a liquid can source pressure;
- support is solid or demonstrably stable material;
- an elevated source has same-type liquid below;
- a destination is open and does not already have same-type liquid below;
- segments stop at solids, the other liquid type, and world boundaries;
- a single supported liquid cell does not move;
- a pool whose depths differ by at most one remains stable;
- water and lava are processed in deterministic opposite scan orientations.

Because support rows cannot move concurrently, a qualifying source cannot lose its support during that color pass.

## Bounded in-place brush pass

The current radius-five brush dispatches over the entire grid. Replace it with a clipped in-place UAV edit:

1. Compute the clipped brush rectangle from the existing five-field `sim_brush_command`.
2. Dispatch only that rectangle with 8x8 groups.
3. Derive absolute coordinates from the rectangle origin plus `SV_DispatchThreadID`.
4. Bind `state_uav[read_state]`; do not bind current state as an SRV.
5. Write only cells inside the circle and leave all other cells untouched.
6. Unbind resources and keep `read_state` unchanged.
7. Execute queued commands sequentially so overlapping brushes preserve FIFO overwrite order.

A radius-five command needs a 2x2 group dispatch (256 padded threads) instead of 230,400 threads. The CPU still uploads only the 20-byte command.

## Host integration

### Explicit pass descriptors

Replace shader-pointer dispatch branching (`src/main.c:215-225`) with explicit descriptors containing:

- compute shader;
- dispatch dimensions or dimension callback;
- SRV/UAV binding mode;
- whether the pass swaps `read_state`;
- optional debug/timestamp label.

Required binding modes:

- ping-pong state pass;
- current-state in-place UAV pass;
- bounded brush UAV pass.

Physics sequence:

```text
Simulate                    ping-pong, swap
MoveFallingAndResolve       ping-pong, swap
MoveLiquidsColor(frame+0)   in-place, no swap
MoveLiquidsColor(frame+1)   in-place, no swap
MoveLiquidsColor(frame+2)   in-place, no swap
```

Only bind `brush_srv` for the brush pass. Render after all UAVs are unbound.

### Constants

Add only data required by the current dispatch:

- brush rectangle origin/extent may use a dedicated small brush constant buffer or fields appended with matching static assertions;
- horizontal color/pass index should be encoded without remapping the frame buffer between every pass when practical—for example, three compiled entry points calling one shared implementation with pass offsets.

Keep CPU/HLSL ABI assertions synchronized.

### Pass-count semantics

Update or rename `SIM_PASSES_PER_UPDATE`. It should count actual physics dispatches (five), not only ping-pong swaps. Tests must separately assert:

- physics dispatch count;
- ping-pong swap count (two);
- horizontal row opportunities (one per row per update).

## Implementation phases

### Phase 0 — benchmark and add missing red tests

1. Add D3D11 timestamp/disjoint queries around each existing compute pass.
2. Capture median/p95 GPU time after warm-up for:
   - empty world;
   - sparse falling material;
   - dense blocked basin;
   - dense mixed materials;
   - repeated radius-five painting.
3. Add failing smoke fixtures for:
   - high-velocity lava above one-cell water with empty space beyond;
   - high-velocity lava above a multi-cell water layer;
   - water falling directly onto lava;
   - an upper pressure source whose supporting row would move in the same old pass;
   - per-material histograms and invalid material IDs.
4. Verify failures occur for the intended reason before implementation.

### Phase 1 — bounded brush

1. Add tested clipped rectangle calculation.
2. Add the in-place brush binding/dispatch path.
3. Verify corner clipping, radius-zero exact test brush, radius-five footprint, maximum radius, FIFO overlap, unchanged outside cells, and unchanged `read_state`.
4. Timestamp repeated painting against the baseline.

### Phase 2 — vertical direct emission and contact reactions

1. Remove the initial column copy using bottom-up direct output.
2. Add first-contact water/lava resolution.
3. Preserve acceleration, density displacement, source clearing, and path clipping.
4. Run exact packed-state comparisons for nonreactive scenes and documented product checks for reactive scenes.

### Phase 3 — one horizontal color pass with parallel segments

1. Implement cooperative row/support loading.
2. Implement parallel segment IDs and candidate reductions for water.
3. Add group-shared commit and cooperative writeback.
4. Extend to lava, preserving sequential water-then-lava semantics.
5. Compare one active color against a CPU/reference extraction of the old segment selection on isolated rows.

### Phase 4 — stable-support three-color schedule

1. Add three in-place color dispatches.
2. Rotate color order by `FrameIndex`.
3. Remove the old ping-pong horizontal shader.
4. Verify each row updates exactly once and no dispatch reads a row written concurrently.
5. Run movable-support, deep-airborne-column, basin, wall, and resting-checksum tests.

### Phase 5 — remove old paths and document

1. Delete obsolete horizontal helpers/entry point and temporary A/B switches.
2. Replace pointer-identity dispatch logic with descriptors.
3. Update shader creation/release, pass constants, static assertions, tests, and README together.
4. Reuse a staging texture in smoke measurements instead of allocating one for every read.
5. Keep `PixelSim.exe --smoke` as the required GPU verification command.

## Verification matrix

### CPU

Run `bbs test -t sim_core_test` and cover:

- fixed scheduler exactness and overflow cap;
- brush ABI and clipped bounds;
- pass/row-color scheduling;
- every row receives one opportunity per update;
- rotating color order has no missing/duplicate color;
- dispatch dimensions at 640x360 and non-multiple test dimensions.

### GPU correctness

Run `bbs build -t pixelsim_app` and `PixelSim.exe --smoke`. Require:

- all compute entry points compile as `cs_5_0`;
- D3D11 debug layer reports no SRV/UAV aliasing or uninitialized output;
- exact source clearing and destination placement;
- matched powder/liquid free fall;
- acceleration sequence and obstacle clipping;
- sand/rust/lava displacement rules;
- both powder diagonals across all local phases;
- deep airborne liquid remains centered;
- movable support cannot disappear concurrently;
- pressure cannot cross internal walls;
- single supported liquid remains fixed;
- basin columns fill with depth difference <= 1 and no holes;
- delayed masked-grid checksum remains stable;
- fast lava cannot tunnel through water;
- water/lava products are correct in both orientations and at multiple speeds;
- rust/iron and wood/lava reactions cover both orientations;
- nonreactive per-material histograms remain unchanged;
- every low-byte material ID is valid;
- two identical runs produce the same packed-state checksum where transient metadata is expected to settle.

### Performance

Use GPU timestamps, not presentation FPS or total smoke wall time. Report adapter, build configuration, warm-up, sample count, seed, and scene.

Acceptance gates:

- repeated radius-five brush GPU time improves by at least 10x;
- dense horizontal pressure time improves materially over the old pass despite the three dispatch boundaries;
- full dense-basin p95 update time is below the old pipeline and under the 8.33 ms 120 Hz budget;
- sparse falling scenes do not regress by more than 10%;
- no performance claim is made if timestamp-disjoint data is invalid.

If the parallel segmented row solver does not beat the old shader, retain the correctness fixes and profile the reduction stages before changing ownership. Do not fall back to lane-0 serial scanning and call it optimized.

### Visual

Inspect equal-simulation-time captures and interactive motion for:

- free-fall and collision;
- powder sinking;
- open-platform liquid runoff;
- early/middle/final basin settling;
- gas rise/drift;
- mirrored scenes.

Reject water/lava tunneling, unsupported sheets, internal holes, wall leakage, one-sided drift, oscillating equilibrium, teleporting pressure bands, and material behavior that varies permanently with row color.

## Risks

| Risk | Mitigation |
| --- | --- |
| Group-shared scratch exceeds 32 KiB | Calculate layout statically and compile-time-document each array; reuse water/lava reduction storage |
| Shared atomics serialize dense segments | One source/destination reduction per segment; benchmark and use tree reductions if contention dominates |
| Row-color order creates bias | Rotate all three orders with `FrameIndex`; mirror and checksum tests |
| In-place UAV resource hazard | UAV-only binding, explicit unbind between dispatches, D3D11 debug-layer verification |
| Vertical direct output reads an uninitialized destination | Bottom-up proof plus identity-heavy packed-state regression |
| Reactive contact changes existing velocity semantics | Stop at first contact, reset product metadata, exact source/contact/product assertions |
| Added dispatches cost more than saved scan work | Timestamp every pass; require full-update improvement before removing baseline path |
| Parallel selection diverges from current segment semantics | Isolated-row oracle tests for forward source/destination and reverse fallback |

## Definition of done

- Fast lava and water contact always resolves according to documented products; no reactive tunneling fixture fails.
- Horizontal support rows cannot move concurrently with a source that reads them.
- The horizontal solver uses cooperative parallel segment selection, not one serial lane per row.
- Brush dispatch is clipped and in-place while preserving the five-field command ABI.
- The world remains exactly two GPU-resident `R32_UINT` textures.
- Every ordinary movement is conservative; all reactions have explicit count/type expectations.
- Existing and new CPU/GPU tests pass, including per-material validity/conservation and delayed equilibrium.
- D3D11 timestamps prove the new full update is faster on the same adapter/configuration and below the 120 Hz budget.
- README, pass constants, shader creation/release, and smoke diagnostics match the final pipeline.
