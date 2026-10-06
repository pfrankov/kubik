cmake_minimum_required(VERSION 3.16)
set(KUBIK_CHARACTER "TESS" CACHE STRING "Firmware character: TESS or PLUSH")
if(KUBIK_CHARACTER STREQUAL "PLUSH")
    set(PLUSH_CONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
    execute_process(COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/../tools/configure-plush.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig" "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults" "${PLUSH_CONFIG}"
        COMMAND_ERROR_IS_FATAL ANY)
    set(SDKCONFIG "${PLUSH_CONFIG}" CACHE FILEPATH "Selected IDF configuration" FORCE)
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kubik)

# `idf.py flash` builds only the flash target's own dependencies: make it repack the assets too.
add_dependencies(flash assets_image)
