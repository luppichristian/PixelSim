#include "sim_core.h"

static const sim_material k_materials[SIM_PIXEL_TYPE_COUNT] = {
    {SIM_PHASE_EMPTY, 0.0f, 20.0f, 0.0f},
    {SIM_PHASE_SOLID, 0.70f, 20.0f, 0.0f},
    {SIM_PHASE_SOLID, 7.80f, 20.0f, 0.0f},
    {SIM_PHASE_POWDER, 1.60f, 20.0f, 0.0f},
    {SIM_PHASE_POWDER, 5.20f, 20.0f, 0.0f},
    {SIM_PHASE_LIQUID, 1.00f, 20.0f, 0.0f},
    {SIM_PHASE_LIQUID, 3.10f, 1200.0f, 0.0f},
    {SIM_PHASE_GAS, 0.02f, 180.0f, 0.85f},
    {SIM_PHASE_GAS, 0.01f, 110.0f, 1.00f},
};

static const char* k_material_names[SIM_PIXEL_TYPE_COUNT] = {
    "Empty",
    "Wood",
    "Iron",
    "Sand",
    "Rust",
    "Water",
    "Lava",
    "Smoke",
    "Steam",
};

sim_material sim_get_material(sim_pixel_type type) {
  if (type < SIM_PIXEL_EMPTY || type >= SIM_PIXEL_TYPE_COUNT)
    return k_materials[SIM_PIXEL_EMPTY];
  return k_materials[type];
}

const char* sim_material_name(sim_pixel_type type) {
  if (type < SIM_PIXEL_EMPTY || type >= SIM_PIXEL_TYPE_COUNT)
    return k_material_names[SIM_PIXEL_EMPTY];
  return k_material_names[type];
}

sim_brush_command sim_make_brush_command(
    sim_pixel_type type,
    int x,
    int y,
    int radius,
    uint32_t seed) {
  sim_brush_command command;
  command.type = type <= SIM_PIXEL_EMPTY || type >= SIM_PIXEL_TYPE_COUNT
                     ? SIM_PIXEL_SAND
                     : (uint32_t)type;
  command.x = x < 0 ? 0 : (x >= SIM_WIDTH ? SIM_WIDTH - 1 : x);
  command.y = y < 0 ? 0 : (y >= SIM_HEIGHT ? SIM_HEIGHT - 1 : y);
  command.radius = radius < 1 ? 1 : (radius > SIM_MAX_BRUSH_RADIUS ? SIM_MAX_BRUSH_RADIUS : radius);
  command.seed = seed;
  return command;
}

sim_viewport sim_make_viewport(int client_width, int client_height) {
  sim_viewport viewport = {0};
  if (client_width <= 0 || client_height <= 0)
    return viewport;

  if ((int64_t)client_width * SIM_HEIGHT <=
      (int64_t)client_height * SIM_WIDTH) {
    viewport.width = client_width;
    viewport.height = client_width * SIM_HEIGHT / SIM_WIDTH;
  } else {
    viewport.width = client_height * SIM_WIDTH / SIM_HEIGHT;
    viewport.height = client_height;
  }
  viewport.x = (client_width - viewport.width) / 2;
  viewport.y = (client_height - viewport.height) / 2;
  return viewport;
}

bool sim_viewport_to_cell(
    sim_viewport viewport,
    int client_x,
    int client_y,
    int* cell_x,
    int* cell_y) {
  if (!cell_x || !cell_y || viewport.width <= 0 || viewport.height <= 0 ||
      client_x < viewport.x || client_y < viewport.y ||
      client_x >= viewport.x + viewport.width ||
      client_y >= viewport.y + viewport.height)
    return false;

  *cell_x = (int)(((int64_t)client_x - viewport.x) * SIM_WIDTH /
                  viewport.width);
  *cell_y = (int)(((int64_t)client_y - viewport.y) * SIM_HEIGHT /
                  viewport.height);
  return true;
}

sim_pixel_type sim_palette_material_at(sim_viewport viewport, int client_x, int client_y) {
  int cell_x;
  int cell_y;
  if (!sim_viewport_to_cell(viewport, client_x, client_y, &cell_x, &cell_y) ||
      cell_y < SIM_PALETTE_Y ||
      cell_y >= SIM_PALETTE_Y + SIM_PALETTE_SWATCH_HEIGHT ||
      cell_x < SIM_PALETTE_X)
    return SIM_PIXEL_EMPTY;

  int offset = cell_x - SIM_PALETTE_X;
  int material = offset / SIM_PALETTE_SWATCH_STRIDE + SIM_PIXEL_WOOD;
  if (material >= SIM_PIXEL_TYPE_COUNT ||
      offset % SIM_PALETTE_SWATCH_STRIDE >= SIM_PALETTE_SWATCH_WIDTH)
    return SIM_PIXEL_EMPTY;
  return (sim_pixel_type)material;
}

bool sim_brush_queue_push(sim_brush_queue* queue, sim_brush_command command) {
  if (!queue || queue->count >= SIM_BRUSH_QUEUE_CAPACITY)
    return false;
  uint32_t write_index =
      (queue->read_index + queue->count) % SIM_BRUSH_QUEUE_CAPACITY;
  queue->commands[write_index] = command;
  ++queue->count;
  return true;
}

bool sim_brush_queue_pop(sim_brush_queue* queue, sim_brush_command* command) {
  if (!queue || !command || queue->count == 0)
    return false;
  *command = queue->commands[queue->read_index];
  queue->read_index = (queue->read_index + 1) % SIM_BRUSH_QUEUE_CAPACITY;
  --queue->count;
  return true;
}

uint32_t sim_brush_cell_count(int radius) {
  if (radius < 1)
    radius = 1;
  else if (radius > SIM_MAX_BRUSH_RADIUS)
    radius = SIM_MAX_BRUSH_RADIUS;
  uint32_t count = 0;
  for (int y = -radius; y <= radius; ++y) {
    for (int x = -radius; x <= radius; ++x) {
      if (x * x + y * y <= radius * radius)
        ++count;
    }
  }
  return count;
}

sim_brush_bounds sim_clip_brush_bounds(sim_brush_command command) {
  const int radius = command.radius < 0 ? 0 : command.radius;
  const int first_x = command.x - radius < 0 ? 0 : command.x - radius;
  const int first_y = command.y - radius < 0 ? 0 : command.y - radius;
  const int last_x = command.x + radius >= SIM_WIDTH
                         ? SIM_WIDTH - 1
                         : command.x + radius;
  const int last_y = command.y + radius >= SIM_HEIGHT
                         ? SIM_HEIGHT - 1
                         : command.y + radius;
  return (sim_brush_bounds){first_x, first_y, last_x - first_x + 1,
                            last_y - first_y + 1};
}

uint32_t sim_consume_fixed_steps(uint64_t* accumulator,
                                 uint64_t elapsed_ticks,
                                 uint64_t ticks_per_second) {
  if (!accumulator || ticks_per_second == 0)
    return 0;

  const uint64_t maximum_accumulator =
      ticks_per_second > UINT64_MAX / SIM_MAX_CATCH_UP_STEPS
          ? UINT64_MAX
          : ticks_per_second * SIM_MAX_CATCH_UP_STEPS;
  const uint64_t scaled_elapsed =
      elapsed_ticks > maximum_accumulator / SIM_UPDATES_PER_SECOND
          ? maximum_accumulator
          : elapsed_ticks * SIM_UPDATES_PER_SECOND;
  if (scaled_elapsed >= maximum_accumulator ||
      *accumulator >= maximum_accumulator - scaled_elapsed)
    *accumulator = maximum_accumulator;
  else
    *accumulator += scaled_elapsed;

  const uint32_t steps =
      (uint32_t)(*accumulator / ticks_per_second);
  *accumulator -= (uint64_t)steps * ticks_per_second;
  return steps;
}
