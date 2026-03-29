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
