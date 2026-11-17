#pragma once

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Adds a "security" object describing the device protection state: device
 * certificate, Secure Boot / Flash Encryption, relevant eFuses, bootloader
 * and running partition.
 */
void security_info_add_to_json(cJSON *root);

#ifdef __cplusplus
}
#endif
