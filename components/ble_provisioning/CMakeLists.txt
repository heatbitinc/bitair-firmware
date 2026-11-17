# BLE GATT service for Wi-Fi provisioning from the mobile app.
# Moved as is from components/heatbit; behaviour unchanged.
idf_component_register(
    SRCS "ble_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        bitair_hw
        bt
        device_identity
        device_state
        heatbit
        mbedtls
        wifi_manager
)
