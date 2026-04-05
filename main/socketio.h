/*
 * socketio.h
 *
 * Minimal Engine.IO v4 + Socket.IO v4 client over esp_websocket_client.
 *
 * Supported inbound packets:
 *   0{…}     EIO open      → extract sid; send "40" (SIO connect)
 *   2        EIO ping      → reply "3" (EIO pong)
 *   40{…}    SIO connect ack → set connected; unblock socketio_connect()
 *   42[…]    SIO event     → invoke user callback
 *   41       SIO disconnect → clear connected flag
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Callback invoked on each incoming Socket.IO event.
 *
 * @param event_name   NUL-terminated event name string.
 * @param payload_json NUL-terminated JSON string for the event payload.
 * @param user_ctx     Opaque pointer supplied to socketio_connect().
 */
typedef void (*socketio_event_cb_t)(const char *event_name,
                                    const char *payload_json,
                                    void       *user_ctx);

/**
 * @brief Connect to a Socket.IO v4 server.
 *
 * Blocks until the SIO connect acknowledgement ("40") is received or 10 s
 * elapses.  The JWT is sent as an HTTP Upgrade header:
 *   Authorization: Bearer <auth_token>
 *
 * TLS is handled by the full ESP-IDF CA bundle (covers GoDaddy G2 chain).
 *
 * @param uri        WebSocket URI, e.g.
 *                   "wss://www.triggercmd.com/socket.io/?EIO=4&transport=websocket"
 * @param auth_token Hardware JWT string (NUL-terminated, ≤ 511 chars).
 * @param cb         Event callback; may be NULL if events are not needed.
 * @param user_ctx   Opaque context forwarded to cb.
 *
 * @return ESP_OK on successful connect, ESP_ERR_TIMEOUT if the SIO ack was
 *         not received within 10 s, or another esp_err_t on failure.
 */
esp_err_t socketio_connect(const char          *uri,
                           const char          *auth_token,
                           socketio_event_cb_t  cb,
                           void                *user_ctx);

/**
 * @brief Disconnect and release all Socket.IO resources.
 */
void socketio_disconnect(void);

/**
 * @brief Return true if an active Socket.IO session exists.
 */
bool socketio_connected(void);
