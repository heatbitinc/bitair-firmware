#include "firmware_version.h"

#include "esp_app_desc.h"

const char *firmware_version_get_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc == NULL || desc->version[0] == '\0') {
        return "unknown";
    }

    return desc->version;
}
