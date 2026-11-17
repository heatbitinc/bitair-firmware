#ifndef POWER_MANAGEMENT_TASK_H_
#define POWER_MANAGEMENT_TASK_H_

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    float fan_perc;
    uint16_t fan_rpm;
    uint16_t fan2_rpm;
    float chip_temp_avg;
    float chip_temp2_avg;
    float vr_temp;
    float voltage;
    float frequency_value;
    float actual_frequency;    
    float expected_hashrate;
    float power;
    float current;
    float core_voltage;
} PowerManagementModule;

void POWER_MANAGEMENT_init_frequency(void * pvParameters);

void POWER_MANAGEMENT_task(void * pvParameters);

void POWER_MANAGEMENT_bind_global_state(void *global_state);
esp_err_t POWER_MANAGEMENT_set_mining_enabled(bool enabled);

/*
 * Mining is started and stopped only by the power management task. It does
 * not start before app_main reports that the network is up; app_main then
 * waits for the result of that first start instead of initialising the ASIC
 * itself.
 */
void POWER_MANAGEMENT_set_network_ready(void);
/* Chip count of the first start attempt after network-ready, or 0 on timeout. */
uint8_t POWER_MANAGEMENT_wait_first_start(TickType_t timeout);

#endif
