cmake_minimum_required(VERSION 3.16)

# Use our own APP_VERSION (from main/main.c) as the project version, so the
# "App version:" line in the boot log shows e.g. "2.0.396" instead of a
# `git describe` hash.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/main/main.c" _app_version_line REGEX "#define APP_VERSION \"([^\"]+)\"")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" PROJECT_VER "${_app_version_line}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_ssh_led)

# WOLFSSL_ED25519_STREAMING_VERIFY is required for wolfSSH server-side ed25519
# user authentication. Without it wolfssh/wolfssh/internal.h forces WOLFSSH_NO_ED25519.
add_compile_definitions(WOLFSSL_ED25519_STREAMING_VERIFY)
