#ifndef FAN_CONTROLLER_TASK_H_
#define FAN_CONTROLLER_TASK_H_

#include "global_state.h"
void FAN_CONTROLLER_task(void * pvParameters);
void update_fan_speed(GlobalState * GLOBAL_STATE, float target_perc, const char * context);

#endif /* FAN_CONTROLLER_TASK_H_ */
