#include "sim_palette.h"

static const char* k_names[SIM_PIXEL_TYPE_COUNT] = {
    "Erase", "Wood",  "Iron", "Sand", "Rust", "Water", "Lava",
    "Smoke", "Steam", "Acid", "Fire", "Oil",  "Salt",  "Bedrock",
};

static const char* k_descriptions[SIM_PIXEL_TYPE_COUNT] = {
    "Remove material from the brush area.",
    "Rigid fuel that catches fire near heat.",
    "Heavy solid that rust and acid can erode.",
    "Granular powder that falls and piles up.",
    "Granular corrosion that spreads through iron.",
    "Dense liquid that cools lava and extinguishes fire.",
    "Hot liquid that ignites fuel and turns water to steam.",
    "Light gas that rises and drifts.",
    "Hot vapor that rises and can condense into water.",
    "Corrosive liquid that dissolves most materials.",
    "Hot gas that spreads through wood and oil.",
    "Buoyant liquid fuel that floats on water.",
    "Granular mineral that slowly dissolves in water.",
    "Permanent solid that resists reactions.",
};

sim_pixel_type sim_palette_material_at(sim_viewport viewport,
                                       int client_x,
                                       int client_y) {
  int cell_x;
  int cell_y;
  if (!sim_viewport_to_cell(viewport, client_x, client_y, &cell_x, &cell_y) ||
      cell_y < SIM_PALETTE_Y || cell_x < SIM_PALETTE_X)
    return SIM_PIXEL_TYPE_COUNT;

  const int local_y = cell_y - SIM_PALETTE_Y;
  const int offset = cell_x - SIM_PALETTE_X;
  const int column = offset / SIM_PALETTE_SWATCH_STRIDE;
  if (column >= SIM_PALETTE_COLUMNS ||
      local_y >= SIM_PALETTE_SWATCH_HEIGHT ||
      offset % SIM_PALETTE_SWATCH_STRIDE >= SIM_PALETTE_SWATCH_WIDTH)
    return SIM_PIXEL_TYPE_COUNT;
  return (sim_pixel_type)column;
}

const char* sim_palette_name(sim_pixel_type material) {
  return material >= SIM_PIXEL_EMPTY && material < SIM_PIXEL_TYPE_COUNT
             ? k_names[material]
             : k_names[SIM_PIXEL_EMPTY];
}

const char* sim_palette_description(sim_pixel_type material) {
  return material >= SIM_PIXEL_EMPTY && material < SIM_PIXEL_TYPE_COUNT
             ? k_descriptions[material]
             : k_descriptions[SIM_PIXEL_EMPTY];
}

char sim_palette_shortcut(sim_pixel_type material) {
  if (material == SIM_PIXEL_EMPTY)
    return '0';
  if (material >= SIM_PIXEL_WOOD && material <= SIM_PIXEL_ACID)
    return (char)('0' + material);
  if (material >= SIM_PIXEL_FIRE && material <= SIM_PIXEL_BEDROCK)
    return (char)('A' + material - SIM_PIXEL_FIRE);
  return '?';
}
