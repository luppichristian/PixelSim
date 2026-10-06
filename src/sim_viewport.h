#pragma once
#include <stdbool.h>

typedef struct sim_viewport {
  int x;
  int y;
  int width;
  int height;
} sim_viewport;

sim_viewport sim_make_viewport(int client_width, int client_height);
bool sim_viewport_to_cell(
    sim_viewport viewport,
    int client_x,
    int client_y,
    int* cell_x,
    int* cell_y);

