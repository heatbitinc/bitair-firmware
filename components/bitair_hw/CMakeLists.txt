# Bitair board peripherals: indication LED, PWM power/fan input, TPM-300A air
# quality sensor. They report into device_state.
idf_component_register(
    SRCS
        "indication_service.c"
        "pwm_duty_monitor.c"
        "tpm300a_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        device_state
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_uart
        esp_timer
)
