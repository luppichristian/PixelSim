#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sim.h"

#define SIM_BRUSH_QUEUE_CAPACITY 256
#define SIM_MAX_BRUSH_RADIUS SIM_WIDTH

typedef struct sim_brush_command {
  uint32_t type;
  int32_t x;
  int32_t y;
  int32_t radius;
  uint32_t seed;
} sim_brush_command;

/* This is uploaded directly to HLSL BrushCommand. */
_Static_assert(sizeof(sim_brush_command) == 20,
               "sim_brush_command must match HLSL BrushCommand");
_Static_assert(offsetof(sim_brush_command, seed) == 16,
               "sim_brush_command field layout must remain stable");

typedef struct sim_brush_bounds {
  int x;
  int y;
  int width;
  int height;
} sim_brush_bounds;

typedef struct sim_brush_queue {
  sim_brush_command commands[SIM_BRUSH_QUEUE_CAPACITY];
  uint32_t read_index;
  uint32_t count;
} sim_brush_queue;

sim_brush_command sim_make_brush_command(
    sim_pixel_type type,
    int x,
    int y,
    int radius,
    uint32_t seed);

bool sim_brush_queue_push(sim_brush_queue* queue, sim_brush_command command);
bool sim_brush_queue_pop(sim_brush_queue* queue, sim_brush_command* command);
uint32_t sim_brush_cell_count(int radius);
sim_brush_bounds sim_clip_brush_bounds(sim_brush_command command);

