#include "sim_clock.h"

#include <stdint.h>

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
