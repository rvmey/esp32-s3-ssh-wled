# Hardware-variant source files: only one output driver is compiled per build.
if(CONFIG_HARDWARE_JC3248W535)
    set(VARIANT_SRCS "screen_control.c")
elseif(CONFIG_HARDWARE_PICTURE_FRAME)
    set(VARIANT_SRCS
        "screen_control.c"
        "picture_frame.c"
        "qrcodegen.c"
        "socketio.c"
        "http_pf_config.c")
elseif(CONFIG_HARDWARE_BIKE_TRACKER)
    set(VARIANT_SRCS
        "bike_tracker.c"
        "mpu6050.c"
        "gps_ubx.c"
        "ride_log.c"
        "ride_upload.c")
elseif(CONFIG_HARDWARE_TCMD_ATOM_ECHO)
    set(VARIANT_SRCS
        "tcmd_atom_echo.c"
        "atom_led.c"
        "atom_audio.c"
        "atom_mic.c"
        "audio_clips.c")
elseif(CONFIG_HARDWARE_ATOMS3_LITE)
    set(VARIANT_SRCS
        "tcmd_atoms3_lite.c"
        "atoms3_led.c"
        "socketio.c")
elseif(CONFIG_HARDWARE_CORE2 OR CONFIG_HARDWARE_CYD)
    set(VARIANT_SRCS
        "screen_control_core2.c"
        "core2_audio.c"
        "core2_mic.c"
        "core2_adc_mic.c"
        "core2_leds.c"
        "mpu6886.c"
        "picture_frame.c"
        "qrcodegen.c"
        "socketio.c"
        "sip_client.c"
        "sip_rtp.c"
        "http_pf_config.c")
    if(CONFIG_HARDWARE_CYD)
        list(REMOVE_ITEM VARIANT_SRCS "screen_control_core2.c")
        list(APPEND VARIANT_SRCS "screen_control_cyd.c")
    endif()
elseif(CONFIG_HARDWARE_ESP32S3_CAM)
    set(VARIANT_SRCS "camera_server.c")
else()
    set(VARIANT_SRCS "led_control.c")
endif()

# SSH/HTTP server sources are not needed for bike-tracker, picture-frame, atom-echo, or atoms3-lite
if(NOT CONFIG_HARDWARE_BIKE_TRACKER AND NOT CONFIG_HARDWARE_PICTURE_FRAME
    AND NOT CONFIG_HARDWARE_TCMD_ATOM_ECHO AND NOT CONFIG_HARDWARE_CORE2
    AND NOT CONFIG_HARDWARE_CYD
   AND NOT CONFIG_HARDWARE_ATOMS3_LITE AND NOT CONFIG_HARDWARE_ESP32S3_CAM)
    set(SSH_SRCS "ssh_server.c" "http_config.c")
else()
    set(SSH_SRCS "")
endif()

# improv_wifi uses driver/usb_serial_jtag.h which is ESP32-S3 only;
# ATOM Echo, Core2, and AtomS3 Lite handle their own provisioning via SoftAP.
if(CONFIG_HARDWARE_TCMD_ATOM_ECHO OR CONFIG_HARDWARE_CORE2 OR CONFIG_HARDWARE_CYD OR CONFIG_HARDWARE_ATOMS3_LITE)
    set(IMPROV_SRCS "")
else()
    set(IMPROV_SRCS "improv_wifi.c")
endif()

idf_component_register(
    SRCS
        "main.c"
        "wifi_manager.c"
        ${IMPROV_SRCS}
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

# CYD intentionally reuses the Core2 runtime path for command parity while
# keeping a distinct Kconfig variant symbol and build profile.
if(CONFIG_HARDWARE_CYD)
    target_compile_definitions(__idf_main PRIVATE
        CONFIG_HARDWARE_CORE2=1
    )
endif()
