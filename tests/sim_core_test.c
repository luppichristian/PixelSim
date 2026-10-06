#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "sim.h"
#include "sim_brush.h"
#include "sim_clock.h"
#include "sim_palette.h"
#include "sim_viewport.h"

static void test_brush(void) {
  const sim_brush_command command =
      sim_make_brush_command(SIM_PIXEL_WATER, -5, 9999, 7, 123u);
  assert(command.type == SIM_PIXEL_WATER);
  assert(command.x == 0);
  assert(command.y == SIM_HEIGHT - 1);
  assert(command.radius == 7);
  assert(command.seed == 123u);
  assert(sim_make_brush_command(SIM_PIXEL_EMPTY, 20, 20, 4, 0u).type ==
         SIM_PIXEL_EMPTY);
  assert(sim_make_brush_command(SIM_PIXEL_WATER, 20, 20, 1000000, 0u)
             .radius == SIM_MAX_BRUSH_RADIUS);
  assert(sim_brush_cell_count(5) == 81u);

  const sim_brush_bounds corner = sim_clip_brush_bounds(
      (sim_brush_command){SIM_PIXEL_WATER, 0, 0, 5, 0u});
  assert(corner.x == 0 && corner.y == 0);
  assert(corner.width == 6 && corner.height == 6);

  sim_brush_queue queue = {0};
  sim_brush_command output;
  assert(sim_brush_queue_push(&queue, command));
  assert(sim_brush_queue_pop(&queue, &output));
  assert(output.seed == command.seed);
  assert(!sim_brush_queue_pop(&queue, &output));
}

static void test_viewport(void) {
  const sim_viewport wide = sim_make_viewport(1280, 720);
  const sim_viewport square = sim_make_viewport(1000, 1000);
  assert(wide.x == 0 && wide.y == 0);
  assert(wide.width == 1280 && wide.height == 720);
  assert(square.x == 0 && square.y == 219);
  assert(square.width == 1000 && square.height == 562);

  int cell_x = -1;
  int cell_y = -1;
  assert(sim_viewport_to_cell(square, 500, 500, &cell_x, &cell_y));
  assert(cell_x == SIM_WIDTH / 2 && cell_y == SIM_HEIGHT / 2);
  assert(!sim_viewport_to_cell(square, 500, 100, &cell_x, &cell_y));
}

static void test_palette(void) {
  const sim_viewport viewport = sim_make_viewport(1280, 720);
  assert(SIM_PALETTE_SWATCH_WIDTH == SIM_PALETTE_SWATCH_HEIGHT);
  for (int material = SIM_PIXEL_EMPTY; material < SIM_PIXEL_TYPE_COUNT;
       ++material) {
    const int cell_x = SIM_PALETTE_X +
                       material * SIM_PALETTE_SWATCH_STRIDE +
                       SIM_PALETTE_SWATCH_WIDTH / 2;
    const int client_x = cell_x * viewport.width / SIM_WIDTH;
    const int client_y =
        (SIM_PALETTE_Y + SIM_PALETTE_SWATCH_HEIGHT / 2) * viewport.height /
        SIM_HEIGHT;
    assert(sim_palette_material_at(viewport, client_x, client_y) == material);
  }
  assert(strcmp(sim_palette_name(SIM_PIXEL_EMPTY), "Erase") == 0);
  assert(strcmp(sim_palette_name(SIM_PIXEL_LAVA), "Lava") == 0);
  assert(sim_palette_shortcut(SIM_PIXEL_EMPTY) == '0');
  assert(sim_palette_shortcut(SIM_PIXEL_BEDROCK) == 'D');
}

static void test_clock(void) {
  uint64_t accumulator = 0;
  uint32_t steps = 0;
  for (uint32_t frame = 0; frame < 144; ++frame)
    steps += sim_consume_fixed_steps(&accumulator, 50, 7200);
  assert(steps == SIM_UPDATES_PER_SECOND);
  assert(accumulator == 0);

  steps = sim_consume_fixed_steps(&accumulator, 7200, 7200);
  assert(steps == SIM_MAX_CATCH_UP_STEPS);
  assert(accumulator == 0);
  assert(SIM_PASSES_PER_UPDATE == 5u);
  assert((SIM_DISSOLVED_SALT_MASK & SIM_PIXEL_TYPE_MASK) == 0u);
}

int main(void) {
  test_brush();
  test_viewport();
  test_palette();
  test_clock();
  return 0;
}
