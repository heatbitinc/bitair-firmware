#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void monitor_logging_service_apply_default(void);
void monitor_logging_service_set_enabled(bool enabled);
bool monitor_logging_service_is_enabled(void);

#ifdef __cplusplus
}
#endif