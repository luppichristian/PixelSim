#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "sim_core.h"

static void TestMaterialProperties(void) {
  const sim_material sand = sim_get_material(SIM_PIXEL_SAND);
  const sim_material water = sim_get_material(SIM_PIXEL_WATER);
  const sim_material lava = sim_get_material(SIM_PIXEL_LAVA);
  const sim_material steam = sim_get_material(SIM_PIXEL_STEAM);

  assert(sand.phase == SIM_PHASE_POWDER);
  assert(sand.density > water.density);
  assert(water.phase == SIM_PHASE_LIQUID);
  assert(lava.temperature > 900.0f);
  assert(steam.phase == SIM_PHASE_GAS);
  assert(steam.buoyancy > 0.0f);
}

static void TestMaterialNamesMatchUserInterface(void) {
  assert(strcmp(sim_material_name(SIM_PIXEL_WOOD), "Wood") == 0);
  assert(strcmp(sim_material_name(SIM_PIXEL_LAVA), "Lava") == 0);
  assert(strcmp(sim_material_name(SIM_PIXEL_STEAM), "Steam") == 0);
}

static void TestBrushCommandClampsToSimulationBounds(void) {
  const sim_brush_command command =
      sim_make_brush_command(SIM_PIXEL_WATER, -5, 9999, 7, 123u);

  assert(command.type == SIM_PIXEL_WATER);
  assert(command.x == 0);
  assert(command.y == SIM_HEIGHT - 1);
  assert(command.radius == 7);
  assert(command.seed == 123u);
}

static void TestBrushCommandRejectsNonSpawnablePixels(void) {
  const sim_brush_command command =
      sim_make_brush_command(SIM_PIXEL_EMPTY, 20, 20, 4, 0u);

  assert(command.type == SIM_PIXEL_SAND);
  assert(command.radius == 4);
}

static void TestBrushCommandBoundsLargeRadius(void) {
  const sim_brush_command command =
      sim_make_brush_command(SIM_PIXEL_WATER, 20, 20, 1000000, 0u);

  assert(command.radius == SIM_MAX_BRUSH_RADIUS);
}

static void TestViewportPreservesSimulationAspectRatio(void) {
  const sim_viewport wide = sim_make_viewport(1280, 720);
  const sim_viewport square = sim_make_viewport(1000, 1000);

  assert(wide.x == 0);
  assert(wide.y == 0);
  assert(wide.width == 1280);
  assert(wide.height == 720);
  assert(square.x == 0);
  assert(square.y == 219);
  assert(square.width == 1000);
  assert(square.height == 562);
}

static void TestClientPointMapsThroughViewport(void) {
  const sim_viewport viewport = sim_make_viewport(1000, 1000);
  int cell_x = -1;
  int cell_y = -1;

  assert(sim_viewport_to_cell(viewport, 500, 500, &cell_x, &cell_y));
  assert(cell_x == SIM_WIDTH / 2);
  assert(cell_y == SIM_HEIGHT / 2);
  assert(!sim_viewport_to_cell(viewport, 500, 100, &cell_x, &cell_y));
  assert(!sim_viewport_to_cell(viewport, 1000, 500, &cell_x, &cell_y));
}

static void TestPaletteHitTestingUsesSimulationCells(void) {
  const sim_viewport viewport = sim_make_viewport(1280, 720);

  assert(sim_palette_material_at(viewport, 20, 20) == SIM_PIXEL_WOOD);
  assert(sim_palette_material_at(viewport, 228, 20) == SIM_PIXEL_SAND);
  assert(sim_palette_material_at(viewport, 20, 100) == SIM_PIXEL_EMPTY);
}

static void TestBrushQueueConsumesOneClickOnce(void) {
  sim_brush_queue queue = {0};
  sim_brush_command output;
  const sim_brush_command click =
      sim_make_brush_command(SIM_PIXEL_SAND, 120, 80, 7, 42u);

  assert(sim_brush_queue_push(&queue, click));
  assert(sim_brush_queue_pop(&queue, &output));
  assert(output.x == 120);
  assert(output.y == 80);
  assert(!sim_brush_queue_pop(&queue, &output));
}

static void TestRadiusFiveBrushContainsEightyOneCells(void) {
  assert(sim_brush_cell_count(5) == 81u);
}

static void TestBrushBoundsClipToSimulation(void) {
  const sim_brush_bounds centered = sim_clip_brush_bounds(
      (sim_brush_command){SIM_PIXEL_WATER, 20, 30, 5, 0u});
  const sim_brush_bounds corner = sim_clip_brush_bounds(
      (sim_brush_command){SIM_PIXEL_WATER, 0, 0, 5, 0u});
  const sim_brush_bounds point = sim_clip_brush_bounds(
      (sim_brush_command){SIM_PIXEL_WATER, SIM_WIDTH - 1,
                          SIM_HEIGHT - 1, 0, 0u});

  assert(centered.x == 15);
  assert(centered.y == 25);
  assert(centered.width == 11);
  assert(centered.height == 11);
  assert(corner.x == 0);
  assert(corner.y == 0);
  assert(corner.width == 6);
  assert(corner.height == 6);
  assert(point.x == SIM_WIDTH - 1);
  assert(point.y == SIM_HEIGHT - 1);
  assert(point.width == 1);
  assert(point.height == 1);
}

static void TestFixedSimulationRateIsIndependentOfRenderRate(void) {
  uint64_t accumulator = 0;
  uint32_t steps = 0;
  for (uint32_t frame = 0; frame < 144; ++frame)
    steps += sim_consume_fixed_steps(&accumulator, 50, 7200);

  assert(steps == 120u);
  assert(accumulator == 0);
}

static void TestFixedSimulationRateCapsCatchUpWork(void) {
  uint64_t accumulator = 0;
  const uint32_t steps =
      sim_consume_fixed_steps(&accumulator, 7200, 7200);

  assert(steps == SIM_MAX_CATCH_UP_STEPS);
  assert(accumulator == 0);
}

static void TestFixedSimulationRateCarriesFractionalTicksExactly(void) {
  uint64_t accumulator = 0;
  uint32_t steps = 0;
  for (uint32_t tick = 0; tick < 1000; ++tick)
    steps += sim_consume_fixed_steps(&accumulator, 1, 1000);

  assert(steps == SIM_UPDATES_PER_SECOND);
  assert(accumulator == 0);
}

static void TestEveryFixedUpdateUsesFiveGpuDispatches(void) {
  assert(SIM_PASSES_PER_UPDATE == 5u);
}

int main(void) {
  TestMaterialProperties();
  TestMaterialNamesMatchUserInterface();
  TestBrushCommandClampsToSimulationBounds();
  TestBrushCommandRejectsNonSpawnablePixels();
  TestBrushCommandBoundsLargeRadius();
  TestViewportPreservesSimulationAspectRatio();
  TestClientPointMapsThroughViewport();
  TestPaletteHitTestingUsesSimulationCells();
  TestBrushQueueConsumesOneClickOnce();
  TestRadiusFiveBrushContainsEightyOneCells();
  TestBrushBoundsClipToSimulation();
  TestFixedSimulationRateIsIndependentOfRenderRate();
  TestFixedSimulationRateCapsCatchUpWork();
  TestFixedSimulationRateCarriesFractionalTicksExactly();
  TestEveryFixedUpdateUsesFiveGpuDispatches();
  return 0;
}
