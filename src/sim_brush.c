#include "sim_brush.h"

sim_brush_command sim_make_brush_command(
    sim_pixel_type type,
    int x,
    int y,
    int radius,
    uint32_t seed) {
  sim_brush_command command;
  command.type = type < SIM_PIXEL_EMPTY || type >= SIM_PIXEL_TYPE_COUNT
                     ? SIM_PIXEL_SAND
                     : (uint32_t)type;
  command.x = x < 0 ? 0 : (x >= SIM_WIDTH ? SIM_WIDTH - 1 : x);
  command.y = y < 0 ? 0 : (y >= SIM_HEIGHT ? SIM_HEIGHT - 1 : y);
  command.radius = radius < 1
                       ? 1
                       : (radius > SIM_MAX_BRUSH_RADIUS
                              ? SIM_MAX_BRUSH_RADIUS
                              : radius);
  command.seed = seed;
  return command;
}

bool sim_brush_queue_push(sim_brush_queue* queue, sim_brush_command command) {
  if (!queue || queue->count >= SIM_BRUSH_QUEUE_CAPACITY)
    return false;
  const uint32_t write_index =
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
