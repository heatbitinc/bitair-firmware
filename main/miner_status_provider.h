#pragma once

#include "global_state.h"

/* Registers the miner_status provider backed by GlobalState. */
void miner_status_provider_register(GlobalState *global_state);
