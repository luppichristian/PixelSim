#ifndef PIXELSIM_SIM_CORE_H
#define PIXELSIM_SIM_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIM_WIDTH 640
#define SIM_HEIGHT 360
#define SIM_PALETTE_X 8
#define SIM_PALETTE_Y 6
#define SIM_PALETTE_SWATCH_WIDTH 24
#define SIM_PALETTE_SWATCH_HEIGHT 24
#define SIM_PALETTE_SWATCH_STRIDE 28
#define SIM_PALETTE_ROW_STRIDE 28
#define SIM_PALETTE_COLUMNS 14
#define SIM_BRUSH_QUEUE_CAPACITY 256
#define SIM_MAX_BRUSH_RADIUS SIM_WIDTH
#define SIM_UPDATES_PER_SECOND 120u
#define SIM_MAX_CATCH_UP_STEPS 4u
#define SIM_PASSES_PER_UPDATE 5u
#define SIM_PIXEL_TYPE_MASK 0xffu
#define SIM_DISSOLVED_SALT_MASK 0x01000000u

typedef enum sim_phase {
  SIM_PHASE_EMPTY,
  SIM_PHASE_SOLID,
  SIM_PHASE_POWDER,
  SIM_PHASE_LIQUID,
  SIM_PHASE_GAS
} sim_phase;

typedef enum sim_pixel_type {
  SIM_PIXEL_EMPTY,
  SIM_PIXEL_WOOD,
  SIM_PIXEL_IRON,
  SIM_PIXEL_SAND,
  SIM_PIXEL_RUST,
  SIM_PIXEL_WATER,
  SIM_PIXEL_LAVA,
  SIM_PIXEL_SMOKE,
  SIM_PIXEL_STEAM,
  SIM_PIXEL_ACID,
  SIM_PIXEL_FIRE,
  SIM_PIXEL_OIL,
  SIM_PIXEL_SALT,
  SIM_PIXEL_BEDROCK,
  SIM_PIXEL_TYPE_COUNT
} sim_pixel_type;

typedef struct sim_material {
  sim_phase phase;
  float density;
  float temperature;
  float buoyancy;
} sim_material;

typedef struct sim_brush_command {
  uint32_t type;
  int32_t x;
  int32_t y;
  int32_t radius;
  uint32_t seed;
} sim_brush_command;

_Static_assert(sizeof(sim_brush_command) == 20, "sim_brush_command must match HLSL BrushCommand");
_Static_assert(offsetof(sim_brush_command, seed) == 16, "sim_brush_command field layout must remain stable");

typedef struct sim_brush_bounds {
  int x;
  int y;
  int width;
  int height;
} sim_brush_bounds;

typedef struct sim_viewport {
  int x;
  int y;
  int width;
  int height;
} sim_viewport;

typedef struct sim_brush_queue {
  sim_brush_command commands[SIM_BRUSH_QUEUE_CAPACITY];
  uint32_t read_index;
  uint32_t count;
} sim_brush_queue;

/* Returns stable physical properties for one pixel material. */
sim_material sim_get_material(sim_pixel_type type);

/* Returns the human-readable label used by the selector UI. */
const char* sim_material_name(sim_pixel_type type);

/* Creates a bounded GPU brush payload. Empty produces an erase command. */
sim_brush_command sim_make_brush_command(
    sim_pixel_type type,
    int x,
    int y,
    int radius,
    uint32_t seed);

/* Fits the simulation aspect ratio inside the client area. */
sim_viewport sim_make_viewport(int client_width, int client_height);

/* Maps a client point to one simulation cell; returns false outside the viewport. */
bool sim_viewport_to_cell(
    sim_viewport viewport,
    int client_x,
    int client_y,
    int* cell_x,
    int* cell_y);

/* Returns the palette material under a client point, or TYPE_COUNT on a miss. */
sim_pixel_type sim_palette_material_at(sim_viewport viewport, int client_x, int client_y);

/* Enqueues and consumes brush events exactly once in FIFO order. */
bool sim_brush_queue_push(sim_brush_queue* queue, sim_brush_command command);
bool sim_brush_queue_pop(sim_brush_queue* queue, sim_brush_command* command);

/* Returns the number of simulation cells covered by a circular brush. */
uint32_t sim_brush_cell_count(int radius);

/* Returns the simulation-clipped rectangle touched by a brush command. */
sim_brush_bounds sim_clip_brush_bounds(sim_brush_command command);

/* Consumes elapsed timer ticks as fixed-rate updates with bounded catch-up. */
uint32_t sim_consume_fixed_steps(
    uint64_t* accumulator,
    uint64_t elapsed_ticks,
    uint64_t ticks_per_second);

#endif
