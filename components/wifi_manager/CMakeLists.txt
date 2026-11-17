# Wi-Fi station: connect, reconnect with factory-network fallback, internet
# reachability check. Reports connectivity through device_state.
idf_component_register(
    SRCS "wifi_connect.c"
    INCLUDE_DIRS "."
    REQUIRES
        device_state
        esp_event
        esp_netif
        esp_wifi
        lwip
)
