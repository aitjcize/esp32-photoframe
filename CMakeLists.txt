cmake_minimum_required(VERSION 3.16)

# Get version from centralized script
execute_process(
    COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/get_version.py --type auto
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# Fallback if script fails
if(NOT PROJECT_VER)
    set(PROJECT_VER "dev-unknown")
endif()

message(STATUS "Project version: ${PROJECT_VER}")

# Allow FIRMWARE_VERSION to be overridden from command line, otherwise use PROJECT_VER
if(NOT DEFINED FIRMWARE_VERSION)
    set(FIRMWARE_VERSION "${PROJECT_VER}")
endif()

message(STATUS "Firmware version: ${FIRMWARE_VERSION}")

add_compile_options(-Wno-missing-field-initializers)
add_compile_definitions(FIRMWARE_VERSION="${FIRMWARE_VERSION}")

# Very early parsing of sdkconfig to forcefully copy the correct partition table
# before ESP-IDF even starts analyzing components.
set(TARGET_CSV "partitions/partitions_8mb.csv") # Default fallback
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig")
    file(STRINGS "${CMAKE_SOURCE_DIR}/sdkconfig" SDKCONFIG_LINES)
    foreach(line ${SDKCONFIG_LINES})
        if(line MATCHES "^CONFIG_INTERNAL_STORAGE_PARTITION_TABLE=\"(.*)\"$")
            set(TARGET_CSV ${CMAKE_MATCH_1})
        endif()
    endforeach()
endif()

message(STATUS "Selected partition table path: ${TARGET_CSV}")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/${TARGET_CSV}" "${CMAKE_SOURCE_DIR}/partitions.csv")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-photoframe)
