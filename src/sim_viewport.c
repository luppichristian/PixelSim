#include "sim_viewport.h"

#include <stdint.h>

#include "sim.h"

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

bool sim_viewport_to_cell(sim_viewport viewport,
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
