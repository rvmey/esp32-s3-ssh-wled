#pragma once

#include "esp_err.h"

/**
 * @brief Start the SSH server task.
 *        Listens on CONFIG_SSH_PORT, authenticates via CONFIG_SSH_USERNAME /
 *        CONFIG_SSH_PASSWORD, and exposes a simple shell to control the
 *        onboard RGB LED.
 *
 * @return ESP_OK if the server task was created successfully.
 */
esp_err_t ssh_server_start(void);

/**
 * @brief Execute the saved initialization script (if any) against the
 *        hardware directly, without an SSH session.  Call this once at
 *        boot after hw_init() and nvs_flash_init() have completed.
 *        Commands are read from NVS (namespace "ssh_cfg", key "initscript")
 *        and executed silently — one per line.
 */
void ssh_run_init_script(void);
