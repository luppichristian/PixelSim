#pragma once
#include <stdint.h>

#define SIM_UPDATES_PER_SECOND 120u
#define SIM_MAX_CATCH_UP_STEPS 4u

uint32_t sim_consume_fixed_steps(
    uint64_t* accumulator,
    uint64_t elapsed_ticks,
    uint64_t ticks_per_second);

