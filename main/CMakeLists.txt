idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        "improv_wifi.c"
        "led_control.c"
        "ssh_server.c"
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
