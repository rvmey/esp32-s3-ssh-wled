cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32_ssh_led)

# WOLFSSL_ED25519_STREAMING_VERIFY is required for wolfSSH server-side ed25519
# user authentication. Without it wolfssh/wolfssh/internal.h forces WOLFSSH_NO_ED25519.
add_compile_definitions(WOLFSSL_ED25519_STREAMING_VERIFY)
