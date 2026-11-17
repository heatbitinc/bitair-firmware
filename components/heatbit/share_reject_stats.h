#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#define SHARE_REJECT_REASON_LIMIT 10
#define SHARE_REJECT_REASON_SIZE 64

typedef struct {
    char reason[SHARE_REJECT_REASON_SIZE];
    uint64_t count;
} share_reject_reason_t;

typedef struct {
    share_reject_reason_t reasons[SHARE_REJECT_REASON_LIMIT];
    size_t count;
    uint64_t other_count;
} share_reject_stats_t;

// The caller must serialize access to the accumulator.
void share_reject_stats_record(share_reject_stats_t *stats, const char *reason);
void share_reject_stats_commit(share_reject_stats_t *stats, const share_reject_stats_t *sent);
cJSON *share_reject_stats_to_json(const share_reject_stats_t *stats);
