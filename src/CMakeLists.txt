find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(FUSEE_PAYLOAD_BIN "${CMAKE_CURRENT_LIST_DIR}/bin/hekate_ctcaer_6.5.2.bin" CACHE FILEPATH "Payload binary to embed in the generated RCM image")
set(FUSEE_INTERMEZZO_BIN "${CMAKE_CURRENT_LIST_DIR}/bin/intermezzo.bin" CACHE FILEPATH "Intermezzo relocator binary to embed in the generated RCM image")

get_filename_component(FUSEE_PAYLOAD_BIN_ABS "${FUSEE_PAYLOAD_BIN}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
get_filename_component(FUSEE_INTERMEZZO_BIN_ABS "${FUSEE_INTERMEZZO_BIN}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")

if (NOT EXISTS "${FUSEE_PAYLOAD_BIN_ABS}")
    message(FATAL_ERROR "FUSEE_PAYLOAD_BIN does not exist: ${FUSEE_PAYLOAD_BIN_ABS}")
endif()

if (NOT EXISTS "${FUSEE_INTERMEZZO_BIN_ABS}")
    message(FATAL_ERROR "FUSEE_INTERMEZZO_BIN does not exist: ${FUSEE_INTERMEZZO_BIN_ABS}")
endif()

set(FUSEE_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(FUSEE_RCM_IMAGE_INCLUDE "${FUSEE_GENERATED_DIR}/rcm_image.h")

add_custom_command(
    OUTPUT "${FUSEE_RCM_IMAGE_INCLUDE}"
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/build_rcm_image.py"
            --payload "${FUSEE_PAYLOAD_BIN_ABS}"
            --intermezzo "${FUSEE_INTERMEZZO_BIN_ABS}"
            --output "${FUSEE_RCM_IMAGE_INCLUDE}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/build_rcm_image.py"
        "${FUSEE_PAYLOAD_BIN_ABS}"
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
