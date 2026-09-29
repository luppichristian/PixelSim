# PixelSim agent notes

- Build with `bbs`; do not add a parallel CMake file.
- The application is Windows/D3D11-only. Keep platform APIs isolated in `src/main.c`.
- The simulation state belongs on the GPU: two `R32_UINT` textures are ping-ponged by compute shaders. Do not introduce a CPU mirror or a per-frame texture upload.
- A paint operation may only upload brush commands. Keep `sim_brush_command` layout in sync with HLSL `BrushCommand` (five 32-bit fields).
- Validate CPU helper changes with `bbs test -t sim_core_test`; validate graphics/shader changes with `PixelSim.exe --smoke` from the repository root.
- The D3D pixel shader draws the clickable one-row square palette at the top left; selection is reflected by its white border, the window title, and keyboard shortcuts (`0`–`9`, `A`–`D`). Empty is the erase brush.
- Preserve the existing Chromium-derived two-space C style.
