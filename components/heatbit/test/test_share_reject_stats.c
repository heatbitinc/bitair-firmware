#include <stdio.h>
#include <string.h>

#include "share_reject_stats.h"
#include "unity.h"

static void test_empty_interval(void)
{
    share_reject_stats_t stats = {0};
    cJSON *json = share_reject_stats_to_json(&stats);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(json));
    cJSON_Delete(json);
}

static void test_counts_and_successive_intervals(void)
{
    share_reject_stats_t stats = {0};
    share_reject_stats_record(&stats, "stale");
    share_reject_stats_record(&stats, "stale");
    share_reject_stats_record(&stats, "duplicate share");

    cJSON *json = share_reject_stats_to_json(&stats);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetObjectItemCaseSensitive(json, "stale")->valueint);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetObjectItemCaseSensitive(json, "duplicate share")->valueint);
    cJSON_Delete(json);

    share_reject_stats_t sent = stats;
    share_reject_stats_commit(&stats, &sent);
    TEST_ASSERT_EQUAL_UINT(0, stats.count);
    share_reject_stats_record(&stats, "stale");
    TEST_ASSERT_EQUAL_UINT64(1, stats.reasons[0].count);
}

static void test_rejects_during_publish_remain_pending(void)
{
    share_reject_stats_t stats = {0};
    share_reject_stats_record(&stats, "stale");
    share_reject_stats_t sent = stats;
    share_reject_stats_record(&stats, "stale");
    share_reject_stats_record(&stats, "duplicate");
    share_reject_stats_commit(&stats, &sent);

    TEST_ASSERT_EQUAL_UINT(2, stats.count);
    TEST_ASSERT_EQUAL_UINT64(1, stats.reasons[0].count);
    TEST_ASSERT_EQUAL_UINT64(1, stats.reasons[1].count);

    sent = stats;
    share_reject_stats_record(&stats, "duplicate");
    share_reject_stats_commit(&stats, &sent);
    TEST_ASSERT_EQUAL_UINT(1, stats.count);
    TEST_ASSERT_EQUAL_STRING("duplicate", stats.reasons[0].reason);
    TEST_ASSERT_EQUAL_UINT64(1, stats.reasons[0].count);
}

static void test_unsent_snapshot_does_not_reset_counts(void)
{
    share_reject_stats_t stats = {0};
    share_reject_stats_record(&stats, "stale");
    cJSON *json = share_reject_stats_to_json(&stats);
    TEST_ASSERT_NOT_NULL(json);
    cJSON_Delete(json);
    // A failed publish must not commit the snapshot.
    share_reject_stats_record(&stats, "stale");
    TEST_ASSERT_EQUAL_UINT64(2, stats.reasons[0].count);
}

static void test_overflow_and_slot_reuse(void)
{
    share_reject_stats_t stats = {0};
    for (size_t i = 0; i < SHARE_REJECT_REASON_LIMIT; i++) {
        char reason[32];
        snprintf(reason, sizeof(reason), "reason-%u", (unsigned)i);
        share_reject_stats_record(&stats, reason);
    }
    share_reject_stats_record(&stats, "overflow");
    share_reject_stats_record(&stats, "__other__");
    share_reject_stats_record(&stats, "reason-0");
    TEST_ASSERT_EQUAL_UINT(SHARE_REJECT_REASON_LIMIT, stats.count);
    TEST_ASSERT_EQUAL_UINT64(2, stats.other_count);
    TEST_ASSERT_EQUAL_UINT64(2, stats.reasons[0].count);
    cJSON *json = share_reject_stats_to_json(&stats);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetObjectItemCaseSensitive(json, "__other__")->valueint);
    cJSON_Delete(json);

    share_reject_stats_t sent = stats;
    share_reject_stats_record(&stats, "overflow");
    share_reject_stats_commit(&stats, &sent);
    TEST_ASSERT_EQUAL_UINT(0, stats.count);
    TEST_ASSERT_EQUAL_UINT64(1, stats.other_count);
    share_reject_stats_record(&stats, "new reason");
    TEST_ASSERT_EQUAL_UINT(1, stats.count);
    TEST_ASSERT_EQUAL_STRING("new reason", stats.reasons[0].reason);
}

static void test_reason_normalization_and_escaping(void)
{
    share_reject_stats_t stats = {0};
    share_reject_stats_record(&stats, NULL);
    share_reject_stats_record(&stats, "");
    TEST_ASSERT_EQUAL_STRING("unknown", stats.reasons[0].reason);
    TEST_ASSERT_EQUAL_UINT64(2, stats.reasons[0].count);

    const char *reason = "quote\" backslash\\ newline\n";
    share_reject_stats_record(&stats, reason);
    char long_reason[128];
    memset(long_reason, 'x', sizeof(long_reason) - 1);
    long_reason[sizeof(long_reason) - 1] = '\0';
    share_reject_stats_record(&stats, long_reason);
    share_reject_stats_record(&stats, long_reason);
    TEST_ASSERT_EQUAL_UINT(SHARE_REJECT_REASON_SIZE - 1, strlen(stats.reasons[2].reason));
    TEST_ASSERT_EQUAL_UINT64(2, stats.reasons[2].count);

    cJSON *json = share_reject_stats_to_json(&stats);
    TEST_ASSERT_NOT_NULL(json);
    char *text = cJSON_PrintUnformatted(json);
    TEST_ASSERT_NOT_NULL(text);
    cJSON *parsed = cJSON_Parse(text);
    TEST_ASSERT_NOT_NULL(parsed);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetObjectItemCaseSensitive(parsed, reason)->valueint);
    cJSON_Delete(parsed);
    cJSON_free(text);
    cJSON_Delete(json);
}

static void test_worst_case_escaped_reasons(void)
{
    share_reject_stats_t stats = {0};
    for (size_t i = 0; i < SHARE_REJECT_REASON_LIMIT; i++) {
        char reason[SHARE_REJECT_REASON_SIZE];
        memset(reason, '\x01', sizeof(reason) - 1);
        reason[0] = (char)('a' + i);
        reason[sizeof(reason) - 1] = '\0';
        share_reject_stats_record(&stats, reason);
        stats.reasons[i].count = UINT32_MAX;
    }
    cJSON *json = share_reject_stats_to_json(&stats);
    TEST_ASSERT_NOT_NULL(json);
    char *text = cJSON_PrintUnformatted(json);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_LESS_THAN_UINT(4096, strlen(text));
    cJSON *parsed = cJSON_Parse(text);
    TEST_ASSERT_NOT_NULL(parsed);
    TEST_ASSERT_EQUAL_INT(SHARE_REJECT_REASON_LIMIT, cJSON_GetArraySize(parsed));
    cJSON_Delete(parsed);
    cJSON_free(text);
    cJSON_Delete(json);
}

#ifdef ESP_PLATFORM
TEST_CASE("share reject reasons track MQTT intervals", "[heatbit]")
{
    test_empty_interval();
    test_counts_and_successive_intervals();
    test_rejects_during_publish_remain_pending();
    test_unsent_snapshot_does_not_reset_counts();
    test_overflow_and_slot_reuse();
    test_reason_normalization_and_escaping();
    test_worst_case_escaped_reasons();
}
#else
void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty_interval);
    RUN_TEST(test_counts_and_successive_intervals);
    RUN_TEST(test_rejects_during_publish_remain_pending);
    RUN_TEST(test_unsent_snapshot_does_not_reset_counts);
    RUN_TEST(test_overflow_and_slot_reuse);
    RUN_TEST(test_reason_normalization_and_escaping);
    RUN_TEST(test_worst_case_escaped_reasons);
    return UNITY_END();
}
#endif
