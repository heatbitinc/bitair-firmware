#pragma once

#include <stdint.h>

/*
 * Retry delay that doubles after each failure up to a maximum, with +/-20%
 * jitter so that a fleet of devices does not retry in lockstep after a
 * backend outage. Reset it after a success.
 */
typedef struct {
    uint32_t min_ms;
    uint32_t max_ms;
    uint32_t current_ms;
} backoff_t;

#define BACKOFF_INIT(min, max) {.min_ms = (min), .max_ms = (max), .current_ms = (min)}

/* Delay to wait before the next attempt; advances the backoff. */
uint32_t backoff_next_ms(backoff_t *backoff);

void backoff_reset(backoff_t *backoff);
