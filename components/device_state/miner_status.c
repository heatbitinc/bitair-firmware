#include <string.h>

#include "miner_status.h"

static miner_status_provider_t s_provider;

void miner_status_set_provider(miner_status_provider_t provider)
{
    s_provider = provider;
}

esp_err_t miner_status_get(miner_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (s_provider == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_provider(out);
    return ESP_OK;
}
