# Device identity: UUID and worker_id from NVS, and the device client
# certificate (written at manufacturing, renewed on the device).
idf_component_register(
    SRCS
        "device_id.c"
        "device_cert.c"
    INCLUDE_DIRS "."
    REQUIRES
        device_state
        esp_http_client
        esp_wifi
        json
        mbedtls
        nvs_flash
)

# Trust anchor of the enrollment server. certs/ is not tracked in git.
if(CONFIG_HEATBIT_ENROLL_ENABLED)
    set(HEATBIT_CERTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../certs")
    set(HEATBIT_CERT_FILES "ca_root.pem")
    foreach(cert_file ${HEATBIT_CERT_FILES})
        if(NOT EXISTS "${HEATBIT_CERTS_DIR}/${cert_file}")
            message(FATAL_ERROR "Enrollment is enabled but certs/${cert_file} is missing")
        endif()
        target_add_binary_data(${COMPONENT_LIB} "${HEATBIT_CERTS_DIR}/${cert_file}" TEXT)
    endforeach()
endif()
