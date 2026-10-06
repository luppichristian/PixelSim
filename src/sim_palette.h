#pragma once
#include "sim.h"
#include "sim_viewport.h"

#define SIM_PALETTE_X 8
#define SIM_PALETTE_Y 6
#define SIM_PALETTE_SWATCH_WIDTH 24
#define SIM_PALETTE_SWATCH_HEIGHT 24
#define SIM_PALETTE_SWATCH_STRIDE 28
#define SIM_PALETTE_COLUMNS SIM_PIXEL_TYPE_COUNT

sim_pixel_type sim_palette_material_at(
    sim_viewport viewport,
    int client_x,
    int client_y);
const char* sim_palette_name(sim_pixel_type material);
const char* sim_palette_description(sim_pixel_type material);
char sim_palette_shortcut(sim_pixel_type material);

