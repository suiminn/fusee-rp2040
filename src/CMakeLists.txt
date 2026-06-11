find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(FUSEE_PAYLOAD_BIN "" CACHE FILEPATH "Payload binary to embed in the generated RCM image; leave empty to download hekate")
set(FUSEE_HEKATE_TAG "" CACHE STRING "hekate release tag to download when FUSEE_PAYLOAD_BIN is empty; leave empty for the latest release")
set(FUSEE_INTERMEZZO_BIN "${CMAKE_CURRENT_LIST_DIR}/bin/intermezzo.bin" CACHE FILEPATH "Intermezzo relocator binary to embed in the generated RCM image")

get_filename_component(FUSEE_INTERMEZZO_BIN_ABS "${FUSEE_INTERMEZZO_BIN}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")

if (NOT EXISTS "${FUSEE_INTERMEZZO_BIN_ABS}")
    message(FATAL_ERROR "FUSEE_INTERMEZZO_BIN does not exist: ${FUSEE_INTERMEZZO_BIN_ABS}")
endif()

set(FUSEE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(FUSEE_RCM_IMAGE_INCLUDE "${FUSEE_GENERATED_DIR}/rcm_image.h")

if (FUSEE_PAYLOAD_BIN)
    get_filename_component(FUSEE_PAYLOAD_BIN_ABS "${FUSEE_PAYLOAD_BIN}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    get_filename_component(FUSEE_LEGACY_PAYLOAD_BIN_ABS "${CMAKE_CURRENT_LIST_DIR}/bin/hekate_ctcaer_6.5.2.bin" ABSOLUTE)

    if (NOT EXISTS "${FUSEE_PAYLOAD_BIN_ABS}" AND FUSEE_PAYLOAD_BIN_ABS STREQUAL FUSEE_LEGACY_PAYLOAD_BIN_ABS)
        message(STATUS "Ignoring missing legacy bundled hekate payload; hekate will be downloaded")
        set(FUSEE_PAYLOAD_BIN "" CACHE FILEPATH "Payload binary to embed in the generated RCM image; leave empty to download hekate" FORCE)
    elseif (NOT EXISTS "${FUSEE_PAYLOAD_BIN_ABS}")
        message(FATAL_ERROR "FUSEE_PAYLOAD_BIN does not exist: ${FUSEE_PAYLOAD_BIN_ABS}")
    endif()
endif()

if (FUSEE_PAYLOAD_BIN)
    set(FUSEE_RCM_PAYLOAD_BIN "${FUSEE_PAYLOAD_BIN_ABS}")
    set(FUSEE_RCM_PAYLOAD_DEPENDS "${FUSEE_PAYLOAD_BIN_ABS}")
else()
    if (FUSEE_HEKATE_TAG)
        string(REGEX REPLACE "[^A-Za-z0-9_.-]" "_" FUSEE_DOWNLOADED_PAYLOAD_SUFFIX "${FUSEE_HEKATE_TAG}")
    else()
        set(FUSEE_DOWNLOADED_PAYLOAD_SUFFIX "latest")
    endif()
    set(FUSEE_DOWNLOADED_PAYLOAD_BIN "${FUSEE_GENERATED_DIR}/hekate_payload_${FUSEE_DOWNLOADED_PAYLOAD_SUFFIX}.bin")

    set(FUSEE_DOWNLOAD_HEKATE_ARGS
        --output "${FUSEE_DOWNLOADED_PAYLOAD_BIN}"
    )
    if (FUSEE_HEKATE_TAG)
        list(APPEND FUSEE_DOWNLOAD_HEKATE_ARGS --tag "${FUSEE_HEKATE_TAG}")
    endif()

    add_custom_command(
        OUTPUT "${FUSEE_DOWNLOADED_PAYLOAD_BIN}"
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_SOURCE_DIR}/tools/download_hekate_payload.py"
                ${FUSEE_DOWNLOAD_HEKATE_ARGS}
        DEPENDS
            "${CMAKE_SOURCE_DIR}/tools/download_hekate_payload.py"
        COMMENT "Downloading hekate payload"
        VERBATIM
    )

    set(FUSEE_RCM_PAYLOAD_BIN "${FUSEE_DOWNLOADED_PAYLOAD_BIN}")
    set(FUSEE_RCM_PAYLOAD_DEPENDS "${FUSEE_DOWNLOADED_PAYLOAD_BIN}")
endif()

add_custom_command(
    OUTPUT "${FUSEE_RCM_IMAGE_INCLUDE}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/build_rcm_image.py"
            --payload "${FUSEE_RCM_PAYLOAD_BIN}"
            --intermezzo "${FUSEE_INTERMEZZO_BIN_ABS}"
            --output "${FUSEE_RCM_IMAGE_INCLUDE}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/build_rcm_image.py"
        "${FUSEE_RCM_PAYLOAD_DEPENDS}"
        "${FUSEE_INTERMEZZO_BIN_ABS}"
    VERBATIM
)

add_custom_target(generate_rcm_image DEPENDS "${FUSEE_RCM_IMAGE_INCLUDE}")

add_executable(fusee fusee.c)
add_dependencies(fusee generate_rcm_image)

set(FUSEE_DEBUG_UART OFF CACHE BOOL "Enable UART debug logging")
set(FUSEE_STATUS_LED_PIN "" CACHE STRING "Override the board status LED GPIO pin; leave empty to use PICO_BOARD defaults")
set(FUSEE_STATUS_LED_INVERTED "" CACHE STRING "Override status LED polarity; true for active-low, false for active-high")
set(FUSEE_STATUS_LED_ENABLED "" CACHE STRING "Override status LED availability; false disables regular and colored status LEDs")
set(FUSEE_STATUS_LED_HOLD_MS "3000" CACHE STRING "How long final success/error status LED indications stay visible, in milliseconds")
set(FUSEE_STATUS_LED_RGB_ORDER "" CACHE STRING "Override colored status LED byte order; true for RGB, false for Pico SDK default GRB")

if (FUSEE_DEBUG_UART)
    target_compile_definitions(fusee PRIVATE FUSEE_DEBUG_UART=1)
    pico_enable_stdio_uart(fusee 1)
else()
    target_compile_definitions(fusee PRIVATE FUSEE_DEBUG_UART=0)
    pico_enable_stdio_uart(fusee 0)
endif()

pico_enable_stdio_usb(fusee 0)

target_compile_definitions(fusee PRIVATE
    FUSEE_STATUS_LED_HOLD_MS=${FUSEE_STATUS_LED_HOLD_MS}
)

if (NOT "${FUSEE_STATUS_LED_RGB_ORDER}" STREQUAL "")
    if (FUSEE_STATUS_LED_RGB_ORDER)
        set(FUSEE_STATUS_LED_RGB_ORDER_VALUE 1)
    else()
        set(FUSEE_STATUS_LED_RGB_ORDER_VALUE 0)
    endif()
    target_compile_definitions(fusee PRIVATE
        FUSEE_STATUS_LED_RGB_ORDER=${FUSEE_STATUS_LED_RGB_ORDER_VALUE}
    )
endif()

if (NOT "${FUSEE_STATUS_LED_PIN}" STREQUAL "")
    target_compile_definitions(fusee PRIVATE
        PICO_DEFAULT_LED_PIN=${FUSEE_STATUS_LED_PIN}
    )
endif()

if (NOT "${FUSEE_STATUS_LED_INVERTED}" STREQUAL "")
    if (FUSEE_STATUS_LED_INVERTED)
        set(FUSEE_STATUS_LED_INVERTED_VALUE 1)
    else()
        set(FUSEE_STATUS_LED_INVERTED_VALUE 0)
    endif()
    target_compile_definitions(fusee PRIVATE
        PICO_DEFAULT_LED_PIN_INVERTED=${FUSEE_STATUS_LED_INVERTED_VALUE}
    )
endif()

if (NOT "${FUSEE_STATUS_LED_ENABLED}" STREQUAL "")
    if (FUSEE_STATUS_LED_ENABLED)
        target_compile_definitions(fusee PRIVATE
            PICO_STATUS_LED_AVAILABLE=1
        )
    else()
        target_compile_definitions(fusee PRIVATE
            PICO_STATUS_LED_AVAILABLE=0
            PICO_COLORED_STATUS_LED_AVAILABLE=0
        )
    endif()
endif()

target_link_libraries(fusee pico_stdlib pico_status_led tinyusb_host tinyusb_board)

target_include_directories(fusee PRIVATE . "${FUSEE_GENERATED_DIR}")

pico_add_extra_outputs(fusee)
