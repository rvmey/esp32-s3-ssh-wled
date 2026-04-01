# Hardware-variant source files: only one output driver is compiled per build.
if(CONFIG_HARDWARE_JC3248W535)
    set(VARIANT_SRCS "screen_control.c")
else()
    set(VARIANT_SRCS "led_control.c")
endif()

idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "improv_wifi.c"
        "ssh_server.c"
        "http_config.c"
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
