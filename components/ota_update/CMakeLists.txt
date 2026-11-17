idf_component_register(
  SRCS "ota_manager.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_http_client esp_https_ota esp_event esp_timer app_update mbedtls esp_wifi esp_netif

)
