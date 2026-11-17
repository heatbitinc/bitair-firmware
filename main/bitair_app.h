#pragma once

#include "esp_err.h"
#include "global_state.h"

/*
 * Bitair hooks into the upstream startup sequence in app_main, in call order.
 */

/* Logging defaults, event loop and OTA events, LED, system status. */
void bitair_app_init_early(GlobalState *gs);

/* After NVS: device state and its listeners, telemetry and pool-config hooks,
 * device identity and certificate. */
esp_err_t bitair_app_init_state(GlobalState *gs);

/* After device_config_init(): board defaults, power management binding. */
void bitair_app_init_board(GlobalState *gs);

/* After SYSTEM_init_peripherals(): PWM input, fan, Wi-Fi/BLE, cloud services,
 * status reporter. */
void bitair_app_start_services(GlobalState *gs, esp_err_t system_init_ret);

/* Once the network is up: mining workers and the startup mining decision. */
void bitair_app_start_mining(GlobalState *gs, esp_err_t *system_init_ret);

/* Before the protocol coordinator starts: V/F calibration (development builds). */
void bitair_app_after_mining_start(GlobalState *gs);
