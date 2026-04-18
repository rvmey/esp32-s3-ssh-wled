/*
 * Improv WiFi Serial v1 provisioning
 * https://www.improv-wifi.com/serial/
 *
 * Packet structure (all packets):
 *   IMPROV (6 B) | version (1 B) | type (1 B) | data_len (1 B)
 *   | data (data_len B) | checksum (1 B)
 *
 * Checksum = sum of every preceding byte, mod 256.
 */

#include "improv_wifi.h"
#include "wifi_manager.h"

#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "improv";

/* ── Protocol constants ──────────────────────────────────────────────────── */

static const uint8_t IMPROV_HEADER[6] = {'I','M','P','R','O','V'};
#define IMPROV_VERSION  1

#define TYPE_CURRENT_STATE  0x01
#define TYPE_ERROR_STATE    0x02
#define TYPE_RPC_CMD        0x03
#define TYPE_RPC_RESULT     0x04

#define STATE_AUTHORIZED    0x02
#define STATE_PROVISIONING  0x03
#define STATE_PROVISIONED   0x04

#define ERR_NONE              0x00
#define ERR_INVALID_RPC       0x01
#define ERR_UNKNOWN_CMD       0x02
#define ERR_UNABLE_TO_CONNECT 0x03

#define CMD_SEND_WIFI_SETTINGS    0x01
#define CMD_REQUEST_CURRENT_STATE  0x02
#define CMD_REQUEST_DEVICE_INFO    0x03

/* ── USB-Serial-JTAG I/O ─────────────────────────────────────────────────── */

#define IMPROV_RX_BUF    512
#define IMPROV_IO_TIMEOUT_MS  200

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static uint8_t pkt_checksum(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return sum;
}

static void send_packet(uint8_t type, const uint8_t *data, uint8_t data_len)
{
    /* Max packet: 6 + 1 + 1 + 1 + 255 + 1 = 265 bytes */
    uint8_t pkt[265];
    size_t  idx = 0;

    memcpy(pkt, IMPROV_HEADER, 6);
    idx += 6;
    pkt[idx++] = IMPROV_VERSION;
    pkt[idx++] = type;
    pkt[idx++] = data_len;
    if (data_len > 0 && data) {
        memcpy(pkt + idx, data, data_len);
        idx += data_len;
    }
    pkt[idx] = pkt_checksum(pkt, idx);
    idx++;

    usb_serial_jtag_write_bytes(pkt, idx, pdMS_TO_TICKS(IMPROV_IO_TIMEOUT_MS));
}

static void send_state(uint8_t state)
{
    send_packet(TYPE_CURRENT_STATE, &state, 1);
}

static void send_error(uint8_t err)
{
    send_packet(TYPE_ERROR_STATE, &err, 1);
}

/* RPC result for CMD_SEND_WIFI_SETTINGS; url may be NULL on failure */
static void send_rpc_result(uint8_t cmd, const char *url)
{
    uint8_t data[256];
    size_t  idx = 0;

    data[idx++] = cmd;  /* command echo */

    if (url && url[0] != '\0') {
        uint8_t url_len  = (uint8_t)strlen(url);
        uint8_t body_len = 1 + url_len; /* string_len byte + string bytes */
        data[idx++] = body_len;
        data[idx++] = url_len;
        memcpy(data + idx, url, url_len);
        idx += url_len;
    } else {
        data[idx++] = 0; /* empty body */
    }

    send_packet(TYPE_RPC_RESULT, data, (uint8_t)idx);
}

/* RPC result for CMD_REQUEST_DEVICE_INFO – four required strings */
static void send_device_info(void)
{
    static const char * const strings[4] = {
        "ESP32-SSH-LED",        /* firmware name    */
        "2.0.53",              /* firmware version */
        "ESP32-S3",            /* chip/variant     */
        "ESP32-S3 SSH Server", /* device name      */
    };

    uint8_t data[256];
    size_t  idx = 0;

    data[idx++] = CMD_REQUEST_DEVICE_INFO; /* command echo */
    size_t body_len_pos = idx++;           /* placeholder */
    size_t body_start   = idx;

    for (int i = 0; i < 4; i++) {
        uint8_t slen = (uint8_t)strlen(strings[i]);
        data[idx++] = slen;
        memcpy(data + idx, strings[i], slen);
        idx += slen;
    }
    data[body_len_pos] = (uint8_t)(idx - body_start);

    send_packet(TYPE_RPC_RESULT, data, (uint8_t)idx);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t improv_wifi_start(void)
{
    /* The USB-Serial-JTAG driver is installed by the IDF console init when
     * CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y.  If for some reason it is not yet
     * installed (e.g. a non-standard build), install it now. */
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t install_ret = usb_serial_jtag_driver_install(&usj_cfg);
    if (install_ret != ESP_OK && install_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s",
                 esp_err_to_name(install_ret));
        return install_ret;
    }

    ESP_LOGI(TAG, "Entering Improv WiFi provisioning mode – "
                  "open the installer page and enter your WiFi credentials.");

    uint8_t buf[IMPROV_RX_BUF];
    size_t  buf_pos = 0;

    while (1) {
        /* Beacon: announce we are waiting for credentials */
        send_state(STATE_AUTHORIZED);

        /* Wait up to 100 ms for incoming bytes (shorter = faster beacon rate) */
        int n = usb_serial_jtag_read_bytes(buf + buf_pos,
                                           sizeof(buf) - buf_pos - 1,
                                           pdMS_TO_TICKS(100));
        if (n <= 0) continue;
        buf_pos += (size_t)n;

        /* ── Scan for IMPROV header ─────────────────────────────────────── */
        size_t hdr_pos = buf_pos; /* sentinel: no header found */
        for (size_t i = 0; i + 6 <= buf_pos; i++) {
            if (memcmp(buf + i, IMPROV_HEADER, 6) == 0) {
                hdr_pos = i;
                break;
            }
        }

        if (hdr_pos == buf_pos) {
            /* No header yet; discard everything except a possible partial
             * match at the very end (up to 5 bytes). */
            if (buf_pos > 5) {
                size_t keep = 5;
                memmove(buf, buf + buf_pos - keep, keep);
                buf_pos = keep;
            }
            continue;
        }

        /* Shift buffer so it starts at the header */
        if (hdr_pos > 0) {
            memmove(buf, buf + hdr_pos, buf_pos - hdr_pos);
            buf_pos -= hdr_pos;
        }

        /* Need at least: 6(hdr)+1(ver)+1(type)+1(dlen)+1(cksum) = 10 B */
        if (buf_pos < 10) continue;

        uint8_t type     = buf[7];
        uint8_t data_len = buf[8];
        size_t  pkt_len  = 6 + 1 + 1 + 1 + data_len + 1;

        if (buf_pos < pkt_len) continue; /* wait for the rest */

        /* Verify checksum */
        uint8_t expected = pkt_checksum(buf, pkt_len - 1);
        if (buf[pkt_len - 1] != expected) {
            ESP_LOGW(TAG, "Bad checksum (got 0x%02x, expected 0x%02x)",
                     buf[pkt_len - 1], expected);
            send_error(ERR_INVALID_RPC);
            goto consume;
        }

        /* ── Handle packet ──────────────────────────────────────────────── */
        const uint8_t *data = buf + 9; /* points at data field */

        if (type == TYPE_RPC_CMD) {
            if (data_len < 2) {
                send_error(ERR_INVALID_RPC);
                goto consume;
            }

            uint8_t        cmd          = data[0];
            uint8_t        rpc_data_len = data[1];
            const uint8_t *rpc          = data + 2;

            if (cmd == CMD_REQUEST_CURRENT_STATE) {
                /* Immediately echo our state so the browser doesn't time out */
                send_state(STATE_AUTHORIZED);

            } else if (cmd == CMD_REQUEST_DEVICE_INFO) {
                send_device_info();

            } else if (cmd == CMD_SEND_WIFI_SETTINGS) {
                /* rpc: ssid_len(1) | ssid | pass_len(1) | pass */
                if (rpc_data_len < 2) {
                    send_error(ERR_INVALID_RPC);
                    goto consume;
                }

                uint8_t ssid_len = rpc[0];
                if (rpc_data_len < (size_t)(1 + ssid_len + 1)) {
                    send_error(ERR_INVALID_RPC);
                    goto consume;
                }
                uint8_t pass_len = rpc[1 + ssid_len];
                if (rpc_data_len < (size_t)(1 + ssid_len + 1 + pass_len)) {
                    send_error(ERR_INVALID_RPC);
                    goto consume;
                }

                char ssid[33]     = {0};
                char password[65] = {0};
                size_t copy_ssid  = ssid_len  < 32 ? ssid_len  : 32;
                size_t copy_pass  = pass_len  < 64 ? pass_len  : 64;
                memcpy(ssid,     rpc + 1,                 copy_ssid);
                memcpy(password, rpc + 2 + ssid_len, copy_pass);

                ESP_LOGI(TAG, "Received WiFi settings for SSID: %s", ssid);
                send_state(STATE_PROVISIONING);

                esp_err_t ret = wifi_connect_with_credentials(ssid, password);
                if (ret == ESP_OK) {
                    wifi_save_credentials(ssid, password);

                    char ip[24]  = {0};
                    char url[48] = {0};
                    if (wifi_get_ip(ip, sizeof(ip)) == ESP_OK) {
                        snprintf(url, sizeof(url), "http://%s", ip);
                    }

                    send_state(STATE_PROVISIONED);
                    send_rpc_result(CMD_SEND_WIFI_SETTINGS, url);

                    ESP_LOGI(TAG, "Provisioning complete – IP: %s", ip);
                    return ESP_OK;
                } else {
                    ESP_LOGW(TAG, "WiFi connection failed");
                    send_error(ERR_UNABLE_TO_CONNECT);
                    send_state(STATE_AUTHORIZED);
                }

            } else {
                send_error(ERR_UNKNOWN_CMD);
            }
        }
        /* silently ignore non-RPC packets (e.g. host querying state) */

consume:
        memmove(buf, buf + pkt_len, buf_pos - pkt_len);
        buf_pos -= pkt_len;
    }

    /* unreachable */
    return ESP_FAIL;
}
