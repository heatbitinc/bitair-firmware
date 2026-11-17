#include "backoff.h"

#include "esp_random.h"

uint32_t backoff_next_ms(backoff_t *backoff)
{
    uint32_t base = backoff->current_ms;
    uint64_t doubled = (uint64_t)backoff->current_ms * 2;
    backoff->current_ms = doubled > backoff->max_ms ? backoff->max_ms : (uint32_t)doubled;

    /* +/-20% jitter, never above the maximum. */
    uint32_t span = base / 5;
    uint32_t delay = base - span + (span > 0 ? esp_random() % (2 * span + 1) : 0);
    return delay > backoff->max_ms ? backoff->max_ms : delay;
}

void backoff_reset(backoff_t *backoff)
{
    backoff->current_ms = backoff->min_ms;
}
