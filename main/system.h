#ifndef SYSTEM_H_
#define SYSTEM_H_

#include "esp_err.h"
#include "global_state.h"
#include "sv2_protocol.h"

void SYSTEM_init_system(GlobalState * GLOBAL_STATE);
esp_err_t SYSTEM_init_peripherals(GlobalState * GLOBAL_STATE);

// Clear the stratum job queue and valid-job tracking on a clean-jobs event,
// and reset hashrate measurements so reconnects don't spike the average.
// Shared by the SV1 and SV2 tasks.
void SYSTEM_clean_jobs_queue(GlobalState * GLOBAL_STATE);

void SYSTEM_notify_submitted_share(GlobalState * GLOBAL_STATE);
void SYSTEM_notify_accepted_share(GlobalState * GLOBAL_STATE);
void SYSTEM_notify_rejected_share(GlobalState * GLOBAL_STATE, char * error_msg);
void SYSTEM_notify_found_nonce(GlobalState * GLOBAL_STATE, double diff, uint8_t job_id);
void SYSTEM_notify_new_ntime(GlobalState * GLOBAL_STATE, uint32_t ntime);
void SYSTEM_mark_mining_started(GlobalState * GLOBAL_STATE);
void SYSTEM_mark_mining_stopped(GlobalState * GLOBAL_STATE);
void SYSTEM_reload_stratum_settings_from_nvs(GlobalState * GLOBAL_STATE);

stratum_protocol_t stratum_protocol_from_string(const char *s);
sv2_channel_type_t sv2_channel_type_from_string(const char *s);

#endif /* SYSTEM_H_ */
