# Device state store (power, fan and work mode, Wi-Fi settings, sensor data)
# with change notifications. Depends on nothing application-level.
idf_component_register(
    SRCS "state_service.c" "miner_status.c" "system_status.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash
)
