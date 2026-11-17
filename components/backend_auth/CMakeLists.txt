# Backend authentication per channel (TLS and client credentials).
idf_component_register(
    SRCS "backend_auth.c"
    INCLUDE_DIRS "."
    REQUIRES
        device_identity
        device_state
        esp_http_client
        esp_timer
        esp_websocket_client
        mbedtls
        mqtt
)
