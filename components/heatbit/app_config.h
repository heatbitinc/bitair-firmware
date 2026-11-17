#pragma once

#include "sdkconfig.h"

/* External backend endpoints and hosts, set in sdkconfig.defaults.heatbit */
#ifndef HEATBIT_API_HOST
#define HEATBIT_API_HOST CONFIG_HEATBIT_API_HOST
#endif

/* Helper to stringify and concatenate URL parts */
#define HEATBIT_URL(host, path) "https://" host path
#define HEATBIT_WS_URL(host, path) "wss://" host path

#define HEATBIT_API_METRICS_URL \
  HEATBIT_URL(HEATBIT_API_HOST, CONFIG_HEATBIT_API_METRICS_PATH)
#define HEATBIT_API_POOL_CONFIG_URL \
  HEATBIT_URL(HEATBIT_API_HOST, CONFIG_HEATBIT_API_POOL_CONFIG_PATH)
#define HEATBIT_API_WS_URL HEATBIT_WS_URL(HEATBIT_API_HOST, CONFIG_HEATBIT_API_WS_PATH)

// #define HEATBIT_DEVICE_MODEL "maxi"
#define HEATBIT_DEVICE_MODEL "bair" // BLE
#define HEATBIT_DEVICE_MODEL_BACKEND "bitair"

#define HEATBIT_METRICS_SEND_PERIOD_MS 30000U
#define HEATBIT_METRICS_CONNECTIVITY_TIMEOUT_MS 4000U
#define HEATBIT_POOL_CONFIG_REQUEST_TIMEOUT_MS 10000U
