# Hardware-variant source files: only one output driver is compiled per build.
if(CONFIG_HARDWARE_JC3248W535)
    set(VARIANT_SRCS "screen_control.c")
elseif(CONFIG_HARDWARE_PICTURE_FRAME)
    set(VARIANT_SRCS
        "screen_control.c"
        "picture_frame.c"
        "socketio.c"
        "http_pf_config.c")
elseif(CONFIG_HARDWARE_BIKE_TRACKER)
    set(VARIANT_SRCS
        "bike_tracker.c"
        "mpu6050.c"
        "gps_ubx.c"
        "ride_log.c"
        "ride_upload.c")
else()
    set(VARIANT_SRCS "led_control.c")
endif()

# SSH/HTTP server sources are not needed for bike-tracker or picture-frame
if(NOT CONFIG_HARDWARE_BIKE_TRACKER AND NOT CONFIG_HARDWARE_PICTURE_FRAME)
    set(SSH_SRCS "ssh_server.c" "http_config.c")
else()
    set(SSH_SRCS "")
endif()

idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "improv_wifi.c"
        ${SSH_SRCS}
        ${VARIANT_SRCS}
    INCLUDE_DIRS
        "."
)

# wolfSSL headers require WOLFSSL_USER_SETTINGS to read user_settings.h.
# wolfssl's own CMakeLists.txt sets this via CMAKE_C_FLAGS but that does not
# reliably propagate to other components in the IDF build system, so we add it
# explicitly here so wolfSSL headers read user_settings.h instead of options.h.
target_compile_definitions(__idf_main PRIVATE
    WOLFSSL_USER_SETTINGS
)
