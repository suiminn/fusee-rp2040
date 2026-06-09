add_executable(fusee fusee.c)

set(FUSEE_STATUS_LED_PIN "" CACHE STRING "Override the board status LED GPIO pin; leave empty to use PICO_BOARD defaults")
set(FUSEE_STATUS_LED_INVERTED "" CACHE STRING "Override status LED polarity; true for active-low, false for active-high")
set(FUSEE_STATUS_LED_ENABLED "" CACHE STRING "Override status LED availability; false disables regular and colored status LEDs")

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

target_include_directories(fusee PRIVATE .)

pico_add_extra_outputs(fusee)
