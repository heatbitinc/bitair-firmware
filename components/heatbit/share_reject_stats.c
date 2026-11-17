#include "share_reject_stats.h"

#include <string.h>

void share_reject_stats_record(share_reject_stats_t *stats, const char *reason)
{
    char key[SHARE_REJECT_REASON_SIZE] = {0};
    if (reason == NULL || reason[0] == '\0') {
        reason = "unknown";
    }
    strncpy(key, reason, sizeof(key) - 1);

    if (strcmp(key, "__other__") == 0) {
        stats->other_count++;
        return;
    }

    for (size_t i = 0; i < stats->count; i++) {
        if (strcmp(stats->reasons[i].reason, key) == 0) {
            stats->reasons[i].count++;
            return;
        }
    }

    if (stats->count == SHARE_REJECT_REASON_LIMIT) {
        stats->other_count++;
        return;
    }

    share_reject_reason_t *entry = &stats->reasons[stats->count++];
    memcpy(entry->reason, key, sizeof(entry->reason));
    entry->count = 1;
}

void share_reject_stats_commit(share_reject_stats_t *stats, const share_reject_stats_t *sent)
{
    for (size_t i = 0; i < sent->count; i++) {
        for (size_t j = 0; j < stats->count; j++) {
            if (strcmp(stats->reasons[j].reason, sent->reasons[i].reason) == 0) {
                stats->reasons[j].count -= sent->reasons[i].count;
                break;
            }
        }
    }
    stats->other_count -= sent->other_count;

    size_t remaining = 0;
    for (size_t i = 0; i < stats->count; i++) {
        if (stats->reasons[i].count != 0) {
            stats->reasons[remaining++] = stats->reasons[i];
        }
    }
    stats->count = remaining;
}

cJSON *share_reject_stats_to_json(const share_reject_stats_t *stats)
{
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < stats->count; i++) {
        if (cJSON_AddNumberToObject(json, stats->reasons[i].reason, stats->reasons[i].count) == NULL) {
            cJSON_Delete(json);
            return NULL;
        }
    }
    if (stats->other_count != 0 && cJSON_AddNumberToObject(json, "__other__", stats->other_count) == NULL) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}
