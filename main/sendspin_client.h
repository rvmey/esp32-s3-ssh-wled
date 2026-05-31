#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* Start the Sendspin WebSocket server so the device appears as a receiver. */
void sendspin_client_start(void);

/* Disconnect and tear down the Sendspin client. */
void sendspin_client_stop(void);

/* Return true if the server is currently running. */
bool sendspin_client_is_running(void);

/* Drive the Sendspin event loop. Call once per main-loop iteration. */
void sendspin_client_loop(void);

#ifdef __cplusplus
}
#endif
