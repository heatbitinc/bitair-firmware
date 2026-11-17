#pragma once

#include "esp_err.h"
#include "global_state.h"

/*
 * Wires device state changes to their effects: network status in GlobalState,
 * LED indication, the mining gate of the power management task and the backend
 * status channel. Call once right after state_service_init(), before anything
 * changes the state.
 */
esp_err_t state_listeners_register(GlobalState *global_state);
