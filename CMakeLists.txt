# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.30)

set(CMAKE_CXX_STANDARD 11)

# (Not part of the boilerplate)
# This example uses an extra component for common functions such as Wi-Fi and Ethernet connection.
# set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)
if(NOT DEFINED FW_VERSION AND DEFINED ENV{FW_VERSION})
    set(FW_VERSION "$ENV{FW_VERSION}")
endif()

if(NOT DEFINED FW_VERSION OR FW_VERSION STREQUAL "")
    set(FW_VERSION "0.0.0")
endif()

set(ESP_PLATFORM "esp32s3")
set(IDF_TARGET "esp32s3")

# Backend endpoints live in sdkconfig.defaults.heatbit, which is not in git.
# See sdkconfig.defaults.heatbit.example.
if(NOT DEFINED SDKCONFIG_DEFAULTS AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.heatbit")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.heatbit")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bitair-miner C CXX)

idf_build_get_property(executable EXECUTABLE)
idf_build_get_property(executable_name EXECUTABLE_NAME)
idf_build_get_property(executable_dir EXECUTABLE_DIR)
idf_build_get_property(build_dir BUILD_DIR)
idf_build_get_property(idf_target IDF_TARGET)

#idf_build_executable(${CMAKE_PROJECT_NAME}.elf)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Generic" )
    add_custom_command(
      OUTPUT ${executable_dir}/merge_bins.stamp
      COMMAND touch ota_data_initial.bin
      COMMAND ${PYTHON} ${IDF_PATH}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py generate --outdir ./ ../configs/config-bitair.cvs config.bin 0x6000
      COMMAND sha256sum config.bin > config.bin.sha256
      COMMAND cat flash_project_args | xargs ${PYTHON} -m esptool --chip ${idf_target} merge_bin --output ${executable_name}_full.bin
      COMMAND sha256sum ${executable_name}.bin > ${executable_name}.bin.sha256
      COMMAND cp ${executable_name}.bin ota.bin
      COMMAND cp ${executable_name}.bin.sha256 ota.bin.sha256
      COMMAND sha256sum ${executable_name}_full.bin > ${executable_name}_full.sha256
      COMMAND touch merge_bins.stamp
      DEPENDS ${executable_name}.bin flash_project_args
      WORKING_DIRECTORY ${executable_dir}
      VERBATIM
    )
    add_custom_target(merge_bins DEPENDS ${executable_dir}/merge_bins.stamp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" OR NOT DEFINED CMAKE_SYSTEM_NAME)
    message(STATUS "Windows detected")
endif ()
