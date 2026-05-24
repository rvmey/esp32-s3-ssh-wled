/*
 * picture_frame.c
 *
 * TriggerCMD Picture Frame firmware loop for the Guition JC3248W535.
 *
 * Boot sequence:
 *   1. screen_init()
 *   2. WiFi — Improv-WiFi BLE provisioning if no stored credentials.
 *   3. User JWT — obtained via pair code flow:
 *        GET /pair?model=TCMDSCREEN → {pairCode, pairToken}
 *        Display code; poll GET /pair/lookup every 5 s (up to 10 min).
 *        On authorisation, token is saved to NVS and device reboots.
 *        On timeout, a fresh pair code is fetched automatically.
 *   4. Provisioning — POST /api/computer/save with name TCMDSCREEN-<MAC>,
 *      receiving back a computer ID stored in NVS.
 *   5. Command sync — GET /api/command/list, then POST /api/command/save
 *      for any commands from picture_frame_commands.json not yet online.
 *   6. Socket.IO connect; subscribe via Sails.io virtual GET
 *        /api/computer/subscribeToFunRoom?roomName=<computer_id>
 *   7. Event loop — dispatches "message" events (text/color/textcolor/
 *      fontsize/landscape/portrait/jpeg) to screen functions; reports back
 *      via POST /api/run/save.
 */

#include "picture_frame.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"

#include "wifi_manager.h"
#include "improv_wifi.h"
#include "screen_control.h"

#if CONFIG_BT_ENABLED
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#if CONFIG_BT_A2DP_ENABLE
#include "esp_a2dp_api.h"
#endif
#endif

#if CONFIG_ESP_COEX_ENABLED
#include "esp_coexist.h"
#endif

#if CONFIG_HARDWARE_CORE2
/* SoftAP provisioning for Core2 (classic ESP32 — no USB-JTAG Improv) */
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"

static const char s_c2_prov_html[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>TRIGGERcmd Core2 Setup</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#0f1117;color:#e2e8f0;"
    "display:flex;align-items:center;justify-content:center;min-height:100vh}"
    ".card{background:#1e2130;border:1px solid #2d3148;border-radius:1rem;"
    "padding:2rem;max-width:380px;width:100%}"
    "h1{color:#a5b4fc;font-size:1.3rem;margin:0 0 1rem}"
    "label{display:block;color:#94a3b8;font-size:.85rem;margin:.75rem 0 .25rem}"
    "input{width:100%;box-sizing:border-box;padding:.6rem;background:#0f1117;"
    "border:1px solid #4f46e5;border-radius:.4rem;color:#e2e8f0;font-size:1rem}"
    "button{margin-top:1.25rem;width:100%;padding:.75rem;background:#4f46e5;"
    "color:#fff;border:none;border-radius:.6rem;font-size:1rem;cursor:pointer}"
    "</style></head><body><div class='card'>"
    "<h1>TRIGGERcmd Core2 Setup</h1>"
    "<form method='POST' action='/save'>"
    "<label>Wi-Fi SSID</label><input name='ssid' required>"
    "<label>Wi-Fi Password</label><input name='pass' type='password'>"
    "<button type='submit'>Save &amp; Connect</button>"
    "</form></div></body></html>";

static const char s_c2_saved_html[] =
    "<!DOCTYPE html><html><body style='font-family:system-ui;background:#0f1117;"
    "color:#86efac;display:flex;align-items:center;justify-content:center;"
    "min-height:100vh'><h2>Saved! Device is restarting...</h2></body></html>";

static bool s_c2_prov_done = false;

static void c2_url_decode(const char *src, char *dst, size_t dst_sz)
{
    size_t i = 0;
    while (*src && i < dst_sz - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], '\0'};
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static bool c2_form_get_field(const char *body, const char *field,
                               char *out, size_t out_sz)
{
    size_t flen = strlen(field);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, field, flen) == 0 && p[flen] == '=') {
            p += flen + 1;
            const char *end = strchr(p, '&');
            char encoded[256] = {0};
            size_t vlen = end ? (size_t)(end - p) : strlen(p);
            if (vlen >= sizeof(encoded)) vlen = sizeof(encoded) - 1;
            memcpy(encoded, p, vlen);
            c2_url_decode(encoded, out, out_sz);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

static esp_err_t c2_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_c2_prov_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t c2_save_handler(httpd_req_t *req)
{
    char body[1024] = {0};
    int  len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[len] = '\0';

    char ssid[64] = {0};
    char pass[64] = {0};
    c2_form_get_field(body, "ssid", ssid, sizeof(ssid));
    c2_form_get_field(body, "pass", pass, sizeof(pass));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_c2_saved_html, HTTPD_RESP_USE_STRLEN);

    if (ssid[0]) {
        wifi_save_credentials(ssid, pass);
        ESP_LOGI("pf_c2", "WiFi credentials saved for SSID: %s", ssid);
    }
    s_c2_prov_done = true;
    return ESP_OK;
}

/* Starts SoftAP + HTTP server; blocks until the form is submitted, then restarts */
static void pf_softap_provision(void) __attribute__((unused));
static void pf_softap_provision(void)
{
    wifi_stack_init_public();

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "TCMD-Core2-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    ESP_LOGI("pf_c2", "Starting SoftAP: %s", ap_ssid);

    screen_set_color(0, 0, 64);
    char msg[128];
    snprintf(msg, sizeof(msg),
             "WiFi Setup\nConnect to:\n%s\nThen browse to\n192.168.4.1", ap_ssid);
    screen_draw_text(msg);

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    (void)ap_netif;

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI("pf_c2", "Provisioning: connect a computer or phone to the '%s' wifi network, then open http://192.168.4.1 and enter your WiFi SSID/password.", ap_ssid);

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_open_sockets = 4;
    http_cfg.lru_purge_enable = true;
    httpd_handle_t server    = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));

    httpd_uri_t get_uri  = { .uri = "/",     .method = HTTP_GET,  .handler = c2_get_handler  };
    httpd_uri_t save_uri = { .uri = "/save", .method = HTTP_POST, .handler = c2_save_handler };
    httpd_register_uri_handler(server, &get_uri);
    httpd_register_uri_handler(server, &save_uri);

    s_c2_prov_done = false;
    while (!s_c2_prov_done) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    httpd_stop(server);
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}
#endif /* CONFIG_HARDWARE_CORE2 */
#include "socketio.h"
#include "http_pf_config.h"
#include "triggercmd_ca.h"   /* embedded Go Daddy Root G2 cert for triggercmd.com */
#include "jpeg_decoder.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "mp3dec.h"

#if CONFIG_HARDWARE_CORE2
#include "core2_audio.h"
#endif


static const char *TAG = "pf";
extern const char g_firmware_version[];

#define MP3_ROOT_PATH        "/sdcard"
#define MP3_MAX_FOLDERS      32
#define MP3_MAX_TRIGGER_LEN  64
#define MP3_MAX_PATH_LEN     256
#define MP3_MAX_FILE_LEN     128
#define MP3_SEEK_STEP_MS     10000U

typedef struct {
    char trigger[MP3_MAX_TRIGGER_LEN];
    char folder_path[MP3_MAX_PATH_LEN];
    int  mp3_count;
} mp3_folder_t;

typedef struct {
    bool       active;
    bool       paused;
    bool       shuffle;
    bool       repeat_track;
    bool       repeat_playlist;
    int        volume;            /* 0..100 */
    int        folder_idx;        /* index into s_mp3_folders */
    int        track_idx;         /* 0-based index in folder */
    uint32_t   duration_ms;
    uint32_t   position_ms;
    TickType_t last_tick;
    uint32_t   play_token;
    char       folder_name[MP3_MAX_TRIGGER_LEN];
    char       file_name[MP3_MAX_FILE_LEN];
    char       file_path[MP3_MAX_PATH_LEN + MP3_MAX_FILE_LEN + 8];
} mp3_state_t;

/* ── Server URL base (change to dev server as needed) ──────────────────── */
/*
 * Examples:
 *   "https://www.triggercmd.com"   — production
 *   "https://xxxx.ngrok-free.app"  — ngrok HTTPS tunnel
 *   "http://192.168.1.50:3000"     — local dev server (no TLS)
 */
static const char *TCMD_BASE_URL = "https://www.triggercmd.com";

/* Strip scheme prefix for use in display text */
static const char *tcmd_display_host(void) __attribute__((unused));
static const char *tcmd_display_host(void)
{
    if (strncmp(TCMD_BASE_URL, "https://", 8) == 0) return TCMD_BASE_URL + 8;
    if (strncmp(TCMD_BASE_URL, "http://",  7) == 0) return TCMD_BASE_URL + 7;
    return TCMD_BASE_URL;
}

/* ── NVS helpers ────────────────────────────────────────────────────────── */

#define NVS_NS          "pf_cfg"
#define NVS_KEY_TOKEN   "hw_token"
#define NVS_KEY_COMPID  "computer_id"
#define NVS_KEY_SAVED   "disp_saved"
#define NVS_KEY_TEXT    "disp_text"
#define NVS_KEY_BG_R    "bg_r"
#define NVS_KEY_BG_G    "bg_g"
#define NVS_KEY_BG_B    "bg_b"
#define NVS_KEY_FG_R    "fg_r"
#define NVS_KEY_FG_G    "fg_g"
#define NVS_KEY_FG_B    "fg_b"
#define NVS_KEY_FONT    "font"
#define NVS_KEY_ORIENT  "land"
#define NVS_KEY_JPEGURL "jpeg_url"
#define NVS_KEY_SHUFFLE "mp3_shuffle"
#define NVS_KEY_REPEAT_TRACK "mp3_repeat_track"
#define NVS_KEY_REPEAT_PLAYLIST "mp3_repeat_playlist"
#define NVS_KEY_BT_BDA  "bt_bda"
#define NVS_KEY_BT_NAME "bt_name"

#define HW_TOKEN_MAX_LEN    513   /* 512 payload + NUL */
#define COMPUTER_ID_MAX_LEN  33   /* 32 payload + NUL  */
#define COMPUTER_NAME_LEN    32   /* "TCMDSCREEN-AABBCCDDEEFF" + NUL */

/* ── Module-level statics shared with event handler ────────────────────── */
static char s_hw_token[HW_TOKEN_MAX_LEN]      __attribute__((unused)) = {0};
static char s_computer_id[COMPUTER_ID_MAX_LEN] __attribute__((unused)) = {0};

/* Pending run/save — set by the WS event task, consumed by the main loop */
static char          s_pending_run_id[33] __attribute__((unused)) = {0};
static volatile bool s_pending_run        = false;
static int           s_pending_run_tries  __attribute__((unused)) = 0;
static TickType_t    s_pending_run_retry_after __attribute__((unused)) = 0;

/* Pending JPEG URL — set by the WS event task, consumed by the main loop */
static char          s_pending_jpeg_url[512] __attribute__((unused)) = {0};
static volatile bool s_pending_jpeg          = false;

/* Cached compressed JPEG — kept in PSRAM so orientation changes can redraw
 * without re-downloading.  Freed when a non-jpeg command replaces the display. */
static uint8_t      *s_jpeg_cache     __attribute__((unused)) = NULL;
static int           s_jpeg_cache_len __attribute__((unused)) = 0;
static volatile bool s_pending_jpeg_redraw = false;

/* Persistable display state (set by commands, committed by 'save'). */
static char s_last_text[512]         __attribute__((unused)) = {0};
static char s_current_jpeg_url[512]  __attribute__((unused)) = {0};

static sdmmc_card_t *s_sd_card __attribute__((unused)) = NULL;
static bool          s_sd_mounted __attribute__((unused)) = false;
static mp3_folder_t  s_mp3_folders[MP3_MAX_FOLDERS] __attribute__((unused));
static size_t        s_mp3_folder_count __attribute__((unused)) = 0;
static mp3_state_t   s_mp3 = {
    .active = false,
    .paused = false,
    .shuffle = false,
    .repeat_track = false,
    .repeat_playlist = false,
    .volume = 50,
    .folder_idx = -1,
    .track_idx = -1,
    .duration_ms = 0,
    .position_ms = 0,
    .last_tick = 0,
};
static volatile bool s_mp3_resume_on_bt_reconnect = false;
static TickType_t s_mp3_last_ui_tick __attribute__((unused)) = 0;
static volatile bool s_mp3_ui_pending = false;
static TaskHandle_t s_mp3_task = NULL;
static TickType_t s_mp3_next_mount_retry __attribute__((unused)) = 0;
static bool s_sd_mount_warned __attribute__((unused)) = false;
static volatile int32_t s_mp3_seek_target_ms = -1;

static bool nvs_read_str(const char *key, char *out, size_t out_sz);
static esp_err_t nvs_write_str(const char *key, const char *val);
static esp_err_t nvs_erase_key_local(const char *key);
static inline void mp3_request_ui_refresh(void);
static bool mp3_advance_track(int step, const char *reason);
static bool mp3_handle_track_end(void);
static bool mp3_queue_seek_relative(int32_t delta_ms, const char *reason);
static void mp3_log_mode_status(const char *reason);
#if CONFIG_BT_ENABLED
typedef struct {
    bool initialized;
    volatile bool discovering;
#if CONFIG_BT_A2DP_ENABLE
    volatile bool connecting;
    volatile bool connected;
    volatile bool media_started;
    bool connect_after_discovery;
    int  connect_retries;     /* auto-retry on page timeout */
    bool pairing_ui_active;   /* true when pair command initiated current flow */
#endif
    bool has_device;
    bool has_bda;
    bool has_candidate;
    int candidate_score;
    esp_bd_addr_t bda;
    esp_bd_addr_t candidate_bda;
    char selected_name[64];
    char selected_bda[18];
    char candidate_name[64];
    char candidate_bda_str[18];
} bt_state_t;

static bt_state_t s_bt = {0};
static volatile bool s_bt_hold_local_speaker = false;

static bool      s_bt_reconnect_attempted  = false;
static volatile bool s_bt_pending_reconnect = false;
static TickType_t    s_bt_reconnect_after   = 0;
static TickType_t    s_bt_connect_started_at = 0;
static bool          s_bt_recent_acl_drop = false;

#define BT_CONNECT_RETRY_MAX          5
#define BT_CONNECT_TIMEOUT_MS          8000U
#define BT_RECONNECT_DELAY_HARD_DROP_MS 1500U

static const uint16_t s_bt_retry_delay_ms[BT_CONNECT_RETRY_MAX] = {
    900, 1800, 3000, 4500, 6000
};

#define BT_STARTUP_MIN_INTERNAL_FREE    (44 * 1024)
#define BT_STARTUP_MIN_INTERNAL_LARGEST (12 * 1024)

static bool bt_has_startup_headroom(const char *reason)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    if (free_internal < BT_STARTUP_MIN_INTERNAL_FREE ||
        largest_block < BT_STARTUP_MIN_INTERNAL_LARGEST) {
        ESP_LOGW(TAG,
                 "bt: skip %s, low internal heap free=%u largest=%u",
                 reason ? reason : "startup",
                 (unsigned int)free_internal,
                 (unsigned int)largest_block);
        return false;
    }
    return true;
}

static void bt_log_heap_snapshot(const char *reason, esp_err_t err)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    size_t largest_dma = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest_spiram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    ESP_LOGW(TAG,
             "bt: heap snapshot (%s err=%s) internal=%u/%u dma=%u/%u spiram=%u/%u",
             reason ? reason : "n/a",
             esp_err_to_name(err),
             (unsigned int)free_internal,
             (unsigned int)largest_internal,
             (unsigned int)free_dma,
             (unsigned int)largest_dma,
             (unsigned int)free_spiram,
             (unsigned int)largest_spiram);
}

static bool bt_parse_bda(const char *s, esp_bd_addr_t out)
{
    if (!s || !out) return false;
    unsigned int b[6] = {0};
    if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
    return true;
}

static bool bt_name_has_token(const char *name, const char *token)
{
    if (!name || !token || !name[0] || !token[0]) return false;
    size_t ln = strlen(name);
    size_t lt = strlen(token);
    if (lt > ln) return false;
    for (size_t i = 0; i + lt <= ln; i++) {
        bool match = true;
        for (size_t j = 0; j < lt; j++) {
            if (tolower((unsigned char)name[i + j]) != tolower((unsigned char)token[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

#if CONFIG_BT_A2DP_ENABLE
static void bt_persist_current_peer(void)
{
    if (s_bt.selected_bda[0]) {
        esp_err_t e = nvs_write_str(NVS_KEY_BT_BDA, s_bt.selected_bda);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "bt: persist bda failed: %s", esp_err_to_name(e));
        }
    }
    if (s_bt.selected_name[0]) {
        esp_err_t e = nvs_write_str(NVS_KEY_BT_NAME, s_bt.selected_name);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "bt: persist name failed: %s", esp_err_to_name(e));
        }
    }
}
#endif

static int bt_score_candidate(const char *name, int rssi, uint32_t cod)
{
    int score = rssi;
    if (!name || !name[0]) return score - 100;

    /*
     * Class of Device filter — most reliable signal.
     * CoD bit layout: [12:8]=major device class, [7:2]=minor device class.
     *
     * Major 4 = Audio/Video.  Audio-only minor classes:
     *   1=wearable headset, 2=hands-free, 5=loudspeaker, 6=headphones,
     *   7=portable audio, 8=car audio, 10=HiFi audio device.
     * Video/other minor classes (9=set-top box, 11-16=video):
     *   strongly penalise — these are TVs, cameras, etc.
     */
    if (cod != 0) {
        uint32_t major = (cod >> 8) & 0x1F;
        uint32_t minor = (cod >> 2) & 0x3F;
        if (major == 4) {
            if (minor == 1 || minor == 2 || minor == 5 ||
                minor == 6 || minor == 7 || minor == 8 || minor == 10) {
                score += 50;   /* confirmed audio sink */
            } else if (minor != 0) {
                score -= 200;  /* video device (TV, camera, set-top box…) */
            }
        } else if (major != 0) {
            score -= 80;       /* computer, phone, peripheral, etc. */
        }
    }

    if (bt_name_has_token(name, "head") || bt_name_has_token(name, "buds") ||
        bt_name_has_token(name, "speaker") || bt_name_has_token(name, "audio") ||
        bt_name_has_token(name, "sound") || bt_name_has_token(name, "ear") ||
        bt_name_has_token(name, "jbl") || bt_name_has_token(name, "sony") ||
        bt_name_has_token(name, "bose") || bt_name_has_token(name, "beats") ||
        bt_name_has_token(name, "anker") || bt_name_has_token(name, "senn") ||
        bt_name_has_token(name, "airpods") || bt_name_has_token(name, "marshall")) {
        score += 80;
    }
    if (bt_name_has_token(name, "phone") || bt_name_has_token(name, "laptop") ||
        bt_name_has_token(name, "pc") || bt_name_has_token(name, "keyboard") ||
        bt_name_has_token(name, "mouse") || bt_name_has_token(name, "tv") ||
        bt_name_has_token(name, "television") || bt_name_has_token(name, "webos")) {
        score -= 60;
    }
    return score;
}

#if CONFIG_BT_A2DP_ENABLE
static void bt_schedule_reconnect(const char *reason, uint32_t min_delay_ms)
{
    if (!s_bt.has_bda || s_bt.discovering || s_bt_pending_reconnect) return;
    if (s_bt.connect_retries >= BT_CONNECT_RETRY_MAX) return;

    s_bt.connect_retries++;
    uint32_t delay_ms = s_bt_retry_delay_ms[s_bt.connect_retries - 1];
    if (delay_ms < min_delay_ms) {
        delay_ms = min_delay_ms;
    }
    s_bt_reconnect_after = xTaskGetTickCount() + pdMS_TO_TICKS(delay_ms);
    s_bt_pending_reconnect = true;

    ESP_LOGW(TAG,
             "bt: scheduling reconnect attempt %d/%d in %lu ms (%s)",
             s_bt.connect_retries,
             BT_CONNECT_RETRY_MAX,
             (unsigned long)delay_ms,
             reason ? reason : "retry");

    if (s_bt.pairing_ui_active) {
        ESP_LOGI(TAG,
                 "bt: pairing retry status %d/%d",
                 s_bt.connect_retries,
                 BT_CONNECT_RETRY_MAX);
    }
}

static bool bt_start_connect_now(const char *reason)
{
    if (!s_bt.has_bda || s_bt.discovering || s_bt.connected || s_bt.connecting) {
        return false;
    }

    if (s_bt.pairing_ui_active) {
        ESP_LOGI(TAG, "bt: pairing connecting...");
    }

    ESP_LOGI(TAG,
             "bt: connecting to %s (%s)",
             s_bt.selected_bda,
             reason ? reason : "request");

    s_bt.connecting = true;
    s_bt_connect_started_at = xTaskGetTickCount();

    esp_err_t err = esp_a2d_source_connect(s_bt.bda);
    if (err != ESP_OK) {
        s_bt.connecting = false;
        ESP_LOGW(TAG,
                 "bt: connect call failed (%s): %s",
                 reason ? reason : "request",
                 esp_err_to_name(err));
        bt_schedule_reconnect("connect call failed", 0);
        return false;
    }
    return true;
}
#endif

#define BT_PCM_RING_BYTES (64 * 1024)
#define BT_PCM_LOW_WATER_BYTES  (10 * 1024)
#define BT_PCM_HIGH_WATER_BYTES (40 * 1024)
#define BT_PCM_TARGET_FILL_BYTES (20 * 1024)
#define BT_PCM_START_PRIME_BYTES (24 * 1024)
#define BT_PCM_START_PRIME_TIMEOUT_MS 1500
#define BT_PCM_RESUME_PRIME_BYTES (12 * 1024)
#define BT_PCM_RESUME_PRIME_TIMEOUT_MS 400
#define BT_A2DP_TARGET_SAMPLE_RATE 44100
#define BT_PCM_FRAME_BYTES 4
#define MP3_INPUT_BUF_BYTES (32 * 1024)
static uint8_t *s_bt_pcm_ring = NULL;
static size_t s_bt_pcm_rpos = 0;
static size_t s_bt_pcm_wpos = 0;
static size_t s_bt_pcm_fill = 0;
static uint32_t s_bt_resample_phase_q16 = 0;
static uint32_t s_bt_a2dp_sample_rate = BT_A2DP_TARGET_SAMPLE_RATE;
static volatile bool s_bt_media_prime_pending = false;
static TickType_t s_bt_media_prime_deadline = 0;
static size_t s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
static portMUX_TYPE s_bt_pcm_mux = portMUX_INITIALIZER_UNLOCKED;
static size_t s_bt_tel_drop_write_bytes = 0;
static size_t s_bt_tel_trim_write_bytes = 0;
static size_t s_bt_tel_trim_read_bytes = 0;
static size_t s_bt_tel_pad_silence_bytes = 0;
static bool s_bt_coex_streaming_hint = false;
#if CONFIG_ESP_COEX_ENABLED
static esp_coex_prefer_t s_bt_coex_preference = ESP_COEX_PREFER_BALANCE;
#endif
static TickType_t s_bt_speaker_resume_after = 0;
#define BT_SPEAKER_REINIT_BACKOFF_MIN_MS 3000U
#define BT_SPEAKER_REINIT_BACKOFF_MAX_MS 15000U
static uint32_t s_bt_speaker_resume_backoff_ms = BT_SPEAKER_REINIT_BACKOFF_MIN_MS;

static void bt_update_coex_preference(bool streaming)
{
#if CONFIG_ESP_COEX_ENABLED
    esp_coex_prefer_t target = streaming ? ESP_COEX_PREFER_BT : ESP_COEX_PREFER_BALANCE;
    if (s_bt_coex_preference == target) return;

    esp_err_t err = esp_coex_preference_set(target);
    if (err == ESP_OK) {
        s_bt_coex_preference = target;
        ESP_LOGI(TAG, "bt: coex prefer=%s", streaming ? "bt" : "balance");
    } else {
        ESP_LOGW(TAG, "bt: coex preference set failed: %s", esp_err_to_name(err));
    }
#else
    (void)streaming;
#endif
}

static void bt_update_coex_streaming_hint(bool streaming)
{
#if CONFIG_ESP_COEX_ENABLED
    if (s_bt_coex_streaming_hint == streaming) return;

    esp_err_t err;
    if (streaming) {
        err = esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_STREAMING);
        if (err == ESP_OK) {
            s_bt_coex_streaming_hint = true;
            ESP_LOGI(TAG, "bt: coex hint set: A2DP streaming");
            bt_update_coex_preference(true);
        } else {
            ESP_LOGW(TAG, "bt: coex set streaming hint failed: %s", esp_err_to_name(err));
        }
    } else {
        err = esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BT, ESP_COEX_BT_ST_A2DP_STREAMING);
        if (err == ESP_OK) {
            s_bt_coex_streaming_hint = false;
            ESP_LOGI(TAG, "bt: coex hint cleared: A2DP streaming");
            bt_update_coex_preference(false);
        } else {
            ESP_LOGW(TAG, "bt: coex clear streaming hint failed: %s", esp_err_to_name(err));
        }
    }
#else
    (void)streaming;
#endif
}

static void bt_tel_snapshot_and_reset(size_t *drop_write,
                                      size_t *trim_write,
                                      size_t *trim_read,
                                      size_t *pad_silence)
{
    taskENTER_CRITICAL(&s_bt_pcm_mux);
    if (drop_write) *drop_write = s_bt_tel_drop_write_bytes;
    if (trim_write) *trim_write = s_bt_tel_trim_write_bytes;
    if (trim_read) *trim_read = s_bt_tel_trim_read_bytes;
    if (pad_silence) *pad_silence = s_bt_tel_pad_silence_bytes;
    s_bt_tel_drop_write_bytes = 0;
    s_bt_tel_trim_write_bytes = 0;
    s_bt_tel_trim_read_bytes = 0;
    s_bt_tel_pad_silence_bytes = 0;
    taskEXIT_CRITICAL(&s_bt_pcm_mux);
}

static void bt_pcm_clear(void)
{
    taskENTER_CRITICAL(&s_bt_pcm_mux);
    s_bt_pcm_rpos = 0;
    s_bt_pcm_wpos = 0;
    s_bt_pcm_fill = 0;
    s_bt_tel_drop_write_bytes = 0;
    s_bt_tel_trim_write_bytes = 0;
    s_bt_tel_trim_read_bytes = 0;
    s_bt_tel_pad_silence_bytes = 0;
    taskEXIT_CRITICAL(&s_bt_pcm_mux);
}

static size_t bt_pcm_write_bytes(const uint8_t *src, size_t len)
{
    if (!s_bt_pcm_ring || !src || len == 0) return 0;

    size_t req_len = len;
    taskENTER_CRITICAL(&s_bt_pcm_mux);
    size_t free_space = BT_PCM_RING_BYTES - s_bt_pcm_fill;
    if (len > free_space) len = free_space;
    if (req_len > len) s_bt_tel_drop_write_bytes += (req_len - len);
    size_t aligned = len - (len % BT_PCM_FRAME_BYTES);
    if (aligned < len) s_bt_tel_trim_write_bytes += (len - aligned);
    len = aligned;
    if (len == 0) {
        taskEXIT_CRITICAL(&s_bt_pcm_mux);
        return 0;
    }

    size_t first = BT_PCM_RING_BYTES - s_bt_pcm_wpos;
    if (first > len) first = len;
    memcpy(&s_bt_pcm_ring[s_bt_pcm_wpos], src, first);
    s_bt_pcm_wpos = (s_bt_pcm_wpos + first) % BT_PCM_RING_BYTES;

    size_t rem = len - first;
    if (rem > 0) {
        memcpy(&s_bt_pcm_ring[s_bt_pcm_wpos], src + first, rem);
        s_bt_pcm_wpos = (s_bt_pcm_wpos + rem) % BT_PCM_RING_BYTES;
    }

    s_bt_pcm_fill += len;
    taskEXIT_CRITICAL(&s_bt_pcm_mux);
    return len;
}

static size_t bt_pcm_read_bytes(uint8_t *dst, size_t len)
{
    if (!s_bt_pcm_ring || !dst || len == 0) return 0;

    taskENTER_CRITICAL(&s_bt_pcm_mux);
    size_t take = (len < s_bt_pcm_fill) ? len : s_bt_pcm_fill;
    size_t aligned = take - (take % BT_PCM_FRAME_BYTES);
    if (aligned < take) s_bt_tel_trim_read_bytes += (take - aligned);
    take = aligned;
    if (take == 0) {
        taskEXIT_CRITICAL(&s_bt_pcm_mux);
        return 0;
    }

    size_t first = BT_PCM_RING_BYTES - s_bt_pcm_rpos;
    if (first > take) first = take;
    memcpy(dst, &s_bt_pcm_ring[s_bt_pcm_rpos], first);
    s_bt_pcm_rpos = (s_bt_pcm_rpos + first) % BT_PCM_RING_BYTES;

    size_t rem = take - first;
    if (rem > 0) {
        memcpy(dst + first, &s_bt_pcm_ring[s_bt_pcm_rpos], rem);
        s_bt_pcm_rpos = (s_bt_pcm_rpos + rem) % BT_PCM_RING_BYTES;
    }

    s_bt_pcm_fill -= take;
    taskEXIT_CRITICAL(&s_bt_pcm_mux);
    return take;
}

static size_t bt_pcm_fill_bytes(void)
{
    size_t fill;
    taskENTER_CRITICAL(&s_bt_pcm_mux);
    fill = s_bt_pcm_fill;
    taskEXIT_CRITICAL(&s_bt_pcm_mux);
    return fill;
}

static int16_t bt_scale_sample(int16_t s, int volume_percent)
{
    if (volume_percent >= 100) return s;
    if (volume_percent <= 0) return 0;
    int32_t scaled = ((int32_t)s * (int32_t)volume_percent) / 100;
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    return (int16_t)scaled;
}

static size_t bt_pcm_write_from_decoder(const int16_t *samples,
                                        size_t sample_count,
                                        int channels,
                                        int volume_percent)
{
    if (!samples || sample_count == 0) return 0;
    if (!s_bt.connected) return 0;
    if (channels != 1 && channels != 2) return 0;

    uint8_t frame_bytes[512];
    size_t in_pos = 0;
    size_t frames_per_chunk = sizeof(frame_bytes) / 4;
    size_t written_total = 0;

    while (in_pos < sample_count) {
        size_t frames = frames_per_chunk;
        if (channels == 2) {
            size_t rem_frames = (sample_count - in_pos) / 2;
            if (rem_frames < frames) frames = rem_frames;
        } else {
            size_t rem_frames = sample_count - in_pos;
            if (rem_frames < frames) frames = rem_frames;
        }
        if (frames == 0) break;

        for (size_t i = 0; i < frames; i++) {
            int16_t l;
            int16_t r;
            if (channels == 2) {
                l = bt_scale_sample(samples[in_pos + (i * 2)], volume_percent);
                r = bt_scale_sample(samples[in_pos + (i * 2) + 1], volume_percent);
            } else {
                l = bt_scale_sample(samples[in_pos + i], volume_percent);
                r = l;
            }

            frame_bytes[(i * 4)] = (uint8_t)(l & 0xFF);
            frame_bytes[(i * 4) + 1] = (uint8_t)((l >> 8) & 0xFF);
            frame_bytes[(i * 4) + 2] = (uint8_t)(r & 0xFF);
            frame_bytes[(i * 4) + 3] = (uint8_t)((r >> 8) & 0xFF);
        }

        written_total += bt_pcm_write_bytes(frame_bytes, frames * 4);
        if (channels == 2) {
            in_pos += frames * 2;
        } else {
            in_pos += frames;
        }
    }

    return written_total;
}

static size_t bt_pcm_write_resampled_44k(const int16_t *samples,
                                         size_t sample_count,
                                         int channels,
                                         int sample_rate,
                                         int volume_percent)
{
    if (!samples || sample_count == 0) return 0;
    if (!s_bt.connected) return 0;
    if (channels != 1 && channels != 2) return 0;
    if (sample_rate <= 0) return 0;

    uint32_t out_rate = s_bt_a2dp_sample_rate;
    if (out_rate == 0) out_rate = BT_A2DP_TARGET_SAMPLE_RATE;

    if ((uint32_t)sample_rate == out_rate) {
        s_bt_resample_phase_q16 = 0;
        return bt_pcm_write_from_decoder(samples, sample_count, channels, volume_percent);
    }

    size_t src_frames = (channels == 2) ? (sample_count / 2U) : sample_count;
    if (src_frames == 0) return 0;

    uint32_t step_q16 = (uint32_t)((((uint64_t)sample_rate) << 16) /
                                   (uint64_t)out_rate);
    if (step_q16 == 0) step_q16 = 1;

    uint32_t src_pos_q16 = s_bt_resample_phase_q16;
    uint8_t frame_bytes[512];
    size_t out_frames = 0;
    size_t written_total = 0;

    while (src_pos_q16 < ((uint32_t)src_frames << 16)) {
        uint32_t src_idx = src_pos_q16 >> 16;
        if (src_idx >= src_frames) break;

        int16_t l;
        int16_t r;
        if (channels == 2) {
            l = bt_scale_sample(samples[src_idx * 2U], volume_percent);
            r = bt_scale_sample(samples[src_idx * 2U + 1U], volume_percent);
        } else {
            l = bt_scale_sample(samples[src_idx], volume_percent);
            r = l;
        }

        frame_bytes[(out_frames * 4U)] = (uint8_t)(l & 0xFF);
        frame_bytes[(out_frames * 4U) + 1U] = (uint8_t)((l >> 8) & 0xFF);
        frame_bytes[(out_frames * 4U) + 2U] = (uint8_t)(r & 0xFF);
        frame_bytes[(out_frames * 4U) + 3U] = (uint8_t)((r >> 8) & 0xFF);
        out_frames++;

        if (out_frames >= (sizeof(frame_bytes) / 4U)) {
            written_total += bt_pcm_write_bytes(frame_bytes, out_frames * 4U);
            out_frames = 0;
        }

        src_pos_q16 += step_q16;
    }

    if (out_frames > 0) {
        written_total += bt_pcm_write_bytes(frame_bytes, out_frames * 4U);
    }

    if (src_pos_q16 >= ((uint32_t)src_frames << 16)) {
        src_pos_q16 -= ((uint32_t)src_frames << 16);
    }
    s_bt_resample_phase_q16 = src_pos_q16;

    return written_total;
}

#if CONFIG_BT_A2DP_ENABLE
static void bt_build_sbc_pref_mcc(esp_a2d_mcc_t *mcc)
{
    if (!mcc) return;
    memset(mcc, 0, sizeof(*mcc));
    mcc->type = ESP_A2D_MCT_SBC;
    /* Conservative SBC profile to reduce ACL/L2CAP congestion on weaker links. */
    mcc->cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_JOINT_STEREO;
    mcc->cie.sbc_info.samp_freq = ESP_A2D_SBC_CIE_SF_44K;
    mcc->cie.sbc_info.alloc_mthd = ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    mcc->cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    mcc->cie.sbc_info.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    mcc->cie.sbc_info.min_bitpool = 2;
    mcc->cie.sbc_info.max_bitpool = 24;
}

static void bt_set_pref_codec(esp_a2d_conn_hdl_t conn_hdl)
{
    esp_a2d_mcc_t pref_mcc;
    bt_build_sbc_pref_mcc(&pref_mcc);
    esp_err_t err = esp_a2d_source_set_pref_mcc(conn_hdl, &pref_mcc);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "bt: preferred SBC codec set (44.1k, bitpool<=%u)",
                 (unsigned int)pref_mcc.cie.sbc_info.max_bitpool);
    } else {
        ESP_LOGW(TAG, "bt: preferred codec set failed: %s", esp_err_to_name(err));
    }
}

static void bt_media_start_if_needed(void) __attribute__((unused));
static void bt_media_start_if_needed(void)
{
    if (!s_bt.connected || s_bt.media_started) return;
    esp_err_t err = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
    if (err == ESP_OK) {
        s_bt.media_started = true;
    } else {
        ESP_LOGW(TAG, "bt: media start failed: %s", esp_err_to_name(err));
    }
}

static void bt_media_stop_if_needed(void) __attribute__((unused));
static void bt_media_stop_if_needed(void)
{
    if (!s_bt.connected || !s_bt.media_started) return;
    esp_err_t err = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
    if (err == ESP_OK) {
        s_bt.media_started = false;
    } else {
        ESP_LOGW(TAG, "bt: media stop failed: %s", esp_err_to_name(err));
    }
}

static int32_t bt_a2dp_data_cb(uint8_t *data, int32_t len)
{
    if (!data || len <= 0) return 0;
    size_t req = ((size_t)len / BT_PCM_FRAME_BYTES) * BT_PCM_FRAME_BYTES;
    size_t taken = bt_pcm_read_bytes(data, req);
    if (taken < req) {
        memset(data + taken, 0, req - taken);
        taskENTER_CRITICAL(&s_bt_pcm_mux);
        s_bt_tel_pad_silence_bytes += (req - taken);
        taskEXIT_CRITICAL(&s_bt_pcm_mux);
    }
    if (req < (size_t)len) {
        memset(data + req, 0, (size_t)len - req);
        taskENTER_CRITICAL(&s_bt_pcm_mux);
        s_bt_tel_pad_silence_bytes += ((size_t)len - req);
        taskEXIT_CRITICAL(&s_bt_pcm_mux);
    }
    return len;
}

static void bt_bda_to_str(const esp_bd_addr_t bda, char *out, size_t out_sz);

static void bt_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (!param) return;

    if (event == ESP_A2D_CONNECTION_STATE_EVT) {
        esp_a2d_connection_state_t st = param->conn_stat.state;
        if (st == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            char connected_bda[18] = {0};
            bt_bda_to_str(param->conn_stat.remote_bda, connected_bda, sizeof(connected_bda));
            if (connected_bda[0]) {
                memcpy(s_bt.bda, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
                s_bt.has_bda = true;
                s_bt.has_device = true;
                strlcpy(s_bt.selected_bda, connected_bda, sizeof(s_bt.selected_bda));
            }
            s_bt.connected = true;
            s_bt.connecting = false;
            s_bt_connect_started_at = 0;
            s_bt.media_started = false;
            s_bt_media_prime_pending = false;
            s_bt_media_prime_deadline = 0;
            s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
            s_bt_hold_local_speaker = false;
            s_bt_resample_phase_q16 = 0;
            s_bt_a2dp_sample_rate = BT_A2DP_TARGET_SAMPLE_RATE;
            bt_update_coex_streaming_hint(false);
            bt_set_pref_codec(param->conn_stat.conn_hdl);
            s_bt.connect_retries = 0;    /* reset — connection succeeded */
            s_bt_pending_reconnect = false;
            bt_pcm_clear();
#if CONFIG_HARDWARE_CORE2
            /* Hard handoff: stop local I2S speaker path while BT sink is active. */
            core2_audio_deinit();
#endif
            ESP_LOGI(TAG, "bt: A2DP connected to %s", s_bt.selected_bda[0] ? s_bt.selected_bda : connected_bda);

            if (s_bt.pairing_ui_active) {
                ESP_LOGI(TAG,
                         "bt: pairing complete name=%s bda=%s",
                         s_bt.selected_name[0] ? s_bt.selected_name : "(unknown)",
                         s_bt.selected_bda[0] ? s_bt.selected_bda : "");
                s_bt.pairing_ui_active = false;
            }
            if (s_mp3_resume_on_bt_reconnect && s_mp3.active) {
                s_mp3.paused = false;
                s_mp3.last_tick = xTaskGetTickCount();
                size_t bt_fill = bt_pcm_fill_bytes();
                if (bt_fill >= BT_PCM_RESUME_PRIME_BYTES) {
                    bt_media_start_if_needed();
                    s_bt_media_prime_pending = false;
                    s_bt_media_prime_deadline = 0;
                    s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
                } else {
                    s_bt_media_prime_pending = true;
                    s_bt_media_prime_target_bytes = BT_PCM_RESUME_PRIME_BYTES;
                    s_bt_media_prime_deadline = xTaskGetTickCount() +
                                                pdMS_TO_TICKS(BT_PCM_RESUME_PRIME_TIMEOUT_MS);
                }
                s_mp3_resume_on_bt_reconnect = false;
                ESP_LOGI(TAG, "bt: resumed MP3 playback after reconnect");
                mp3_request_ui_refresh();
            }
            bt_persist_current_peer();
        } else if (st == ESP_A2D_CONNECTION_STATE_CONNECTING) {
            s_bt.connecting = true;
            s_bt.connected = false;
            s_bt.media_started = false;
            s_bt_connect_started_at = xTaskGetTickCount();
        } else {
            s_bt.connecting = false;
            s_bt_connect_started_at = 0;
            s_bt.connected = false;
            s_bt.media_started = false;
            s_bt_media_prime_pending = false;
            s_bt_media_prime_deadline = 0;
            s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
            s_bt_resample_phase_q16 = 0;
            s_bt_a2dp_sample_rate = BT_A2DP_TARGET_SAMPLE_RATE;
            bt_update_coex_streaming_hint(false);
            bt_pcm_clear();
            ESP_LOGI(TAG, "bt: A2DP disconnected reason=%d", (int)param->conn_stat.disc_rsn);
            s_bt_recent_acl_drop = ((int)param->conn_stat.disc_rsn == 0x13);
            if (s_mp3.active && !s_mp3.paused) {
                s_mp3.paused = true;
                s_mp3_resume_on_bt_reconnect = true;
                ESP_LOGI(TAG, "bt: pausing MP3 playback at %lu ms awaiting reconnect",
                         (unsigned long)s_mp3.position_ms);
                mp3_request_ui_refresh();
            }
            s_bt_speaker_resume_after = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
            /* Schedule a retry on transient open/disconnect failures. */
            bool scheduled_retry = false;
            bool allow_runtime_retry = s_bt.pairing_ui_active || s_mp3.active;
            if (s_bt.has_bda && !s_bt.discovering && allow_runtime_retry &&
                s_bt.connect_retries < BT_CONNECT_RETRY_MAX && !s_bt_pending_reconnect) {
                uint32_t min_delay_ms = s_bt_recent_acl_drop ? BT_RECONNECT_DELAY_HARD_DROP_MS : 0;
                bt_schedule_reconnect("A2DP disconnected", min_delay_ms);
                scheduled_retry = true;
            } else {
                s_bt.connect_retries = 0;
            }
            if (s_bt.pairing_ui_active) {
                if (scheduled_retry) {
                } else {
                    ESP_LOGW(TAG, "bt: pairing failed");
                    s_bt.pairing_ui_active = false;
                    s_bt_hold_local_speaker = false;
                }
            } else if (!scheduled_retry) {
                s_bt_hold_local_speaker = false;
            }
        }
        return;
    }

    if (event == ESP_A2D_AUDIO_STATE_EVT) {
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            s_bt.media_started = true;
            s_bt_recent_acl_drop = false;
            bt_update_coex_streaming_hint(true);
        } else {
            s_bt.media_started = false;
            bt_update_coex_streaming_hint(false);
        }
        ESP_LOGI(TAG, "bt: A2DP audio state=%d", (int)param->audio_stat.state);
        return;
    }

    if (event == ESP_A2D_AUDIO_CFG_EVT) {
        uint8_t sf = param->audio_cfg.mcc.cie.sbc_info.samp_freq;
        uint8_t ch_mode = param->audio_cfg.mcc.cie.sbc_info.ch_mode;
        uint8_t block_len = param->audio_cfg.mcc.cie.sbc_info.block_len;
        uint8_t subbands = param->audio_cfg.mcc.cie.sbc_info.num_subbands;
        uint8_t alloc = param->audio_cfg.mcc.cie.sbc_info.alloc_mthd;
        uint8_t min_bp = param->audio_cfg.mcc.cie.sbc_info.min_bitpool;
        uint8_t max_bp = param->audio_cfg.mcc.cie.sbc_info.max_bitpool;
        if (sf & ESP_A2D_SBC_CIE_SF_48K) {
            s_bt_a2dp_sample_rate = 48000;
        } else if (sf & ESP_A2D_SBC_CIE_SF_44K) {
            s_bt_a2dp_sample_rate = 44100;
        } else if (sf & ESP_A2D_SBC_CIE_SF_32K) {
            s_bt_a2dp_sample_rate = 32000;
        } else if (sf & ESP_A2D_SBC_CIE_SF_16K) {
            s_bt_a2dp_sample_rate = 16000;
        } else {
            s_bt_a2dp_sample_rate = BT_A2DP_TARGET_SAMPLE_RATE;
        }
        s_bt_resample_phase_q16 = 0;
        ESP_LOGI(TAG,
                 "bt: negotiated SBC cfg sr=%lu ch_mode=0x%02X blk=0x%02X sub=0x%02X alloc=0x%02X bitpool=%u..%u",
                 (unsigned long)s_bt_a2dp_sample_rate,
                 ch_mode,
                 block_len,
                 subbands,
                 alloc,
                 (unsigned)min_bp,
                 (unsigned)max_bp);
    }
}
#endif

static void bt_bda_to_str(const esp_bd_addr_t bda, char *out, size_t out_sz)
{
    if (!out || out_sz < 18) return;
    snprintf(out, out_sz, "%02X:%02X:%02X:%02X:%02X:%02X",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (!param) return;

    if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            s_bt.discovering = true;
            s_bt.has_candidate = false;
            s_bt.candidate_score = -1000;
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            s_bt.discovering = false;
            if (!s_bt.has_device && s_bt.has_candidate) {
                s_bt.has_device = true;
                s_bt.has_bda = true;
                memcpy(s_bt.bda, s_bt.candidate_bda, ESP_BD_ADDR_LEN);
                strlcpy(s_bt.selected_name, s_bt.candidate_name, sizeof(s_bt.selected_name));
                strlcpy(s_bt.selected_bda, s_bt.candidate_bda_str, sizeof(s_bt.selected_bda));
            }
#if CONFIG_BT_A2DP_ENABLE
            if (s_bt.connect_after_discovery && s_bt.has_bda) {
                s_bt.connect_after_discovery = false;
                if (!bt_start_connect_now("scan complete")) {
                    if (s_bt.pairing_ui_active && !s_bt_pending_reconnect) {
                        ESP_LOGW(TAG, "bt: pairing connect failed");
                        s_bt.pairing_ui_active = false;
                        s_bt_hold_local_speaker = false;
                    }
                }
            } else if (s_bt.pairing_ui_active && !s_bt.has_bda) {
                ESP_LOGW(TAG, "bt: pairing no audio device found");
                s_bt.pairing_ui_active = false;
                s_bt_hold_local_speaker = false;
            }
#endif
        }
        return;
    }

    if (event != ESP_BT_GAP_DISC_RES_EVT) {
        return;
    }

    char name[sizeof(s_bt.selected_name)] = {0};
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME && p->val && p->len > 0) {
            size_t n = (size_t)p->len;
            if (n >= sizeof(name)) n = sizeof(name) - 1;
            memcpy(name, p->val, n);
            name[n] = '\0';
            break;
        }
        if (p->type == ESP_BT_GAP_DEV_PROP_EIR && p->val) {
            uint8_t eir_len = 0;
            uint8_t *eir = (uint8_t *)p->val;
            uint8_t *eir_name = esp_bt_gap_resolve_eir_data(eir,
                                                             ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME,
                                                             &eir_len);
            if (!eir_name || eir_len == 0) {
                eir_name = esp_bt_gap_resolve_eir_data(eir,
                                                       ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME,
                                                       &eir_len);
            }
            if (eir_name && eir_len > 0) {
                size_t n = (size_t)eir_len;
                if (n >= sizeof(name)) n = sizeof(name) - 1;
                memcpy(name, eir_name, n);
                name[n] = '\0';
                break;
            }
        }
    }

    if (name[0] == '\0') {
        return;
    }

    int rssi = -90;
    uint32_t cod = 0;
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        if (p->type == ESP_BT_GAP_DEV_PROP_RSSI && p->val) {
            rssi = (int)(*(int8_t *)p->val);
        } else if (p->type == ESP_BT_GAP_DEV_PROP_COD && p->val) {
            cod = *(uint32_t *)p->val;
        }
    }

    int score = bt_score_candidate(name, rssi, cod);
    if (!s_bt.has_candidate || score > s_bt.candidate_score) {
        s_bt.has_candidate = true;
        s_bt.candidate_score = score;
        strlcpy(s_bt.candidate_name, name, sizeof(s_bt.candidate_name));
        bt_bda_to_str(param->disc_res.bda, s_bt.candidate_bda_str, sizeof(s_bt.candidate_bda_str));
        memcpy(s_bt.candidate_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
        ESP_LOGI(TAG, "bt: candidate [CoD 0x%05" PRIx32 "] %s (%s) score=%d rssi=%d",
                 cod,
                 s_bt.candidate_name,
                 s_bt.candidate_bda_str,
                 score,
                 rssi);
    }
}

static bool bt_init_if_needed(void)
{
    if (s_bt.initialized) return true;

    if (!s_bt_pcm_ring) {
        s_bt_pcm_ring = heap_caps_malloc(BT_PCM_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_bt_pcm_ring) {
            s_bt_pcm_ring = malloc(BT_PCM_RING_BYTES);
        }
        if (!s_bt_pcm_ring) {
            ESP_LOGE(TAG, "bt: failed to allocate pcm ring");
            return false;
        }
        bt_pcm_clear();
    }

    esp_err_t err;
#if defined(CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY) || defined(CONFIG_BTDM_CONTROLLER_MODE_BR_EDR_ONLY)
    err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "bt: mem_release(BLE) failed: %s", esp_err_to_name(err));
    }
#endif

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bt: controller init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err == ESP_ERR_INVALID_ARG) {
        // Some BT controller configs only accept BTDM at runtime.
        ESP_LOGW(TAG, "bt: CLASSIC enable rejected, retrying BTDM");
        err = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bt: controller enable failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bt: bluedroid init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bt: bluedroid enable failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_bt_gap_register_callback(bt_gap_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt: GAP callback registration failed: %s", esp_err_to_name(err));
        return false;
    }

#if CONFIG_BT_A2DP_ENABLE
    err = esp_a2d_register_callback(bt_a2dp_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt: A2DP callback registration failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_a2d_source_register_data_callback(bt_a2dp_data_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt: A2DP data callback registration failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_a2d_source_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt: A2DP source init failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_a2d_mcc_t sep_mcc;
    bt_build_sbc_pref_mcc(&sep_mcc);
    err = esp_a2d_source_register_stream_endpoint(0, &sep_mcc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bt: register SEP(0) failed: %s", esp_err_to_name(err));
    }
#endif

    err = esp_bt_gap_set_device_name("TCMD-Core2");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bt: set device name failed: %s", esp_err_to_name(err));
    }

    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code;
    memset(pin_code, 0, sizeof(pin_code));
    err = esp_bt_gap_set_pin(pin_type, 0, pin_code);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bt: set pin failed: %s", esp_err_to_name(err));
    }

    /* Allow outgoing Classic BT connections but do not advertise the device */
    err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bt: set_scan_mode failed: %s", esp_err_to_name(err));
    }

    s_bt.initialized = true;
    ESP_LOGI(TAG, "bt: classic Bluetooth initialized");
    return true;
}

static void bt_cmd_pair_start(void) __attribute__((unused));
static void bt_cmd_pair_start(void)
{
    s_bt_hold_local_speaker = true;
#if CONFIG_HARDWARE_CORE2
    /* Free local speaker-task and I2S resources before Bluedroid startup. */
    core2_audio_deinit();
#endif
    if (!bt_init_if_needed()) {
        s_bt_hold_local_speaker = false;
        screen_draw_text("Bluetooth init\nfailed");
        return;
    }
    if (s_bt.discovering) {
        screen_draw_text("Bluetooth pairing\nin progress...\nScanning");
        return;
    }

    s_bt.has_device = false;
    s_bt.has_bda = false;
    s_bt.has_candidate = false;
    s_bt.candidate_score = -1000;
#if CONFIG_BT_A2DP_ENABLE
    s_bt.connect_after_discovery = true;
    s_bt.connect_retries = 0;
    s_bt.pairing_ui_active = true;
    s_bt_pending_reconnect = false;
    s_bt_connect_started_at = 0;
#endif
    s_bt.selected_name[0] = '\0';
    s_bt.selected_bda[0] = '\0';

    esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt: start discovery failed: %s", esp_err_to_name(err));
        s_bt_hold_local_speaker = false;
        screen_draw_text("Bluetooth pairing\nfailed to start");
        return;
    }

    screen_draw_text("Bluetooth pairing\nstarted...\nScanning for\naudio devices");
}

static void bt_cmd_status(void) __attribute__((unused));
static void bt_cmd_status(void)
{
    char saved_bda[18] = {0};
    bool auto_target_set = nvs_read_str(NVS_KEY_BT_BDA, saved_bda, sizeof(saved_bda));

    if (!s_bt.initialized) {
        screen_draw_text(auto_target_set
                             ? "Bluetooth status:\nnot initialized\nauto target: set"
                             : "Bluetooth status:\nnot initialized\nauto target: none");
        return;
    }
    if (s_bt.discovering) {
        screen_draw_text(auto_target_set
                             ? "Bluetooth status:\nscanning...\nauto target: set"
                             : "Bluetooth status:\nscanning...\nauto target: none");
        return;
    }
#if CONFIG_BT_A2DP_ENABLE
    if (s_bt.connecting) {
        screen_draw_text(auto_target_set
                             ? "Bluetooth status:\nconnecting...\nauto target: set"
                             : "Bluetooth status:\nconnecting...\nauto target: none");
        return;
    }
    if (s_bt.connected) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "Bluetooth status:\nconnected:\n%s\n%s\nauto target: %s",
                 s_bt.selected_name[0] ? s_bt.selected_name : "(unknown)",
                 s_bt.selected_bda[0] ? s_bt.selected_bda : "",
                 auto_target_set ? "set" : "none");
        screen_draw_text(msg);
        return;
    }
#endif
    if (s_bt.has_device) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "Bluetooth status:\nselected device:\n%s\n%s\nauto target: %s",
                 s_bt.selected_name,
                 s_bt.selected_bda,
                 auto_target_set ? "set" : "none");
        screen_draw_text(msg);
        return;
    }
    screen_draw_text(auto_target_set
                         ? "Bluetooth status:\nno device selected\nauto target: set"
                         : "Bluetooth status:\nno device selected\nauto target: none");
}

static void bt_cmd_disconnect(void) __attribute__((unused));
static void bt_cmd_disconnect(void)
{
    if (!s_bt.initialized) {
        screen_draw_text("Bluetooth\nnot initialized");
        return;
    }
    if (s_bt.discovering) {
        esp_bt_gap_cancel_discovery();
    }
#if CONFIG_BT_A2DP_ENABLE
    if ((s_bt.connected || s_bt.connecting) && s_bt.has_bda) {
        esp_err_t err = esp_a2d_source_disconnect(s_bt.bda);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bt: disconnect request failed: %s", esp_err_to_name(err));
        }
    }
    s_bt.connect_after_discovery = false;
    s_bt.connect_retries = 0;    /* user-initiated: cancel retries */
    s_bt.pairing_ui_active = false;
    s_bt_pending_reconnect = false;
    s_bt_connect_started_at = 0;
    s_bt_hold_local_speaker = false;
    s_mp3_resume_on_bt_reconnect = false;
#endif
    s_bt.has_device = false;
    s_bt.has_bda = false;
    s_bt.has_candidate = false;
#if CONFIG_BT_A2DP_ENABLE
    s_bt.connected = false;
    s_bt.connecting = false;
#endif
    s_bt.selected_name[0] = '\0';
    s_bt.selected_bda[0] = '\0';
    bt_pcm_clear();
    screen_draw_text("Bluetooth device\ncleared");
}

static void bt_cmd_forget(void) __attribute__((unused));
static void bt_cmd_forget(void)
{
    bt_cmd_disconnect();
    esp_err_t e1 = nvs_erase_key_local(NVS_KEY_BT_BDA);
    esp_err_t e2 = nvs_erase_key_local(NVS_KEY_BT_NAME);
    if (e1 == ESP_OK && e2 == ESP_OK) {
        screen_draw_text("Bluetooth\npaired device\nforgotten");
        ESP_LOGI(TAG, "bt: persisted device removed");
    } else {
        screen_draw_text("Bluetooth\nforget failed");
        ESP_LOGW(TAG, "bt: forget failed bda=%s name=%s",
                 esp_err_to_name(e1),
                 esp_err_to_name(e2));
    }
}

static void bt_try_reconnect_on_boot(void) __attribute__((unused));
static void bt_try_reconnect_on_boot(void)
{
#if CONFIG_BT_A2DP_ENABLE
    if (s_bt_reconnect_attempted) return;
    s_bt_reconnect_attempted = true;

    char bda_str[18] = {0};
    char name[64] = {0};
    if (!nvs_read_str(NVS_KEY_BT_BDA, bda_str, sizeof(bda_str))) {
        return;
    }

    esp_bd_addr_t bda = {0};
    if (!bt_parse_bda(bda_str, bda)) {
        ESP_LOGW(TAG, "bt: invalid persisted bda '%s'", bda_str);
        return;
    }

    s_bt_hold_local_speaker = true;
#if CONFIG_HARDWARE_CORE2
    /* Free local speaker resources before boot-time BT auto reconnect. */
    core2_audio_deinit();
#endif

    if (!bt_has_startup_headroom("boot reconnect")) {
        s_bt_hold_local_speaker = false;
        return;
    }

    if (!bt_init_if_needed()) {
        s_bt_hold_local_speaker = false;
        return;
    }

    memcpy(s_bt.bda, bda, ESP_BD_ADDR_LEN);
    s_bt.has_bda = true;
    s_bt.has_device = true;
    strlcpy(s_bt.selected_bda, bda_str, sizeof(s_bt.selected_bda));
    if (nvs_read_str(NVS_KEY_BT_NAME, name, sizeof(name))) {
        strlcpy(s_bt.selected_name, name, sizeof(s_bt.selected_name));
    } else {
        strlcpy(s_bt.selected_name, "last device", sizeof(s_bt.selected_name));
    }

    esp_err_t err = esp_a2d_source_connect(s_bt.bda);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bt: reconnect on boot failed: %s", esp_err_to_name(err));
        bt_schedule_reconnect("boot reconnect", 0);
        s_bt_hold_local_speaker = false;
        return;
    }

    s_bt.connecting = true;
    s_bt_connect_started_at = xTaskGetTickCount();
    ESP_LOGI(TAG, "bt: reconnecting to %s (%s)", s_bt.selected_name, s_bt.selected_bda);
#endif
}
#else
static void bt_cmd_pair_start(void)
{
    screen_draw_text("Bluetooth not\nenabled in this\nfirmware build");
}

static void bt_cmd_status(void)
{
    screen_draw_text("Bluetooth status:\ndisabled in\nfirmware build");
}

static void bt_cmd_disconnect(void)
{
    screen_draw_text("Bluetooth disabled\nin firmware build");
}

static void bt_cmd_forget(void)
{
    screen_draw_text("Bluetooth disabled\nin firmware build");
}

static void bt_try_reconnect_on_boot(void)
{
}
#endif

static inline void mp3_request_ui_refresh(void)
{
    s_mp3_ui_pending = true;
}

static void pf_status_draw(const char *msg) __attribute__((unused));
static void pf_status_draw(const char *msg)
{
    /* Keep status screens legible even if saved colors are invalid/invisible. */
    screen_set_color(0, 0, 0);
    screen_set_text_color(255, 255, 255);
    screen_draw_text(msg);
}

static bool nvs_read_str(const char *key, char *out, size_t out_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_str(h, key, out, &out_sz);
    nvs_close(h);
    return err == ESP_OK && out[0] != '\0';
}

static esp_err_t nvs_write_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool nvs_read_u8(const char *key, uint8_t *out) __attribute__((unused));
static bool nvs_read_u8(const char *key, uint8_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_u8(h, key, out);
    nvs_close(h);
    return err == ESP_OK;
}

static esp_err_t nvs_write_u8(const char *key, uint8_t val) __attribute__((unused));
static esp_err_t nvs_write_u8(const char *key, uint8_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_erase_key_local(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool str_ends_with_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s);
    size_t lf = strlen(suffix);
    if (ls < lf) return false;
    s += (ls - lf);
    while (*suffix) {
        if (tolower((unsigned char)*s++) != tolower((unsigned char)*suffix++)) {
            return false;
        }
    }
    return true;
}

static bool is_mp3_file_name(const char *name)
{
    return str_ends_with_ci(name, ".mp3");
}

static bool trigger_reserved(const char *trigger) __attribute__((unused));
static bool trigger_reserved(const char *trigger)
{
    static const char *reserved[] = {
        "text", "color", "textcolor", "fontsize", "landscape", "portrait",
        "jpeg", "save", "play", "stop", "next", "previous", "forward", "reverse", "volumeup",
        "volumedown", "shuffle", "repeattrack", "repeatplaylist",
        "pair", "btstatus", "btdisconnect", "btforget"
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (strcasecmp(trigger, reserved[i]) == 0) return true;
    }
    return false;
}
static int mp3_count_in_folder(const char *folder_path) __attribute__((unused));
static int mp3_count_in_folder(const char *folder_path)
{
    DIR *d = opendir(folder_path);
    if (!d) return 0;

    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (is_mp3_file_name(e->d_name)) count++;
    }
    closedir(d);
    return count;
}

static bool mp3_get_nth_file(const char *folder_path,
                             int target_idx,
                             char *out_name,
                             size_t out_sz,
                             int *total_out) __attribute__((unused));
static bool mp3_get_nth_file(const char *folder_path,
                             int target_idx,
                             char *out_name,
                             size_t out_sz,
                             int *total_out)
{
    DIR *d = opendir(folder_path);
    if (!d) return false;

    int index = 0;
    int total = 0;
    bool ok = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!is_mp3_file_name(e->d_name)) continue;
        if (index == target_idx) {
            strncpy(out_name, e->d_name, out_sz - 1);
            out_name[out_sz - 1] = '\0';
            ok = true;
        }
        index++;
        total++;
    }
    closedir(d);

    if (total_out) *total_out = total;
    return ok;
}

typedef struct {
    int frame_bytes;
    int sample_rate;
    int samples_per_frame;
} mp3_frame_info_t;

static bool mp3_parse_frame_header(uint32_t h, mp3_frame_info_t *out)
{
    static const int br_v1_l3[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const int br_v2_l3[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const int sr_tbl[3]    = {44100, 48000, 32000};

    if ((h & 0xFFE00000U) != 0xFFE00000U) return false;

    int version_id = (int)((h >> 19) & 0x3U);
    int layer = (int)((h >> 17) & 0x3U);
    int bitrate_idx = (int)((h >> 12) & 0xFU);
    int sample_idx = (int)((h >> 10) & 0x3U);
    int padding = (int)((h >> 9) & 0x1U);

    if (version_id == 1) return false;        /* reserved */
    if (layer != 1) return false;             /* only Layer III */
    if (bitrate_idx == 0 || bitrate_idx == 15) return false;
    if (sample_idx == 3) return false;

    int sample_rate = sr_tbl[sample_idx];
    bool mpeg1 = (version_id == 3);
    if (version_id == 2) sample_rate /= 2;    /* MPEG 2 */
    if (version_id == 0) sample_rate /= 4;    /* MPEG 2.5 */

    int bitrate_kbps = mpeg1 ? br_v1_l3[bitrate_idx] : br_v2_l3[bitrate_idx];
    if (bitrate_kbps <= 0 || sample_rate <= 0) return false;

    int samples_per_frame = mpeg1 ? 1152 : 576;
    int coeff = mpeg1 ? 144 : 72;
    int frame_bytes = (coeff * bitrate_kbps * 1000) / sample_rate + padding;
    if (frame_bytes < 24) return false;

    out->frame_bytes = frame_bytes;
    out->sample_rate = sample_rate;
    out->samples_per_frame = samples_per_frame;
    return true;
}

static bool mp3_stream_next_frame(FILE *fp, mp3_frame_info_t *info)
{
    int b0 = fgetc(fp);
    int b1 = fgetc(fp);
    int b2 = fgetc(fp);
    int b3 = fgetc(fp);
    if (b0 == EOF || b1 == EOF || b2 == EOF || b3 == EOF) return false;

    while (true) {
        uint32_t h = ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) |
                     ((uint32_t)b2 << 8) | (uint32_t)b3;
        if (mp3_parse_frame_header(h, info)) {
            return true;
        }

        b0 = b1;
        b1 = b2;
        b2 = b3;
        b3 = fgetc(fp);
        if (b3 == EOF) return false;
    }
}

static uint32_t mp3_probe_duration_ms(const char *file_path) __attribute__((unused));
static uint32_t mp3_probe_duration_ms(const char *file_path)
{
    FILE *fp = fopen(file_path, "rb");
    if (!fp) return 180000;

    uint64_t total_samples = 0;
    int last_sample_rate = 0;
    mp3_frame_info_t info;

    while (mp3_stream_next_frame(fp, &info)) {
        total_samples += (uint64_t)info.samples_per_frame;
        last_sample_rate = info.sample_rate;
        if (fseek(fp, info.frame_bytes - 4, SEEK_CUR) != 0) {
            break;
        }
    }
    fclose(fp);

    if (total_samples == 0 || last_sample_rate <= 0) return 180000;

    uint64_t ms = (total_samples * 1000ULL) / (uint64_t)last_sample_rate;
    if (ms < 1000ULL) ms = 1000ULL;
    if (ms > 7200000ULL) ms = 7200000ULL;
    return (uint32_t)ms;
}

static bool mp3_queue_seek_relative(int32_t delta_ms, const char *reason)
{
    if (!s_mp3.active) {
        ESP_LOGW(TAG, "mp3: %s ignored because no track is active", reason ? reason : "seek");
        return false;
    }

    int64_t target_ms = (int64_t)s_mp3.position_ms + (int64_t)delta_ms;
    if (target_ms < 0) target_ms = 0;
    if (s_mp3.duration_ms > 0 && target_ms >= s_mp3.duration_ms) {
        target_ms = (int64_t)s_mp3.duration_ms - 1;
        if (target_ms < 0) target_ms = 0;
    }

    s_mp3_seek_target_ms = (int32_t)target_ms;
    ESP_LOGI(TAG, "mp3: %s queued -> %lu ms", reason ? reason : "seek",
             (unsigned long)target_ms);
    return true;
}

static void mp3_log_mode_status(const char *reason)
{
    ESP_LOGI(TAG,
             "mp3: %s -> shuffle=%s repeattrack=%s repeatplaylist=%s",
             reason ? reason : "mode update",
             s_mp3.shuffle ? "on" : "off",
             s_mp3.repeat_track ? "on" : "off",
             s_mp3.repeat_playlist ? "on" : "off");
}

static void mp3_player_task(void *arg) __attribute__((unused));
static void mp3_player_task(void *arg)
{
    (void)arg;

    FILE *fp = NULL;
    uint32_t opened_token = 0;
    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) {
        ESP_LOGE(TAG, "mp3: MP3InitDecoder failed");
        vTaskDelete(NULL);
        return;
    }

    unsigned char *inbuf = heap_caps_malloc(MP3_INPUT_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!inbuf) inbuf = heap_caps_malloc(MP3_INPUT_BUF_BYTES, MALLOC_CAP_8BIT);
    short *pcm = heap_caps_malloc((size_t)(1152 * 2 * (int)sizeof(short)),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) pcm = heap_caps_malloc((size_t)(1152 * 2 * (int)sizeof(short)), MALLOC_CAP_8BIT);
    if (!inbuf || !pcm) {
        ESP_LOGE(TAG, "mp3: buffer allocation failed (inbuf=%p pcm=%p)", (void *)inbuf, (void *)pcm);
        if (inbuf) free(inbuf);
        if (pcm) free(pcm);
        MP3FreeDecoder(dec);
        s_mp3_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    unsigned char *read_ptr = inbuf;
    int bytes_left = 0;
    bool speaker_path_ready = false;
    uint32_t speaker_last_rate = 0;

    int64_t tel_window_start_us __attribute__((unused)) = esp_timer_get_time();
    uint32_t tel_decoded_frames __attribute__((unused)) = 0;
    uint32_t tel_underflows __attribute__((unused)) = 0;
    uint32_t tel_decode_errors __attribute__((unused)) = 0;
    uint32_t tel_refills __attribute__((unused)) = 0;
    size_t tel_refill_bytes __attribute__((unused)) = 0;
    uint64_t tel_refill_us_total __attribute__((unused)) = 0;
    uint64_t tel_refill_us_max __attribute__((unused)) = 0;
    uint64_t tel_decode_us_total __attribute__((unused)) = 0;
    uint64_t tel_decode_us_max __attribute__((unused)) = 0;
    uint64_t tel_output_us_total __attribute__((unused)) = 0;
    uint64_t tel_output_us_max __attribute__((unused)) = 0;
    uint32_t tel_output_calls __attribute__((unused)) = 0;
    uint32_t tel_audio_ms __attribute__((unused)) = 0;
    size_t tel_bt_fill_peak __attribute__((unused)) = 0;
    size_t tel_bt_drop_write_bytes __attribute__((unused)) = 0;
    size_t tel_bt_trim_write_bytes __attribute__((unused)) = 0;
    size_t tel_bt_trim_read_bytes __attribute__((unused)) = 0;
    size_t tel_bt_pad_silence_bytes __attribute__((unused)) = 0;
    int tel_last_rate __attribute__((unused)) = 0;
    int tel_last_channels __attribute__((unused)) = 0;

    while (true) {
        if (!s_mp3.active) {
            if (fp) {
                fclose(fp);
                fp = NULL;
                opened_token = 0;
            }
            bytes_left = 0;
            read_ptr = inbuf;
            speaker_last_rate = 0;
#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
            s_bt_media_prime_pending = false;
            s_bt_media_prime_deadline = 0;
            s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
#endif

            tel_window_start_us = esp_timer_get_time();
            tel_decoded_frames = 0;
            tel_underflows = 0;
            tel_decode_errors = 0;
            tel_refills = 0;
            tel_refill_bytes = 0;
            tel_refill_us_total = 0;
            tel_refill_us_max = 0;
            tel_decode_us_total = 0;
            tel_decode_us_max = 0;
            tel_output_us_total = 0;
            tel_output_us_max = 0;
            tel_output_calls = 0;
            tel_audio_ms = 0;
            tel_bt_fill_peak = 0;
            tel_bt_drop_write_bytes = 0;
            tel_bt_trim_write_bytes = 0;
            tel_bt_trim_read_bytes = 0;
            tel_bt_pad_silence_bytes = 0;
            tel_last_rate = 0;
            tel_last_channels = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (s_mp3.paused) {
            int32_t paused_seek_target = s_mp3_seek_target_ms;
            if (paused_seek_target >= 0) {
                s_mp3_seek_target_ms = -1;
                if (fp) {
                    fclose(fp);
                    fp = NULL;
                }
                MP3FreeDecoder(dec);
                dec = MP3InitDecoder();
                if (!dec) {
                    ESP_LOGE(TAG, "mp3: decoder reset failed while paused seek");
                    s_mp3.paused = true;
                    s_mp3.position_ms = 0;
                    mp3_request_ui_refresh();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
                fp = fopen(s_mp3.file_path, "rb");
                opened_token = s_mp3.play_token;
                bytes_left = 0;
                read_ptr = inbuf;
                if (!fp) {
                    ESP_LOGW(TAG, "mp3: failed to reopen %s for seek", s_mp3.file_path);
                    s_mp3.paused = true;
                    mp3_request_ui_refresh();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
                bt_pcm_clear();
                s_bt_media_prime_pending = false;
                s_bt_media_prime_deadline = 0;
                s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
#endif
                uint32_t seeked_ms = 0;
                while (seeked_ms < (uint32_t)paused_seek_target) {
                    if (bytes_left < (MP3_INPUT_BUF_BYTES / 2)) {
                        if (bytes_left > 0 && read_ptr != inbuf) {
                            memmove(inbuf, read_ptr, (size_t)bytes_left);
                        }
                        read_ptr = inbuf;
                        size_t n = fread(inbuf + bytes_left,
                                         1,
                                         (size_t)MP3_INPUT_BUF_BYTES - (size_t)bytes_left,
                                         fp);
                        bytes_left += (int)n;
                    }

                    if (bytes_left <= 4) break;

                    int sync = MP3FindSyncWord(read_ptr, bytes_left);
                    if (sync < 0) {
                        bytes_left = 0;
                        read_ptr = inbuf;
                        continue;
                    }
                    read_ptr += sync;
                    bytes_left -= sync;

                    int seek_err = MP3Decode(dec, &read_ptr, &bytes_left, pcm, 0);
                    if (seek_err == ERR_MP3_INDATA_UNDERFLOW || seek_err == ERR_MP3_MAINDATA_UNDERFLOW) {
                        continue;
                    }
                    if (seek_err != ERR_MP3_NONE) {
                        if (bytes_left > 0) {
                            read_ptr++;
                            bytes_left--;
                        } else {
                            bytes_left = 0;
                            read_ptr = inbuf;
                        }
                        continue;
                    }

                    MP3FrameInfo seek_fi = {0};
                    MP3GetLastFrameInfo(dec, &seek_fi);
                    if (seek_fi.outputSamps <= 0 || seek_fi.samprate <= 0 || seek_fi.nChans <= 0) {
                        continue;
                    }

                    uint32_t mono_samples = (uint32_t)(seek_fi.outputSamps / seek_fi.nChans);
                    uint32_t frame_ms = (uint32_t)(((uint64_t)mono_samples * 1000ULL) /
                                                   (uint64_t)seek_fi.samprate);
                    if (frame_ms == 0) frame_ms = 1;
                    seeked_ms += frame_ms;
                }

                s_mp3.position_ms = seeked_ms;
                s_mp3.last_tick = xTaskGetTickCount();
                ESP_LOGI(TAG, "mp3: seek applied while paused -> %lu/%lu ms",
                         (unsigned long)s_mp3.position_ms,
                         (unsigned long)s_mp3.duration_ms);
                mp3_request_ui_refresh();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!fp || opened_token != s_mp3.play_token) {
            if (fp) fclose(fp);
            fp = fopen(s_mp3.file_path, "rb");
            opened_token = s_mp3.play_token;
            bytes_left = 0;
            read_ptr = inbuf;
            if (!fp) {
                ESP_LOGW(TAG, "mp3: failed to open %s", s_mp3.file_path);
                s_mp3.paused = true;
                mp3_request_ui_refresh();
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }

        int32_t seek_target_ms = s_mp3_seek_target_ms;
        if (seek_target_ms >= 0) {
            s_mp3_seek_target_ms = -1;
            if (fp) {
                fclose(fp);
                fp = NULL;
            }
            MP3FreeDecoder(dec);
            dec = MP3InitDecoder();
            if (!dec) {
                ESP_LOGE(TAG, "mp3: decoder reset failed while seeking");
                s_mp3.paused = true;
                s_mp3.position_ms = 0;
                mp3_request_ui_refresh();
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            fp = fopen(s_mp3.file_path, "rb");
            opened_token = s_mp3.play_token;
            bytes_left = 0;
            read_ptr = inbuf;
            if (!fp) {
                ESP_LOGW(TAG, "mp3: failed to reopen %s for seek", s_mp3.file_path);
                s_mp3.paused = true;
                mp3_request_ui_refresh();
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
            bt_pcm_clear();
            s_bt_media_prime_pending = false;
            s_bt_media_prime_deadline = 0;
            s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
#endif
            speaker_path_ready = false;
            speaker_last_rate = 0;
            uint32_t seeked_ms = 0;
            while (seeked_ms < (uint32_t)seek_target_ms) {
                if (bytes_left < (MP3_INPUT_BUF_BYTES / 2)) {
                    if (bytes_left > 0 && read_ptr != inbuf) {
                        memmove(inbuf, read_ptr, (size_t)bytes_left);
                    }
                    read_ptr = inbuf;
                    size_t n = fread(inbuf + bytes_left,
                                     1,
                                     (size_t)MP3_INPUT_BUF_BYTES - (size_t)bytes_left,
                                     fp);
                    bytes_left += (int)n;
                }

                if (bytes_left <= 4) break;

                int sync = MP3FindSyncWord(read_ptr, bytes_left);
                if (sync < 0) {
                    bytes_left = 0;
                    read_ptr = inbuf;
                    continue;
                }
                read_ptr += sync;
                bytes_left -= sync;

                int seek_err = MP3Decode(dec, &read_ptr, &bytes_left, pcm, 0);
                if (seek_err == ERR_MP3_INDATA_UNDERFLOW || seek_err == ERR_MP3_MAINDATA_UNDERFLOW) {
                    continue;
                }
                if (seek_err != ERR_MP3_NONE) {
                    if (bytes_left > 0) {
                        read_ptr++;
                        bytes_left--;
                    } else {
                        bytes_left = 0;
                        read_ptr = inbuf;
                    }
                    continue;
                }

                MP3FrameInfo seek_fi = {0};
                MP3GetLastFrameInfo(dec, &seek_fi);
                if (seek_fi.outputSamps <= 0 || seek_fi.samprate <= 0 || seek_fi.nChans <= 0) {
                    continue;
                }

                uint32_t mono_samples = (uint32_t)(seek_fi.outputSamps / seek_fi.nChans);
                uint32_t frame_ms = (uint32_t)(((uint64_t)mono_samples * 1000ULL) /
                                               (uint64_t)seek_fi.samprate);
                if (frame_ms == 0) frame_ms = 1;
                seeked_ms += frame_ms;
            }

            s_mp3.position_ms = seeked_ms;
            s_mp3.last_tick = xTaskGetTickCount();
            ESP_LOGI(TAG, "mp3: seek applied -> %lu/%lu ms",
                     (unsigned long)s_mp3.position_ms,
                     (unsigned long)s_mp3.duration_ms);
            mp3_request_ui_refresh();
            continue;
        }

        if (bytes_left < (MP3_INPUT_BUF_BYTES / 2)) {
            if (bytes_left > 0 && read_ptr != inbuf) {
                memmove(inbuf, read_ptr, (size_t)bytes_left);
            }
            read_ptr = inbuf;
            int64_t refill_t0 = esp_timer_get_time();
            size_t n = fread(inbuf + bytes_left,
                             1,
                             (size_t)MP3_INPUT_BUF_BYTES - (size_t)bytes_left,
                             fp);
            uint64_t refill_us = (uint64_t)(esp_timer_get_time() - refill_t0);
            tel_refills++;
            tel_refill_bytes += n;
            tel_refill_us_total += refill_us;
            if (refill_us > tel_refill_us_max) tel_refill_us_max = refill_us;
            bytes_left += (int)n;
        }

        if (bytes_left <= 4) {
            if (!mp3_handle_track_end()) {
                s_mp3.paused = true;
                s_mp3.position_ms = s_mp3.duration_ms;
                mp3_request_ui_refresh();
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int sync = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync < 0) {
            bytes_left = 0;
            read_ptr = inbuf;
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        read_ptr += sync;
        bytes_left -= sync;

        int64_t decode_t0 = esp_timer_get_time();
        int err = MP3Decode(dec, &read_ptr, &bytes_left, pcm, 0);
        uint64_t decode_us = (uint64_t)(esp_timer_get_time() - decode_t0);
        if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            tel_underflows++;
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (err != ERR_MP3_NONE) {
            tel_decode_errors++;
            if (bytes_left > 0) {
                read_ptr++;
                bytes_left--;
            } else {
                bytes_left = 0;
                read_ptr = inbuf;
            }
            continue;
        }

        MP3FrameInfo fi = {0};
        MP3GetLastFrameInfo(dec, &fi);
        if (fi.outputSamps <= 0 || fi.samprate <= 0 || fi.nChans <= 0) {
            continue;
        }

        tel_decoded_frames++;
        tel_decode_us_total += decode_us;
        if (decode_us > tel_decode_us_max) tel_decode_us_max = decode_us;
        tel_last_rate = fi.samprate;
        tel_last_channels = fi.nChans;

        int64_t output_t0 __attribute__((unused)) = esp_timer_get_time();

#if CONFIG_HARDWARE_CORE2
    #if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
        if (s_bt.connected) {
            bool prime_pending = (s_bt_media_prime_pending && !s_bt.media_started);
            size_t stall_limit = prime_pending ? s_bt_media_prime_target_bytes : BT_PCM_TARGET_FILL_BYTES;
            /* Pace the decoder to real-time: stall before writing if the ring
             * is at or above the target fill level, giving the BT stack time
             * to drain ahead of the next frame.  Keeping a larger safety
             * margin below the congestion point is less bursty than waiting
             * for the buffer to nearly fill. */
            while (bt_pcm_fill_bytes() >= stall_limit &&
                   s_mp3.active && !s_mp3.paused && s_bt.connected) {
                vTaskDelay(pdMS_TO_TICKS(8));
            }
            bt_pcm_write_resampled_44k(pcm,
                                       (size_t)fi.outputSamps,
                                       fi.nChans,
                                       fi.samprate,
                                       s_mp3.volume);
            speaker_path_ready = false;
            speaker_last_rate = 0;
            size_t bt_fill = bt_pcm_fill_bytes();
            if (bt_fill > tel_bt_fill_peak) tel_bt_fill_peak = bt_fill;
            int64_t tel_now_us = esp_timer_get_time();
            if ((tel_now_us - tel_window_start_us) >= 5000000LL) {
                bt_tel_snapshot_and_reset(&tel_bt_drop_write_bytes,
                                          &tel_bt_trim_write_bytes,
                                          &tel_bt_trim_read_bytes,
                                          &tel_bt_pad_silence_bytes);
                ESP_LOGI(TAG,
                         "mp3 tel(core2 bt): sr=%d ch=%d dec=%u uf=%u err=%u refill=%u/%uB bt_fill(cur/peak)=%u/%u bt_bytes(drop/trim_w/trim_r/pad)=%u/%u/%u/%u",
                         tel_last_rate,
                         tel_last_channels,
                         tel_decoded_frames,
                         tel_underflows,
                         tel_decode_errors,
                         tel_refills,
                         (unsigned int)tel_refill_bytes,
                         (unsigned int)bt_fill,
                         (unsigned int)tel_bt_fill_peak,
                         (unsigned int)tel_bt_drop_write_bytes,
                         (unsigned int)tel_bt_trim_write_bytes,
                         (unsigned int)tel_bt_trim_read_bytes,
                         (unsigned int)tel_bt_pad_silence_bytes);

                tel_window_start_us = tel_now_us;
                tel_decoded_frames = 0;
                tel_underflows = 0;
                tel_decode_errors = 0;
                tel_refills = 0;
                tel_refill_bytes = 0;
                tel_refill_us_total = 0;
                tel_refill_us_max = 0;
                tel_decode_us_total = 0;
                tel_decode_us_max = 0;
                tel_output_us_total = 0;
                tel_output_us_max = 0;
                tel_output_calls = 0;
                tel_audio_ms = 0;
                tel_bt_fill_peak = bt_fill;
            }
            if (prime_pending) {
                TickType_t now = xTaskGetTickCount();
                if (bt_fill >= s_bt_media_prime_target_bytes ||
                    (s_bt_media_prime_deadline != 0 && now >= s_bt_media_prime_deadline)) {
                    bt_media_start_if_needed();
                    s_bt_media_prime_pending = false;
                    s_bt_media_prime_deadline = 0;
                    s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
                    ESP_LOGI(TAG, "bt: media start after prime fill=%u",
                             (unsigned int)bt_fill);
                }
            } else {
                while (bt_fill > BT_PCM_TARGET_FILL_BYTES &&
                       s_mp3.active && !s_mp3.paused && s_bt.connected) {
                    vTaskDelay(pdMS_TO_TICKS(8));
                    bt_fill = bt_pcm_fill_bytes();
                }
            }
        } else if (s_bt_hold_local_speaker || s_bt.connecting || s_bt.discovering || s_bt_pending_reconnect) {
            /* Keep local I2S path down while BT stack is starting/pairing/retrying. */
            speaker_path_ready = false;
            speaker_last_rate = 0;
            vTaskDelay(pdMS_TO_TICKS(8));
        } else if (s_bt_speaker_resume_after != 0 &&
                   (int32_t)(xTaskGetTickCount() - s_bt_speaker_resume_after) < 0) {
            speaker_path_ready = false;
            speaker_last_rate = 0;
            vTaskDelay(pdMS_TO_TICKS(8));
        } else {
            esp_err_t speaker_err = core2_audio_init();
            if (speaker_err != ESP_OK) {
                ESP_LOGW(TAG, "bt: speaker init deferred: %s", esp_err_to_name(speaker_err));
                bt_log_heap_snapshot("speaker_init_deferred", speaker_err);
                speaker_path_ready = false;
                speaker_last_rate = 0;
                uint32_t cooldown_ms = BT_SPEAKER_REINIT_BACKOFF_MIN_MS;
                if (speaker_err == ESP_ERR_NO_MEM) {
                    cooldown_ms = s_bt_speaker_resume_backoff_ms;
                    if (s_bt_speaker_resume_backoff_ms < BT_SPEAKER_REINIT_BACKOFF_MAX_MS) {
                        s_bt_speaker_resume_backoff_ms *= 2;
                        if (s_bt_speaker_resume_backoff_ms > BT_SPEAKER_REINIT_BACKOFF_MAX_MS) {
                            s_bt_speaker_resume_backoff_ms = BT_SPEAKER_REINIT_BACKOFF_MAX_MS;
                        }
                    }
                } else {
                    s_bt_speaker_resume_backoff_ms = BT_SPEAKER_REINIT_BACKOFF_MIN_MS;
                }
                s_bt_speaker_resume_after = xTaskGetTickCount() + pdMS_TO_TICKS(cooldown_ms);
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            s_bt_speaker_resume_after = 0;
            s_bt_speaker_resume_backoff_ms = BT_SPEAKER_REINIT_BACKOFF_MIN_MS;
            if (!speaker_path_ready) {
                speaker_path_ready = true;
                speaker_last_rate = 0;
            }
            if (speaker_last_rate != (uint32_t)fi.samprate) {
                core2_audio_set_sample_rate((uint32_t)fi.samprate);
                speaker_last_rate = (uint32_t)fi.samprate;
            }
            core2_audio_write_pcm(pcm, (size_t)fi.outputSamps, fi.nChans, s_mp3.volume);
        }
#else
        uint64_t output_us = (uint64_t)(esp_timer_get_time() - output_t0);
        tel_output_calls++;
        tel_output_us_total += output_us;
        if (output_us > tel_output_us_max) tel_output_us_max = output_us;

        uint32_t mono_samples = (uint32_t)(fi.outputSamps / fi.nChans);
        uint32_t frame_ms = (uint32_t)(((uint64_t)mono_samples * 1000ULL) /
                                       (uint64_t)fi.samprate);
        if (frame_ms == 0) frame_ms = 1;
        tel_audio_ms += frame_ms;

        if (!s_mp3.active || s_mp3.paused || opened_token != s_mp3.play_token) {
            continue;
        }

        if (s_mp3.position_ms + frame_ms >= s_mp3.duration_ms) {
            if (!mp3_handle_track_end()) {
                s_mp3.position_ms = s_mp3.duration_ms;
                s_mp3.paused = true;
                mp3_request_ui_refresh();
            }
        } else {
            s_mp3.position_ms += frame_ms;
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - s_mp3_last_ui_tick) >= pdMS_TO_TICKS(3000)) {
            s_mp3_last_ui_tick = now;
            mp3_request_ui_refresh();
        }

        int64_t tel_now_us = esp_timer_get_time();
        if ((tel_now_us - tel_window_start_us) >= 5000000LL) {
            uint32_t win_ms = (uint32_t)((tel_now_us - tel_window_start_us) / 1000LL);
            uint64_t decode_avg_us = tel_decoded_frames ? (tel_decode_us_total / tel_decoded_frames) : 0;
            uint64_t output_avg_us = tel_output_calls ? (tel_output_us_total / tel_output_calls) : 0;
            uint64_t refill_avg_us = tel_refills ? (tel_refill_us_total / tel_refills) : 0;
#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
            size_t bt_fill = s_bt.connected ? bt_pcm_fill_bytes() : 0;
            bt_tel_snapshot_and_reset(&tel_bt_drop_write_bytes,
                                      &tel_bt_trim_write_bytes,
                                      &tel_bt_trim_read_bytes,
                                      &tel_bt_pad_silence_bytes);
#else
            size_t bt_fill = 0;
#endif
            ESP_LOGI(TAG,
                     "mp3 tel: mode=%s sr=%d ch=%d win=%ums audio=%ums dec=%u uf=%u err=%u in=%d refill=%u/%uB refill_us(avg/max)=%llu/%llu dec_us(avg/max)=%llu/%llu out_us(avg/max)=%llu/%llu bt_fill(cur/peak)=%u/%u bt_bytes(drop/trim_w/trim_r/pad)=%u/%u/%u/%u",
#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
                     s_bt.connected ? "bt" : ((s_bt_hold_local_speaker || s_bt.connecting || s_bt.discovering) ? "bt-handoff" : "speaker"),
#else
                     "speaker",
#endif
                     tel_last_rate,
                     tel_last_channels,
                     win_ms,
                     tel_audio_ms,
                     tel_decoded_frames,
                     tel_underflows,
                     tel_decode_errors,
                     bytes_left,
                     tel_refills,
                     (unsigned int)tel_refill_bytes,
                     (unsigned long long)refill_avg_us,
                     (unsigned long long)tel_refill_us_max,
                     (unsigned long long)decode_avg_us,
                     (unsigned long long)tel_decode_us_max,
                     (unsigned long long)output_avg_us,
                     (unsigned long long)tel_output_us_max,
                     (unsigned int)bt_fill,
                     (unsigned int)tel_bt_fill_peak,
                     (unsigned int)tel_bt_drop_write_bytes,
                     (unsigned int)tel_bt_trim_write_bytes,
                     (unsigned int)tel_bt_trim_read_bytes,
                     (unsigned int)tel_bt_pad_silence_bytes);

            tel_window_start_us = tel_now_us;
            tel_decoded_frames = 0;
            tel_underflows = 0;
            tel_decode_errors = 0;
            tel_refills = 0;
            tel_refill_bytes = 0;
            tel_refill_us_total = 0;
            tel_refill_us_max = 0;
            tel_decode_us_total = 0;
            tel_decode_us_max = 0;
            tel_output_us_total = 0;
            tel_output_us_max = 0;
            tel_output_calls = 0;
            tel_audio_ms = 0;
            tel_bt_fill_peak = bt_fill;
            tel_bt_drop_write_bytes = 0;
            tel_bt_trim_write_bytes = 0;
            tel_bt_trim_read_bytes = 0;
            tel_bt_pad_silence_bytes = 0;
        }

#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
        if (s_bt.connected) {
            size_t bt_fill = bt_pcm_fill_bytes();
            /* Let the decoder build some headroom; only yield once the ring is healthy. */
            if (bt_fill >= BT_PCM_HIGH_WATER_BYTES) {
                vTaskDelay(pdMS_TO_TICKS((frame_ms > 3U) ? 3U : frame_ms));
            } else if (bt_fill <= BT_PCM_LOW_WATER_BYTES) {
                taskYIELD();
            }
        }
#endif
#endif
#endif
    }

    MP3FreeDecoder(dec);
    free(inbuf);
    free(pcm);
}

static void mp3_ensure_task(void)
{
    if (s_mp3_task) return;
#if CONFIG_HARDWARE_CORE2
    (void)core2_audio_init();
#endif
    xTaskCreate(mp3_player_task, "mp3_play", 6144, NULL, 5, &s_mp3_task);
}

static void mp3_format_time(uint32_t ms, char *out, size_t out_sz)
{
    uint32_t sec = ms / 1000U;
    uint32_t m = sec / 60U;
    uint32_t s = sec % 60U;
    snprintf(out, out_sz, "%02u:%02u", (unsigned)m, (unsigned)s);
}

static void mp3_render_now_playing(void)
{
    if (!s_mp3.active) return;

    char bar[21];
    int width = 20;
    int filled = 0;
    if (s_mp3.duration_ms > 0) {
        filled = (int)((((uint64_t)s_mp3.position_ms) * (uint64_t)width) / s_mp3.duration_ms);
        if (filled < 0) filled = 0;
        if (filled > width) filled = width;
    }
    for (int i = 0; i < width; i++) bar[i] = (i < filled) ? '#' : '-';
    bar[width] = '\0';

    char cur[8];
    char total[8];
    mp3_format_time(s_mp3.position_ms, cur, sizeof(cur));
    mp3_format_time(s_mp3.duration_ms, total, sizeof(total));

    char msg[700];
    snprintf(msg, sizeof(msg),
             "MP3 %s\n"
             "Folder: %s\n"
             "File: %s\n"
             "[%s]\n"
             "%s / %s\n"
             "Vol:%d Shuffle:%s\n"
             "Repeat Track:%s Repeat Playlist:%s",
             s_mp3.paused ? "Paused" : "Playing",
             s_mp3.folder_name[0] ? s_mp3.folder_name : "(none)",
             s_mp3.file_name[0] ? s_mp3.file_name : "(none)",
             bar,
             cur,
             total,
             s_mp3.volume,
             s_mp3.shuffle ? "on" : "off",
             s_mp3.repeat_track ? "on" : "off",
             s_mp3.repeat_playlist ? "on" : "off");
    screen_draw_text(msg);
}

static bool mp3_start_track(int folder_idx, int track_idx, bool keep_position)
{
    if (folder_idx < 0 || (size_t)folder_idx >= s_mp3_folder_count) return false;
    const mp3_folder_t *folder = &s_mp3_folders[folder_idx];
    if (folder->mp3_count <= 0) return false;

    int resolved_idx = track_idx;
    if (resolved_idx < 0 || resolved_idx >= folder->mp3_count) {
        resolved_idx = s_mp3.shuffle ? (int)(esp_random() % (uint32_t)folder->mp3_count) : 0;
    }

    char file_name[MP3_MAX_FILE_LEN] = {0};
    int total = 0;
    if (!mp3_get_nth_file(folder->folder_path, resolved_idx, file_name, sizeof(file_name), &total) || total <= 0) {
        return false;
    }

    s_mp3.active = true;
    s_mp3.paused = false;
    s_mp3_resume_on_bt_reconnect = false;
    s_mp3.folder_idx = folder_idx;
    s_mp3.track_idx = resolved_idx;
    int path_n = snprintf(s_mp3.file_path, sizeof(s_mp3.file_path), "%s/%s",
                          folder->folder_path, file_name);
    if (path_n <= 0 || path_n >= (int)sizeof(s_mp3.file_path)) {
        return false;
    }
    /* Keep command handling non-blocking; deep file probe stalls WS callback. */
    s_mp3.duration_ms = 180000;
    s_mp3.position_ms = keep_position ? s_mp3.position_ms : 0;
    s_mp3.last_tick = xTaskGetTickCount();
    s_mp3.play_token++;
#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
    s_bt_resample_phase_q16 = 0;
    s_bt_a2dp_sample_rate = BT_A2DP_TARGET_SAMPLE_RATE;
    if (s_bt.connected) {
        bt_pcm_clear();
        s_bt_media_prime_pending = true;
        s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
        s_bt_media_prime_deadline = xTaskGetTickCount() +
                                    pdMS_TO_TICKS(BT_PCM_START_PRIME_TIMEOUT_MS);
    } else {
        s_bt_media_prime_pending = false;
        s_bt_media_prime_deadline = 0;
        s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
    }
#endif
    strncpy(s_mp3.folder_name, folder->trigger, sizeof(s_mp3.folder_name) - 1);
    s_mp3.folder_name[sizeof(s_mp3.folder_name) - 1] = '\0';
    strncpy(s_mp3.file_name, file_name, sizeof(s_mp3.file_name) - 1);
    s_mp3.file_name[sizeof(s_mp3.file_name) - 1] = '\0';

    ESP_LOGI(TAG, "mp3: folder='%s' file='%s' idx=%d/%d (audio pipeline pending)",
             s_mp3.folder_name, s_mp3.file_name, resolved_idx + 1, folder->mp3_count);

    mp3_request_ui_refresh();
    return true;
}

static bool mp3_advance_track(int step, const char *reason)
{
    if (!s_mp3.active || s_mp3.folder_idx < 0 || (size_t)s_mp3.folder_idx >= s_mp3_folder_count) {
        return false;
    }

    const mp3_folder_t *folder = &s_mp3_folders[s_mp3.folder_idx];
    if (folder->mp3_count <= 0) {
        return false;
    }

    int next_idx = s_mp3.track_idx + step;
    while (next_idx < 0) next_idx += folder->mp3_count;
    while (next_idx >= folder->mp3_count) next_idx -= folder->mp3_count;

    if (!mp3_start_track(s_mp3.folder_idx, next_idx, false)) {
        return false;
    }

    if (reason && reason[0]) {
        ESP_LOGI(TAG, "mp3: %s -> track %d/%d", reason, next_idx + 1, folder->mp3_count);
    }
    return true;
}

static bool mp3_handle_track_end(void)
{
    if (!s_mp3.active || s_mp3.folder_idx < 0 || (size_t)s_mp3.folder_idx >= s_mp3_folder_count) {
        return false;
    }

    const mp3_folder_t *folder = &s_mp3_folders[s_mp3.folder_idx];
    if (folder->mp3_count <= 0) {
        return false;
    }

    if (s_mp3.repeat_track) {
        if (mp3_start_track(s_mp3.folder_idx, s_mp3.track_idx, false)) {
            ESP_LOGI(TAG, "mp3: track ended -> replaying current track %d/%d",
                     s_mp3.track_idx + 1, folder->mp3_count);
            return true;
        }
        return false;
    }

    bool at_last_linear_track = (!s_mp3.shuffle && s_mp3.track_idx >= (folder->mp3_count - 1));
    if (at_last_linear_track && !s_mp3.repeat_playlist) {
        ESP_LOGI(TAG, "mp3: track ended -> end of playlist reached (repeatplaylist=off)");
        return false;
    }

    return mp3_advance_track(1, "track ended");
}

#if CONFIG_HARDWARE_CORE2
#define CORE2_AXP_I2C_NUM   I2C_NUM_0
#define CORE2_AXP_I2C_ADDR  0x34

static esp_err_t core2_axp_read_reg(uint8_t reg, uint8_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    return i2c_master_write_read_device(CORE2_AXP_I2C_NUM,
                                        CORE2_AXP_I2C_ADDR,
                                        &reg, 1,
                                        out, 1,
                                        pdMS_TO_TICKS(30));
}

static esp_err_t core2_axp_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(CORE2_AXP_I2C_NUM,
                                      CORE2_AXP_I2C_ADDR,
                                      buf, sizeof(buf),
                                      pdMS_TO_TICKS(30));
}

static void core2_reassert_sd_power(void)
{
    uint8_t reg = 0;

    /* DC3 backlight/peripheral rail nominal setting used by Core2 PMU init. */
    (void)core2_axp_write_reg(0x27, 0x67);

    /* LDO2 voltage (LCD logic + SD card rail) to 3.3V. */
    if (core2_axp_read_reg(0x28, &reg) == ESP_OK) {
        uint8_t next = (uint8_t)((reg & 0x0F) | 0xF0);
        if (next != reg) {
            (void)core2_axp_write_reg(0x28, next);
        }
    }

    /* Ensure LCD logic/SD rail enabled and vibration rail disabled. */
    if (core2_axp_read_reg(0x12, &reg) == ESP_OK) {
        uint8_t next = (uint8_t)((reg & (uint8_t)~0x0E) | 0x06);
        if (next != reg) {
            (void)core2_axp_write_reg(0x12, next);
        }
    }

    /* Re-assert Core2 GPIO4 function used by LCD reset/peripheral routing. */
    (void)core2_axp_write_reg(0x95, 0x84);

    /* Give rail state a short settle window before card init commands. */
    vTaskDelay(pdMS_TO_TICKS(20));
}

static void core2_cycle_sd_power(void)
{
    uint8_t reg = 0;

    if (core2_axp_read_reg(0x12, &reg) == ESP_OK) {
        /* Briefly drop DC3 + LDO2 so SD rail can reset cleanly. */
        uint8_t off = (uint8_t)(reg & (uint8_t)~0x06);
        if (off != reg) {
            (void)core2_axp_write_reg(0x12, off);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(80));
    core2_reassert_sd_power();
    vTaskDelay(pdMS_TO_TICKS(120));
}
#endif

static int mp3_find_folder_trigger(const char *trigger)
{
    for (size_t i = 0; i < s_mp3_folder_count; i++) {
        if (strcasecmp(trigger, s_mp3_folders[i].trigger) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool mount_sd_card_if_needed(void)
{
    if (s_sd_mounted) return true;

    bool spi_locked = screen_spi_lock(3000);
    if (!spi_locked) {
        ESP_LOGW(TAG, "sd: could not acquire screen SPI lock before mount attempt");
        return false;
    }

    /* Keep LCD and SD deselected while probing so only one device can drive MISO. */
    gpio_set_direction(GPIO_NUM_5, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_5, 1);
    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_4, 1);

    /* Core2 wiring can be marginal on these lines; enable internal pull-ups where supported. */
    gpio_pullup_en(GPIO_NUM_4);
    gpio_pullup_en(GPIO_NUM_18);
    gpio_pullup_en(GPIO_NUM_23);
    gpio_set_drive_capability(GPIO_NUM_4, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(GPIO_NUM_18, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(GPIO_NUM_23, GPIO_DRIVE_CAP_3);
    vTaskDelay(pdMS_TO_TICKS(5));

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    const spi_host_device_t host_candidates[] = {SPI3_HOST};
    const int freq_candidates_khz[] = {4000, 1000, 400, 200};
    esp_err_t err = ESP_FAIL;
#if CONFIG_HARDWARE_CORE2
    const int power_passes = 2;
#else
    const int power_passes = 1;
#endif

    for (int pass = 0; pass < power_passes && err != ESP_OK; pass++) {
#if CONFIG_HARDWARE_CORE2
        if (pass == 0) {
            core2_reassert_sd_power();
        } else {
            ESP_LOGW(TAG, "sd: first mount sweep failed; cycling Core2 SD rail and retrying");
            core2_cycle_sd_power();
        }
#endif

        for (size_t h = 0; h < sizeof(host_candidates) / sizeof(host_candidates[0]) && err != ESP_OK; h++) {
            sdmmc_host_t host = SDSPI_HOST_DEFAULT();
            host.slot = host_candidates[h];

            sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
            slot_config.host_id = host_candidates[h];
            slot_config.gpio_cs = GPIO_NUM_4;
            slot_config.gpio_cd = GPIO_NUM_NC;
            slot_config.gpio_wp = GPIO_NUM_NC;
            slot_config.gpio_int = GPIO_NUM_NC;

            for (size_t i = 0; i < sizeof(freq_candidates_khz) / sizeof(freq_candidates_khz[0]); i++) {
                host.max_freq_khz = freq_candidates_khz[i];
                err = esp_vfs_fat_sdspi_mount(MP3_ROOT_PATH, &host, &slot_config,
                                              &mount_config, &s_sd_card);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "sd: mounted using host=%s freq=%d kHz",
                             host_candidates[h] == SPI3_HOST ? "SPI3" : "SPI2",
                             freq_candidates_khz[i]);
                    break;
                }

                ESP_LOGW(TAG, "sd: mount retry failed pass=%d host=%s freq=%d kHz: %s",
                         pass + 1,
                         host_candidates[h] == SPI3_HOST ? "SPI3" : "SPI2",
                         freq_candidates_khz[i], esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(80));
            }
        }
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sd: mount failed at %s: %s", MP3_ROOT_PATH, esp_err_to_name(err));
        s_mp3_next_mount_retry = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
        if (!s_sd_mount_warned) {
            pf_status_draw("SD card timeout\nDevice online\nMP3 unavailable");
            s_sd_mount_warned = true;
        }
        if (spi_locked) {
            screen_spi_unlock();
        }
        return false;
    }

    s_sd_mounted = true;
    s_sd_mount_warned = false;
    s_mp3_next_mount_retry = 0;
    if (spi_locked) {
        screen_spi_unlock();
    }
    ESP_LOGI(TAG, "sd: mounted at %s", MP3_ROOT_PATH);
    return true;
}

static void rebuild_mp3_folder_index(void)
{
    s_mp3_folder_count = 0;

    if (!mount_sd_card_if_needed()) return;

    DIR *d = opendir(MP3_ROOT_PATH);
    if (!d) {
        ESP_LOGW(TAG, "sd: cannot open root %s", MP3_ROOT_PATH);
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (s_mp3_folder_count >= MP3_MAX_FOLDERS) break;
        if (trigger_reserved(e->d_name)) continue;

        char folder_path[MP3_MAX_PATH_LEN];
        int max_name = (int)sizeof(folder_path) - (int)strlen(MP3_ROOT_PATH) - 2;
        if (max_name <= 0) continue;
        int n = snprintf(folder_path, sizeof(folder_path), "%s/%.*s",
                 MP3_ROOT_PATH, max_name, e->d_name);
        if (n <= 0 || n >= (int)sizeof(folder_path)) continue;

        struct stat st;
        if (stat(folder_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        int mp3_count = mp3_count_in_folder(folder_path);
        if (mp3_count <= 0) continue;

        mp3_folder_t *f = &s_mp3_folders[s_mp3_folder_count++];
        strncpy(f->trigger, e->d_name, sizeof(f->trigger) - 1);
        f->trigger[sizeof(f->trigger) - 1] = '\0';
        strncpy(f->folder_path, folder_path, sizeof(f->folder_path) - 1);
        f->folder_path[sizeof(f->folder_path) - 1] = '\0';
        f->mp3_count = mp3_count;
    }
    closedir(d);

    ESP_LOGI(TAG, "mp3: discovered %u folders with mp3 content", (unsigned)s_mp3_folder_count);
}

static esp_err_t save_display_state_to_nvs(void)
{
    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    uint8_t fg_r = 255, fg_g = 255, fg_b = 255;
    bool landscape = false;
    int font_scale = 2;

    screen_get_color(&bg_r, &bg_g, &bg_b);
    screen_get_text_color(&fg_r, &fg_g, &fg_b);
    screen_get_landscape(&landscape);
    screen_get_font_scale(&font_scale);

    esp_err_t err = ESP_OK;
    if ((err = nvs_write_u8(NVS_KEY_BG_R, bg_r)) != ESP_OK) return err;
    if ((err = nvs_write_u8(NVS_KEY_BG_G, bg_g)) != ESP_OK) return err;
    if ((err = nvs_write_u8(NVS_KEY_BG_B, bg_b)) != ESP_OK) return err;
    if ((err = nvs_write_u8(NVS_KEY_FG_R, fg_r)) != ESP_OK) return err;
    if ((err = nvs_write_u8(NVS_KEY_FG_G, fg_g)) != ESP_OK) return err;
    if ((err = nvs_write_u8(NVS_KEY_FG_B, fg_b)) != ESP_OK) return err;
    if ((err = nvs_write_u8(NVS_KEY_ORIENT, landscape ? 1 : 0)) != ESP_OK) return err;
    if ((err = nvs_write_u8(NVS_KEY_FONT, (uint8_t)font_scale)) != ESP_OK) return err;
    if ((err = nvs_write_str(NVS_KEY_TEXT, s_last_text)) != ESP_OK) return err;

    if (s_jpeg_cache && s_jpeg_cache_len > 0 && s_current_jpeg_url[0]) {
        if ((err = nvs_write_str(NVS_KEY_JPEGURL, s_current_jpeg_url)) != ESP_OK) return err;
    } else {
        if ((err = nvs_erase_key_local(NVS_KEY_JPEGURL)) != ESP_OK) return err;
    }

    return nvs_write_u8(NVS_KEY_SAVED, 1);
}

static void restore_display_state_from_nvs(void)
{
    uint8_t saved = 0;
    if (!nvs_read_u8(NVS_KEY_SAVED, &saved) || saved != 1) {
        return;
    }

    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    uint8_t fg_r = 255, fg_g = 255, fg_b = 255;
    uint8_t orient = 0;
    uint8_t font_scale = 2;
    char text[sizeof(s_last_text)] = {0};
    char jpeg_url[sizeof(s_current_jpeg_url)] = {0};

    /* Missing keys fall back to current defaults. */
    screen_get_color(&bg_r, &bg_g, &bg_b);
    screen_get_text_color(&fg_r, &fg_g, &fg_b);
    {
        bool land = false;
        screen_get_landscape(&land);
        orient = land ? 1 : 0;
    }
    {
        int fs = 2;
        screen_get_font_scale(&fs);
        font_scale = (uint8_t)fs;
    }
    nvs_read_u8(NVS_KEY_BG_R, &bg_r);
    nvs_read_u8(NVS_KEY_BG_G, &bg_g);
    nvs_read_u8(NVS_KEY_BG_B, &bg_b);
    nvs_read_u8(NVS_KEY_FG_R, &fg_r);
    nvs_read_u8(NVS_KEY_FG_G, &fg_g);
    nvs_read_u8(NVS_KEY_FG_B, &fg_b);
    nvs_read_u8(NVS_KEY_ORIENT, &orient);
    nvs_read_u8(NVS_KEY_FONT, &font_scale);
    nvs_read_str(NVS_KEY_TEXT, text, sizeof(text));
    nvs_read_str(NVS_KEY_JPEGURL, jpeg_url, sizeof(jpeg_url));

    /* Guard against invisible text when saved fg/bg are identical or near-identical. */
    {
        int dr = (int)fg_r - (int)bg_r; if (dr < 0) dr = -dr;
        int dg = (int)fg_g - (int)bg_g; if (dg < 0) dg = -dg;
        int db = (int)fg_b - (int)bg_b; if (db < 0) db = -db;
        if (dr < 24 && dg < 24 && db < 24) {
            int luma = ((int)bg_r * 299 + (int)bg_g * 587 + (int)bg_b * 114) / 1000;
            if (luma >= 128) {
                fg_r = fg_g = fg_b = 0;
            } else {
                fg_r = fg_g = fg_b = 255;
            }
            ESP_LOGW(TAG, "restore: adjusted text color for contrast");
        }
    }

    if (font_scale < 1) font_scale = 1;
    if (font_scale > 8) font_scale = 8;

    screen_set_landscape(orient != 0);
    screen_set_color(bg_r, bg_g, bg_b);
    screen_set_text_color(fg_r, fg_g, fg_b);
    screen_set_font_scale(font_scale);

    strncpy(s_last_text, text, sizeof(s_last_text) - 1);
    s_last_text[sizeof(s_last_text) - 1] = '\0';

    if (jpeg_url[0]) {
        strncpy(s_current_jpeg_url, jpeg_url, sizeof(s_current_jpeg_url) - 1);
        s_current_jpeg_url[sizeof(s_current_jpeg_url) - 1] = '\0';
        strncpy(s_pending_jpeg_url, jpeg_url, sizeof(s_pending_jpeg_url) - 1);
        s_pending_jpeg_url[sizeof(s_pending_jpeg_url) - 1] = '\0';
        s_pending_jpeg = true;
    } else {
        s_current_jpeg_url[0] = '\0';
        if (s_last_text[0]) {
            screen_draw_text(s_last_text);
        } else {
            strncpy(s_last_text, "Connected!\nWaiting for\ncommands...", sizeof(s_last_text) - 1);
            s_last_text[sizeof(s_last_text) - 1] = '\0';
            screen_draw_text(s_last_text);
        }
    }

    ESP_LOGI(TAG, "Restored saved display state");
}


/*
 * HTTPS GET with Bearer token auth, reads up to 2048 bytes of response body
 * into a malloc'd NUL-terminated buffer (caller must free).
 * Returns body length, or -1 on failure (HTTP error or network problem).
 */
static int https_get_auth(const char *url, const char *token, char **body)
{
    char bearer[560];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);

    *body = NULL;

    esp_http_client_config_t cfg = {
        .url      = url,
        .method   = HTTP_METHOD_GET,
        .timeout_ms = 15000,
    };
    /* TLS: embedded GoDaddy cert for prod; crt_bundle for other HTTPS; plain HTTP needs nothing */
    if (strstr(TCMD_BASE_URL, "triggercmd.com")) {
        cfg.cert_pem = TRIGGERCMD_CA_PEM;
    } else if (strncmp(TCMD_BASE_URL, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_http_client_set_header(client, "Authorization", bearer);

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return -1;
    }

    int64_t cl = esp_http_client_fetch_headers(client);
    int max_body = 2048;
    if (cl > 0 && cl < max_body) max_body = (int)cl;

    char *buf = malloc(max_body + 1);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    int total = 0;
    while (total < max_body) {
        int n = esp_http_client_read(client, buf + total, max_body - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTPS GET %s → HTTP %d", url, status);
        free(buf);
        return -1;
    }

    *body = buf;
    return total;
}

/*
 * HTTPS POST with url-encoded form body and Bearer token auth.
 * body_form is a pre-encoded "key=val&key2=val2" string.
 * Returns HTTP status code, or -1 on network/TLS error.
 * Response body written into malloc'd *resp_body (caller frees), or NULL on failure.
 */
static int https_post_form(const char *url, const char *token,
                           const char *body_form, char **resp_body)
{
    if (resp_body) *resp_body = NULL;

    char bearer[560];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);

    int body_len = (int)strlen(body_form);

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 15000,
    };
    if (strstr(TCMD_BASE_URL, "triggercmd.com")) {
        cfg.cert_pem = TRIGGERCMD_CA_PEM;
    } else if (strncmp(TCMD_BASE_URL, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_http_client_set_header(client, "Authorization", bearer);
    esp_http_client_set_header(client, "Content-Type",
                               "application/x-www-form-urlencoded");

    esp_err_t ret = esp_http_client_open(client, body_len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "https_post_form open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return -1;
    }

    int written = esp_http_client_write(client, body_form, body_len);
    if (written < 0) {
        ESP_LOGE(TAG, "https_post_form write failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    int64_t cl = esp_http_client_fetch_headers(client);
    int max_resp = 2048;
    if (cl > 0 && cl < max_resp) max_resp = (int)cl;

    if (resp_body) {
        char *buf = malloc(max_resp + 1);
        if (buf) {
            int total = 0;
            while (total < max_resp) {
                int n = esp_http_client_read(client, buf + total, max_resp - total);
                if (n <= 0) break;
                total += n;
            }
            buf[total] = '\0';
            *resp_body = buf;
        }
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

/*
 * HTTPS GET without auth headers. Returns body length or -1 on failure.
 * Caller must free *body.
 */
static int https_get_simple(const char *url, char **body)
{
    *body = NULL;

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_GET,
        .timeout_ms = 15000,
    };
    /* TLS: embedded GoDaddy cert for prod; crt_bundle for other HTTPS; plain HTTP needs nothing */
    if (strstr(TCMD_BASE_URL, "triggercmd.com")) {
        cfg.cert_pem = TRIGGERCMD_CA_PEM;
    } else if (strncmp(TCMD_BASE_URL, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "https_get_simple open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return -1;
    }

    int64_t cl = esp_http_client_fetch_headers(client);
    int max_body = 2048;
    if (cl > 0 && cl < max_body) max_body = (int)cl;

    char *buf = malloc(max_body + 1);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    int total = 0;
    while (total < max_body) {
        int n = esp_http_client_read(client, buf + total, max_body - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';

    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "https_get_simple %s → HTTP %d", url, status);
        free(buf);
        return -1;
    }

    *body = buf;
    return total;
}

/* ── Minimal JSON string-value extractor ────────────────────────────────── */
/*
 * Extracts a string value from a flat JSON object for the given key.
 * Handles basic backslash escapes.  Does NOT handle nested objects or
 * arrays inside the target value — which is fine for our use case.
 * Returns true and writes to out if the key is found and value is non-empty.
 */
static bool json_extract_str(const char *json, const char *key,
                              char *out, size_t out_sz)
{
    /* Build search pattern: "key" */
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    /* skip whitespace and ':' */
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return false;
    p++; /* skip opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        if (*p == '\\') {
            p++;
            if (!*p) break;
            switch (*p) {
                case 'n':  out[i++] = '\n'; break;
                case 'r':  out[i++] = '\r'; break;
                case 't':  out[i++] = '\t'; break;
                default:   out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return i > 0;
}

/*
 * Two-step nested JSON extractor: finds "outer":{...} then extracts key
 * inside that block.  e.g. json_extract_nested("...", "data", "id", ...).
 */
static bool json_extract_nested(const char *json, const char *outer,
                                 const char *key, char *out, size_t out_sz)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", outer);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '{') return false;
    /* find closing brace — simple scan, not bracket-aware beyond one level */
    const char *end = strchr(p, '}');
    if (!end) return false;
    /* extract key from the sub-object */
    char sub[256];
    size_t sub_len = (size_t)(end - p + 1);
    if (sub_len >= sizeof(sub)) sub_len = sizeof(sub) - 1;
    memcpy(sub, p, sub_len);
    sub[sub_len] = '\0';
    return json_extract_str(sub, key, out, out_sz);
}

/* ── URL percent-encoding helpers ──────────────────────────────────────── */
/*
 * Append a URL-encoded version of src into dst (remaining capacity rem).
 * Spaces → '+', other non-unreserved bytes → %XX.
 * Returns pointer past the last written byte.
 */
static char *url_encode_append(char *dst, size_t rem, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    while (*src && rem > 1) {
        unsigned char c = (unsigned char)*src++;
        if (c == ' ') {
            if (rem > 1) { *dst++ = '+'; rem--; }
        } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            *dst++ = (char)c; rem--;
        } else {
            if (rem > 3) {
                *dst++ = '%';
                *dst++ = hex[c >> 4];
                *dst++ = hex[c & 0xF];
                rem -= 3;
            }
        }
    }
    return dst;
}

/* ── Embedded command definitions ──────────────────────────────────────── */

typedef struct {
    const char *trigger;
    const char *voice;
    const char *allow_params;   /* "true" or "false" */
    const char *mcp_desc;
    const char *icon;           /* UTF-8 emoji */
} pf_cmd_t;

static const pf_cmd_t s_pf_cmds[] = {
    { "text",      "text",      "true",  "Update the display text. Example: 'text Hello world!'",           "\xF0\x9F\x93\x9D" /* 📝 */ },
    { "color",     "color",     "true",  "Change the display color. Example: 'color red' or 'color #FF0000'", "\xF0\x9F\x94\xA4" /* 🔤 */ },
    { "textcolor", "textcolor", "true",  "Change the text color. Example: 'textcolor blue' or 'textcolor #0000FF'", "\xF0\x9F\x8E\xA8" /* 🎨 */ },
    { "fontsize",  "fontsize",  "true",  "Change the font size (1-4). Example: 'fontsize 3'",                     "\xF0\x9F\x94\xA1" /* 🔡 */ },
    { "landscape", "landscape", "false", "Set the display to landscape orientation.",                        "\xE2\x86\x94\xEF\xB8\x8F" /* ↔️ */ },
    { "portrait",  "portrait",  "false", "Set the display to portrait orientation.",                         "\xE2\x86\x95\xEF\xB8\x8F" /* ↕️ */ },
    { "jpeg",      "jpeg",      "true",  "Display a JPEG image. Example: 'jpeg https://example.com/image.jpg'", "\xF0\x9F\x96\xBC\xEF\xB8\x8F" /* 🖼️ */ },
    { "save",      "save",      "false", "Save the current display settings and image to non-volatile memory.", "\xF0\x9F\x92\xBE" /* 💾 */ },
};
#define PF_CMD_COUNT  (sizeof(s_pf_cmds) / sizeof(s_pf_cmds[0]))

static const pf_cmd_t s_pf_media_cmds[] = {
    { "play",        "play",        "false", "Resume paused MP3 playback.", "\xE2\x96\xB6\xEF\xB8\x8F" /* ▶️ */ },
    { "stop",        "stop",        "false", "Pause MP3 playback at the current position.", "\xE2\x8F\xB8\xEF\xB8\x8F" /* ⏸️ */ },
    { "next",        "next",        "false", "Skip to the next MP3 file in the current folder.", "\xE2\x8F\xA9" /* ⏩ */ },
    { "previous",    "previous",    "false", "Go to the previous MP3 file in the current folder.", "\xE2\x8F\xAA" /* ⏪ */ },
    { "forward",     "forward",     "false", "Skip forward 10 seconds within the current MP3.", "\xE2\x8F\xA9" /* ⏩ */ },
    { "reverse",     "reverse",     "false", "Skip backward 10 seconds within the current MP3.", "\xE2\x8F\xAA" /* ⏪ */ },
    { "volumeup",    "volumeup",    "false", "Increase playback volume.", "\xF0\x9F\x94\x8A" /* 🔊 */ },
    { "volumedown",  "volumedown",  "false", "Decrease playback volume.", "\xF0\x9F\x94\x89" /* 🔉 */ },
    { "shuffle",     "shuffle",     "true",  "Enable or disable shuffle mode. Example: 'shuffle on' or 'shuffle off'", "\xF0\x9F\x94\x80" /* 🔀 */ },
    { "repeattrack", "repeattrack", "true",  "Enable or disable repeat-track mode. Example: 'repeattrack on' or 'repeattrack off'", "\xF0\x9F\x94\x82" /* 🔂 */ },
    { "repeatplaylist", "repeatplaylist", "true",  "Enable or disable repeat-playlist mode. Example: 'repeatplaylist on' or 'repeatplaylist off'", "\xF0\x9F\x94\x81" /* 🔁 */ },
    { "pair",        "pair",        "true",  "Pair with a Bluetooth headset or speaker. Example: 'pair'", "\xF0\x9F\x8E\xA7" /* 🎧 */ },
    { "btstatus",    "btstatus",    "false", "Show Bluetooth audio connection status.", "\xF0\x9F\x93\xB6" /* 📶 */ },
    { "btdisconnect", "btdisconnect", "false", "Disconnect the current Bluetooth audio device.", "\xF0\x9F\x94\x8C" /* 🔌 */ },
    { "btforget",    "btforget",    "false", "Forget the saved Bluetooth device and stop auto-reconnect.", "\xF0\x9F\xA7\xB9" /* 🧹 */ },
};
#define PF_MEDIA_CMD_COUNT (sizeof(s_pf_media_cmds) / sizeof(s_pf_media_cmds[0]))

static bool command_exists_online(const char *list_body, int list_len, const char *trigger)
{
    if (!list_body || list_len <= 0 || !trigger[0]) return false;
    char needle[128];
    snprintf(needle, sizeof(needle), "\"name\":\"%s\"", trigger);
    return strstr(list_body, needle) != NULL;
}

static void sync_command_if_missing(const pf_cmd_t *cmd,
                                    const char *cmd_url,
                                    const char *list_body,
                                    int list_len)
{
    if (!cmd || !cmd->trigger || !cmd->trigger[0]) return;
    if (command_exists_online(list_body, list_len, cmd->trigger)) {
        ESP_LOGI(TAG, "cmd '%s' already online - skip", cmd->trigger);
        return;
    }

    char body[640];
    char *p = body;
    char *bend = body + sizeof(body);

#define APPEND_FIELD_LOCAL(k, v) \
    p = url_encode_append(p, (size_t)(bend - p), (k)); \
    if (p < bend) *p++ = '='; \
    p = url_encode_append(p, (size_t)(bend - p), (v)); \
    if (p < bend) *p++ = '&';

    APPEND_FIELD_LOCAL("name",               cmd->trigger)
    APPEND_FIELD_LOCAL("computer",           s_computer_id)
    APPEND_FIELD_LOCAL("voice",              cmd->voice ? cmd->voice : "")
    APPEND_FIELD_LOCAL("voiceReply",         "")
    APPEND_FIELD_LOCAL("allowParams",        cmd->allow_params ? cmd->allow_params : "false")
    APPEND_FIELD_LOCAL("mcpToolDescription", cmd->mcp_desc ? cmd->mcp_desc : "")
    APPEND_FIELD_LOCAL("icon",               cmd->icon ? cmd->icon : "")

#undef APPEND_FIELD_LOCAL

    if (p > body && *(p - 1) == '&') p--;
    *p = '\0';

    int cs = https_post_form(cmd_url, s_hw_token, body, NULL);
    ESP_LOGI(TAG, "cmd/save '%s' -> HTTP %d", cmd->trigger, cs);
}

/* ── Color parser ──────────────────────────────────────────────────────── */

static bool parse_color(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* skip leading whitespace */
    while (*s == ' ') s++;

    /* Named colours */
    struct { const char *name; uint8_t r, g, b; } named[] = {
        {"red",     255,   0,   0}, {"green",   0, 200,   0},
        {"blue",      0,   0, 255}, {"white",  255, 255, 255},
        {"black",     0,   0,   0}, {"yellow", 255, 255,   0},
        {"cyan",      0, 255, 255}, {"magenta",255,   0, 255},
        {"orange",  255, 165,   0}, {"purple", 128,   0, 128},
        {"pink",    255, 105, 180}, {"gray",   128, 128, 128},
        {"grey",    128, 128, 128},
    };
    for (int i = 0; i < (int)(sizeof(named)/sizeof(named[0])); i++) {
        if (strcasecmp(s, named[i].name) == 0) {
            *r = named[i].r; *g = named[i].g; *b = named[i].b;
            return true;
        }
    }

    /* Hex: #RRGGBB or RRGGBB */
    if (*s == '#') s++;
    if (strlen(s) >= 6) {
        unsigned int rv, gv, bv;
        if (sscanf(s, "%02x%02x%02x", &rv, &gv, &bv) == 3) {
            *r = (uint8_t)rv; *g = (uint8_t)gv; *b = (uint8_t)bv;
            return true;
        }
    }
    return false;
}

/* ── Socket.IO event handler ────────────────────────────────────────────── */

static void pf_event_handler(const char *event_name,
                              const char *payload_json,
                              void       *ctx)
{
    (void)ctx;

    ESP_LOGI(TAG, "pf_event: name='%s' payload=%.200s", event_name, payload_json);

    if (strcmp(event_name, "message") != 0) return;

    static char s_trigger[64];
    static char s_id[32];
    static char s_params[256];

    s_trigger[0] = s_id[0] = s_params[0] = '\0';
    json_extract_str(payload_json, "trigger", s_trigger, sizeof(s_trigger));
    json_extract_str(payload_json, "id",      s_id,      sizeof(s_id));
    json_extract_str(payload_json, "params",  s_params,  sizeof(s_params));

    ESP_LOGI(TAG, "message dispatch: trigger='%s' id='%s' params='%s'",
             s_trigger, s_id, s_params);

    if (strcmp(s_trigger, "text") == 0) {
        /* Discard any cached JPEG so orientation changes redraw text, not image */
        if (s_jpeg_cache) { free(s_jpeg_cache); s_jpeg_cache = NULL; s_jpeg_cache_len = 0; }
        strncpy(s_last_text, s_params[0] ? s_params : " ", sizeof(s_last_text) - 1);
        s_last_text[sizeof(s_last_text) - 1] = '\0';
        s_current_jpeg_url[0] = '\0';
        screen_draw_text(s_last_text);

    } else if (strcmp(s_trigger, "color") == 0) {
        uint8_t r = 0, g = 0, b = 0;
        if (parse_color(s_params, &r, &g, &b)) {
            screen_set_color(r, g, b);
            /* If a JPEG is cached, re-blit it over the new background color */
            if (s_jpeg_cache) s_pending_jpeg_redraw = true;
        } else {
            ESP_LOGW(TAG, "color: unrecognised '%s'", s_params);
        }

    } else if (strcmp(s_trigger, "textcolor") == 0) {
        uint8_t r = 255, g = 255, b = 255;
        if (parse_color(s_params, &r, &g, &b)) {
            screen_set_text_color(r, g, b);
        } else {
            ESP_LOGW(TAG, "textcolor: unrecognised '%s'", s_params);
        }

    } else if (strcmp(s_trigger, "fontsize") == 0) {
        int scale = atoi(s_params);
        if (scale < 1) scale = 1;
        if (scale > 4) scale = 4;
        screen_set_font_scale(scale);

    } else if (strcmp(s_trigger, "landscape") == 0) {
        screen_set_landscape(true);
        if (s_jpeg_cache) s_pending_jpeg_redraw = true;

    } else if (strcmp(s_trigger, "portrait") == 0) {
        screen_set_landscape(false);
        if (s_jpeg_cache) s_pending_jpeg_redraw = true;

    } else if (strcmp(s_trigger, "jpeg") == 0) {
        if (s_params[0]) {
            /* Trim leading whitespace from URL */
            const char *url = s_params;
            while (*url == ' ') url++;
            strncpy(s_pending_jpeg_url, url, sizeof(s_pending_jpeg_url) - 1);
            s_pending_jpeg_url[sizeof(s_pending_jpeg_url) - 1] = '\0';
            s_pending_jpeg = true;
        } else {
            ESP_LOGW(TAG, "jpeg: no URL in params");
            return;  /* don't report run/save for empty command */
        }

    } else if (strcmp(s_trigger, "save") == 0) {
        esp_err_t err = save_display_state_to_nvs();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "save: failed to persist display state: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "save: display state persisted");
        }

    } else if (strcmp(s_trigger, "play") == 0) {
        if (s_mp3.active) {
            s_mp3.paused = false;
            s_mp3_resume_on_bt_reconnect = false;
            s_mp3.last_tick = xTaskGetTickCount();
#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
            if (s_bt.connected) {
                size_t bt_fill = bt_pcm_fill_bytes();
                if (bt_fill >= BT_PCM_RESUME_PRIME_BYTES) {
                    bt_media_start_if_needed();
                    s_bt_media_prime_pending = false;
                    s_bt_media_prime_deadline = 0;
                    s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
                } else {
                    s_bt_media_prime_pending = true;
                    s_bt_media_prime_target_bytes = BT_PCM_RESUME_PRIME_BYTES;
                    s_bt_media_prime_deadline = xTaskGetTickCount() +
                                                pdMS_TO_TICKS(BT_PCM_RESUME_PRIME_TIMEOUT_MS);
                }
            } else {
                s_bt_media_prime_pending = false;
                s_bt_media_prime_deadline = 0;
                s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
            }
#endif
            mp3_request_ui_refresh();
        } else if (s_mp3_folder_count > 0) {
            mp3_start_track(0, -1, false);
        } else {
            screen_draw_text("No MP3 folders\nfound on SD card");
        }

    } else if (strcmp(s_trigger, "stop") == 0) {
        if (s_mp3.active) {
            s_mp3.paused = true;
            s_mp3_resume_on_bt_reconnect = false;
#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
            s_bt_media_prime_pending = false;
            s_bt_media_prime_deadline = 0;
            s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
            bt_media_stop_if_needed();
#endif
            mp3_request_ui_refresh();
        }

    } else if (strcmp(s_trigger, "next") == 0) {
        (void)mp3_advance_track(1, "next command");

    } else if (strcmp(s_trigger, "previous") == 0) {
        (void)mp3_advance_track(-1, "previous command");

    } else if (strcmp(s_trigger, "forward") == 0) {
        (void)mp3_queue_seek_relative((int32_t)MP3_SEEK_STEP_MS, "forward command");

    } else if (strcmp(s_trigger, "reverse") == 0) {
        (void)mp3_queue_seek_relative(-(int32_t)MP3_SEEK_STEP_MS, "reverse command");

    } else if (strcmp(s_trigger, "volumeup") == 0) {
        s_mp3.volume += 5;
        if (s_mp3.volume > 100) s_mp3.volume = 100;
        if (s_mp3.active) mp3_request_ui_refresh();

    } else if (strcmp(s_trigger, "volumedown") == 0) {
        s_mp3.volume -= 5;
        if (s_mp3.volume < 0) s_mp3.volume = 0;
        if (s_mp3.active) mp3_request_ui_refresh();

    } else if (strcmp(s_trigger, "shuffle") == 0) {
        char mode[16] = {0};
        strncpy(mode, s_params, sizeof(mode) - 1);
        for (size_t i = 0; mode[i]; i++) mode[i] = (char)tolower((unsigned char)mode[i]);
        if (strcmp(mode, "on") == 0) {
            s_mp3.shuffle = true;
        } else if (strcmp(mode, "off") == 0) {
            s_mp3.shuffle = false;
        } else {
            s_mp3.shuffle = !s_mp3.shuffle;
        }
        nvs_write_u8(NVS_KEY_SHUFFLE, s_mp3.shuffle ? 1 : 0);
        mp3_log_mode_status("shuffle command");
        if (s_mp3.active) mp3_request_ui_refresh();

    } else if (strcmp(s_trigger, "repeattrack") == 0) {
        char mode[16] = {0};
        strncpy(mode, s_params, sizeof(mode) - 1);
        for (size_t i = 0; mode[i]; i++) mode[i] = (char)tolower((unsigned char)mode[i]);
        if (strcmp(mode, "on") == 0) {
            s_mp3.repeat_track = true;
        } else if (strcmp(mode, "off") == 0) {
            s_mp3.repeat_track = false;
        } else {
            s_mp3.repeat_track = !s_mp3.repeat_track;
        }
        nvs_write_u8(NVS_KEY_REPEAT_TRACK, s_mp3.repeat_track ? 1 : 0);
        mp3_log_mode_status("repeattrack command");
        if (s_mp3.active) mp3_request_ui_refresh();

    } else if (strcmp(s_trigger, "repeatplaylist") == 0) {
        char mode[16] = {0};
        strncpy(mode, s_params, sizeof(mode) - 1);
        for (size_t i = 0; mode[i]; i++) mode[i] = (char)tolower((unsigned char)mode[i]);
        if (strcmp(mode, "on") == 0) {
            s_mp3.repeat_playlist = true;
        } else if (strcmp(mode, "off") == 0) {
            s_mp3.repeat_playlist = false;
        } else {
            s_mp3.repeat_playlist = !s_mp3.repeat_playlist;
        }
        nvs_write_u8(NVS_KEY_REPEAT_PLAYLIST, s_mp3.repeat_playlist ? 1 : 0);
        mp3_log_mode_status("repeatplaylist command");
        if (s_mp3.active) mp3_request_ui_refresh();

    } else if (strcmp(s_trigger, "pair") == 0) {
        bt_cmd_pair_start();

    } else if (strcmp(s_trigger, "btstatus") == 0) {
        bt_cmd_status();

    } else if (strcmp(s_trigger, "btdisconnect") == 0) {
        bt_cmd_disconnect();

    } else if (strcmp(s_trigger, "btforget") == 0) {
        bt_cmd_forget();

    } else {
        int folder_idx = mp3_find_folder_trigger(s_trigger);
        if (folder_idx >= 0) {
            int requested_idx = -1;
            const char *p = s_params;
            while (*p == ' ') p++;
            if (*p >= '0' && *p <= '9') {
                int n = atoi(p);
                if (n >= 1 && n <= 100) {
                    requested_idx = n - 1;
                }
            }
            if (!mp3_start_track(folder_idx, requested_idx, false)) {
                ESP_LOGW(TAG, "mp3: unable to start folder trigger '%s'", s_trigger);
                return;
            }
        } else {
            ESP_LOGW(TAG, "message: unknown trigger '%s'", s_trigger);
            return;
        }
    }

    /* Queue run/save to the main loop — do NOT call https_post_form here.
     * This callback runs in the esp_websocket_client internal task; blocking
     * it with an HTTP request prevents ping/pong processing and causes the
     * server to close the WebSocket connection. */
    if (s_id[0] && s_computer_id[0]) {
        strncpy(s_pending_run_id, s_id, sizeof(s_pending_run_id) - 1);
        s_pending_run_id[sizeof(s_pending_run_id) - 1] = '\0';
        s_pending_run_tries = 0;
        s_pending_run_retry_after = 0;
        s_pending_run = true;
    }
}

/* ── JPEG download + decode + display ───────────────────────────────────── */
/*
 * Downloads a JPEG from url, decodes it to RGB565, and blits it to the
 * screen.  All large allocations use PSRAM so heap fragmentation is avoided.
 * The JPEG input buffer is capped at 512 KB; most photos will be much smaller.
 */
#define JPEG_DL_MAX   (512 * 1024)   /* max JPEG download size */

/*
 * Decode compressed JPEG bytes (buf, len) to RGB565 and blit to the screen.
 * Called both after a fresh download and when re-blitting on orientation change.
 */
static bool decode_and_show_jpeg(const uint8_t *buf, int len)
{
    esp_jpeg_image_cfg_t info_cfg = {
        .indata      = (uint8_t *)buf,
        .indata_size = (uint32_t)len,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_get_image_info(&info_cfg, &info) != ESP_OK || info.output_len == 0) {
        ESP_LOGE(TAG, "jpeg: get_image_info failed");
        screen_draw_text("Image decode\nfailed");
        return false;
    }
    ESP_LOGI(TAG, "jpeg: image %ux%u, output_len=%zu", info.width, info.height, info.output_len);

    uint8_t *rgb565_buf = heap_caps_malloc(info.output_len,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb565_buf) rgb565_buf = malloc(info.output_len);
    if (!rgb565_buf) {
        ESP_LOGE(TAG, "jpeg: no memory for %zu byte RGB565 buffer", info.output_len);
        screen_draw_text("Image decode\nfailed");
        return false;
    }

    esp_jpeg_image_cfg_t dec_cfg = {
        .indata      = (uint8_t *)buf,
        .indata_size = (uint32_t)len,
        .outbuf      = rgb565_buf,
        .outbuf_size = info.output_len,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t out = {0};
    esp_err_t dec_ret = esp_jpeg_decode(&dec_cfg, &out);
    if (dec_ret != ESP_OK) {
        ESP_LOGE(TAG, "jpeg: decode failed: %s", esp_err_to_name(dec_ret));
        free(rgb565_buf);
        screen_draw_text("Image decode\nfailed");
        return false;
    }

    ESP_LOGI(TAG, "jpeg: decoded %ux%u → blitting", out.width, out.height);
    screen_draw_rgb565(rgb565_buf, (int)out.width, (int)out.height);
    free(rgb565_buf);
    return true;
}

static bool download_and_show_jpeg(const char *url)
{
    screen_draw_text("Loading image...");

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_GET,
        .timeout_ms = 15000,
    };
    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "jpeg: http_client_init failed");
        screen_draw_text("Image load\nfailed");
        return false;
    }

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "jpeg: HTTP open failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        screen_draw_text("Image load\nfailed");
        return false;
    }

    int64_t cl = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "jpeg: HTTP %d for %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        screen_draw_text("Image load\nfailed");
        return false;
    }

    int max_dl = JPEG_DL_MAX;
    if (cl > 0 && cl < max_dl) max_dl = (int)cl;

    uint8_t *jpeg_buf = heap_caps_malloc(max_dl, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_buf) {
        /* fall back to internal heap for small content-lengths */
        jpeg_buf = malloc(max_dl);
    }
    if (!jpeg_buf) {
        ESP_LOGE(TAG, "jpeg: no memory for %d byte download", max_dl);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        screen_draw_text("Image load\nfailed");
        return false;
    }

    int total = 0;
    while (total < max_dl) {
        int n = esp_http_client_read(client, (char *)jpeg_buf + total, max_dl - total);
        if (n <= 0) break;
        total += n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total == 0) {
        ESP_LOGE(TAG, "jpeg: empty response from %s", url);
        free(jpeg_buf);
        screen_draw_text("Image load\nfailed");
        return false;
    }
    ESP_LOGI(TAG, "jpeg: downloaded %d bytes", total);

    /* Replace cache — keep compressed bytes for orientation-change redraws */
    if (s_jpeg_cache) { free(s_jpeg_cache); s_jpeg_cache = NULL; }
    s_jpeg_cache     = jpeg_buf;   /* take ownership — do NOT free */
    s_jpeg_cache_len = total;

    if (decode_and_show_jpeg(s_jpeg_cache, s_jpeg_cache_len)) {
        return true;
    }

    return false;
}

/* ── Connection step: Socket.IO + subscribeToFunRoom ────────────────────── */

static esp_err_t connect_and_subscribe(void)
{
    pf_status_draw("Connecting to server...");

    /* __sails_io_sdk_version=0.11.0 in the handshake URL is how Sails 0.12.x
     * identifies a valid sails.io SDK client (checked in parseVirtualRequest). */
    char sio_url[256];
    if (strncmp(TCMD_BASE_URL, "https://", 8) == 0) {
        snprintf(sio_url, sizeof(sio_url),
                 "wss://%s/socket.io/?EIO=4&transport=websocket&__sails_io_sdk_version=0.11.0",
                 TCMD_BASE_URL + 8);
    } else {
        snprintf(sio_url, sizeof(sio_url),
                 "ws://%s/socket.io/?EIO=4&transport=websocket&__sails_io_sdk_version=0.11.0",
                 TCMD_BASE_URL + 7);   /* skip "http://" */
    }

    esp_err_t ret = socketio_connect(sio_url, s_hw_token, pf_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "socketio_connect failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Subscribe via Sails.io virtual GET over the active socket.
     * Append __sails_io_sdk_version to the path (Python client style). */
    char sub_path[192];
    snprintf(sub_path, sizeof(sub_path),
             "/api/computer/subscribeToFunRoom?roomName=%s&__sails_io_sdk_version=0.11.0",
             s_computer_id);
    socketio_send_vget(sub_path, s_hw_token);

    pf_status_draw("Connected!\nWaiting for\ncommands...");
    return ESP_OK;
}

/* ── Main entry point ───────────────────────────────────────────────────── */

void picture_frame_run(void)
{
    /* Initialise display first — screen_init() creates s_draw_mutex which all
     * screen_draw_*() helpers require.  Must happen before any screen call. */
    screen_init();
    ESP_LOGI(TAG, "firmware version %s", g_firmware_version);
    mp3_ensure_task();

    /* ── WiFi ────────────────────────────────────────────────────────────── */
    pf_status_draw("Waiting for WiFi...");

    if (!wifi_has_stored_credentials()) {
#if CONFIG_HARDWARE_CORE2
        pf_softap_provision();   /* never returns — restarts after credentials saved */
#else
        screen_set_color(0, 0, 64);   /* blue = BLE provisioning */
        if (improv_wifi_start() != ESP_OK) {
            screen_set_color(64, 0, 0);
            ESP_LOGE(TAG, "Improv WiFi provisioning failed");
            vTaskSuspend(NULL);
        }
#endif
    }

    if (wifi_connect() != ESP_OK) {
        screen_set_color(64, 0, 0);
        ESP_LOGE(TAG, "WiFi connect failed");
        vTaskSuspend(NULL);
    }

    screen_set_color(0, 32, 0);   /* green = connected */
    vTaskDelay(pdMS_TO_TICKS(500));
    screen_off();

    /* ── NVS: read hw_token and computer_id ─────────────────────────────── */
    bool have_token   = nvs_read_str(NVS_KEY_TOKEN,  s_hw_token,   sizeof(s_hw_token));
    bool have_comp_id = nvs_read_str(NVS_KEY_COMPID, s_computer_id, sizeof(s_computer_id));

    {
        uint8_t shuffle = 0;
        if (nvs_read_u8(NVS_KEY_SHUFFLE, &shuffle)) {
            s_mp3.shuffle = (shuffle != 0);
        }
        uint8_t repeat_track = 0;
        if (nvs_read_u8(NVS_KEY_REPEAT_TRACK, &repeat_track)) {
            s_mp3.repeat_track = (repeat_track != 0);
        }
        uint8_t repeat_playlist = 0;
        if (nvs_read_u8(NVS_KEY_REPEAT_PLAYLIST, &repeat_playlist)) {
            s_mp3.repeat_playlist = (repeat_playlist != 0);
        }
    }
    /* Defer SD mount/index work until the main loop so boot UI is responsive. */
    s_mp3_next_mount_retry = xTaskGetTickCount();

    /* ── Pair code flow — runs until a user JWT is obtained ─────────────── */
    if (!have_token) {
        while (true) {  /* pair_loop — retries on network failure or 10-min timeout */

            char *pair_body = NULL;
            char pair_url[192];
            snprintf(pair_url, sizeof(pair_url),
                     "%s/pair?model=TCMDSCREEN", TCMD_BASE_URL);
            int pair_len = https_get_simple(pair_url, &pair_body);

            if (pair_len <= 0 || !pair_body) {
                ESP_LOGE(TAG, "GET /pair failed — retrying in 10s");
                screen_draw_text("Pairing failed\nRetrying...");
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }

            char pair_code[8]    = {0};
            char pair_token[768] = {0};
            json_extract_str(pair_body, "pairCode",  pair_code,  sizeof(pair_code));
            json_extract_str(pair_body, "pairToken", pair_token, sizeof(pair_token));
            free(pair_body);

            if (pair_code[0] == '\0' || pair_token[0] == '\0') {
                ESP_LOGE(TAG, "GET /pair: missing pairCode/pairToken — retrying in 10s");
                screen_draw_text("Pairing failed\nRetrying...");
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }

            ESP_LOGI(TAG, "Pair code: %s", pair_code);

            char pair_disp[192];
#if CONFIG_HARDWARE_CORE2
            snprintf(pair_disp, sizeof(pair_disp),
                     "Pair code:\n%s", pair_code);
#else
            snprintf(pair_disp, sizeof(pair_disp),
                     "Visit %s\nSign in -> click name\n"
                     "Click Pair -> enter:\n%s", tcmd_display_host(), pair_code);
#endif
            screen_draw_text(pair_disp);
#if !CONFIG_HARDWARE_CORE2
            http_pf_config_start(pair_code);
#endif

            char lookup_url[900];
            snprintf(lookup_url, sizeof(lookup_url),
                     "%s/pair/lookup?token=%s", TCMD_BASE_URL, pair_token);

            bool paired = false;
            for (int i = 0; i < 120 && !paired; i++) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                char *lk_body = NULL;
                int lk_len = https_get_simple(lookup_url, &lk_body);
                if (lk_len > 0 && lk_body) {
                    char tok_new[HW_TOKEN_MAX_LEN] = {0};
                    if (json_extract_str(lk_body, "token", tok_new, sizeof(tok_new)) &&
                            tok_new[0] != '\0') {
                        nvs_write_str(NVS_KEY_TOKEN, tok_new);
                        ESP_LOGI(TAG, "Paired — token saved; rebooting");
                        paired = true;
                    }
                    free(lk_body);
                }
            }

#if !CONFIG_HARDWARE_CORE2
            http_pf_config_stop();
#endif

            if (paired) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }

            ESP_LOGI(TAG, "Pair code timed out — fetching new code");
        }
    }

    /* ── Create computer if not already provisioned ──────────────────────── */
    if (!have_comp_id) {
        /* Build a unique computer name from the WiFi base MAC */
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char computer_name[COMPUTER_NAME_LEN];
        snprintf(computer_name, sizeof(computer_name),
                 "TCMDSCREEN-%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        ESP_LOGI(TAG, "Creating computer: %s", computer_name);
        pf_status_draw("Creating computer...");

        char save_url[192];
        snprintf(save_url, sizeof(save_url), "%s/api/computer/save", TCMD_BASE_URL);

        /* Body: name=TCMDSCREEN-AABBCCDDEEFF (no special encoding needed) */
        char form[64];
        snprintf(form, sizeof(form), "name=%s", computer_name);

        char *resp = NULL;
        int status = https_post_form(save_url, s_hw_token, form, &resp);
        if (status >= 200 && status < 300 && resp) {
            char cid[COMPUTER_ID_MAX_LEN] = {0};
            if (json_extract_nested(resp, "data", "id", cid, sizeof(cid)) && cid[0]) {
                strncpy(s_computer_id, cid, sizeof(s_computer_id) - 1);
                nvs_write_str(NVS_KEY_COMPID, s_computer_id);
                ESP_LOGI(TAG, "computer_id stored: %s", s_computer_id);

                char msg[64];
                snprintf(msg, sizeof(msg), "Ready!\n%s", computer_name);
                screen_draw_text(msg);
            } else {
                ESP_LOGE(TAG, "computer/save: could not parse data.id from: %s", resp);
                free(resp);
                screen_draw_text("Provision failed\nRetrying in 10s");
                vTaskDelay(pdMS_TO_TICKS(10000));
                esp_restart();
            }
        } else {
            ESP_LOGE(TAG, "computer/save → HTTP %d", status);
            if (resp) free(resp);
            screen_draw_text("Provision failed\nRetrying in 10s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            esp_restart();
        }
        if (resp) free(resp);

        vTaskDelay(pdMS_TO_TICKS(1500));

        rebuild_mp3_folder_index();

        /* ── Sync commands ────────────────────────────────────────────────── */
        pf_status_draw("Syncing commands...");

        char list_url[192];
        snprintf(list_url, sizeof(list_url),
                 "%s/api/command/list?computer_id=%s",
                 TCMD_BASE_URL, s_computer_id);

        char *list_body = NULL;
        int list_len = https_get_auth(list_url, s_hw_token, &list_body);
        /* list_body may be NULL if request fails — we'll still attempt adds */

        char cmd_url[192];
        snprintf(cmd_url, sizeof(cmd_url), "%s/api/command/save", TCMD_BASE_URL);

        for (size_t i = 0; i < PF_CMD_COUNT; i++) {
            sync_command_if_missing(&s_pf_cmds[i], cmd_url, list_body, list_len);
        }

        for (size_t i = 0; i < PF_MEDIA_CMD_COUNT; i++) {
            sync_command_if_missing(&s_pf_media_cmds[i], cmd_url, list_body, list_len);
        }

        for (size_t i = 0; i < s_mp3_folder_count; i++) {
            char desc[320];
            snprintf(desc, sizeof(desc),
                     "Play mp3 files in the %s folder. If the parameter is a number from 1 to 100 to specify one of the mp3 files, otherwise, this command will play the first mp3 file, or a random file in the folder if shuffle mode is on.",
                     s_mp3_folders[i].trigger);
            pf_cmd_t dyn_cmd = {
                .trigger = s_mp3_folders[i].trigger,
                .voice = s_mp3_folders[i].trigger,
                .allow_params = "true",
                .mcp_desc = desc,
                .icon = "\xF0\x9F\x8E\xB6", /* 🎶 */
            };
            sync_command_if_missing(&dyn_cmd, cmd_url, list_body, list_len);
        }

        if (list_body) free(list_body);
    }

    /* ── Connect + subscribe loop ────────────────────────────────────────── */
    bool restored_display_state = false;

    while (true) {
        esp_err_t ret = connect_and_subscribe();
        if (ret != ESP_OK) {
            pf_status_draw("Server connect\nfailed\nRetrying in 10s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            socketio_disconnect();
            continue;
        }

        TickType_t last_ping_tick = xTaskGetTickCount();

        if (!restored_display_state) {
            restore_display_state_from_nvs();
            restored_display_state = true;

            /* Defer Classic BT reconnect until after the first Socket.IO/TLS
             * session is established; BT startup can otherwise starve mbedTLS
             * allocation during the reboot path. */
            bt_try_reconnect_on_boot();
        }

        while (true) {
            /* Download + display JPEG from main task — blocking HTTP + decode */
            if (s_pending_jpeg_redraw) {
                s_pending_jpeg_redraw = false;
                if (s_jpeg_cache) {
                    decode_and_show_jpeg(s_jpeg_cache, s_jpeg_cache_len);
                }
            }

            if (s_pending_jpeg) {
                s_pending_jpeg = false;
                char jpeg_url[512];
                strncpy(jpeg_url, s_pending_jpeg_url, sizeof(jpeg_url) - 1);
                jpeg_url[sizeof(jpeg_url) - 1] = '\0';
                if (download_and_show_jpeg(jpeg_url)) {
                    strncpy(s_current_jpeg_url, jpeg_url, sizeof(s_current_jpeg_url) - 1);
                    s_current_jpeg_url[sizeof(s_current_jpeg_url) - 1] = '\0';
                }
            }

            if (s_mp3_ui_pending && s_mp3.active && !s_pending_jpeg && !s_pending_jpeg_redraw) {
                s_mp3_ui_pending = false;
                mp3_render_now_playing();
            } else if (s_mp3_ui_pending && !s_mp3.active) {
                s_mp3_ui_pending = false;
            }

            /* Post run/save from the main task — over existing Socket.IO session
             * so we avoid a second TLS handshake under low-memory conditions. */
            if (s_pending_run &&
                (s_pending_run_retry_after == 0 ||
                 xTaskGetTickCount() >= s_pending_run_retry_after)) {
                char data_json[320];
                snprintf(data_json, sizeof(data_json),
                         "{\"status\":\"Command ran\",\"computer\":\"%s\",\"command\":\"%s\"}",
                         s_computer_id, s_pending_run_id);
                esp_err_t post_err = socketio_send_vpost("/api/run/save", s_hw_token, data_json);
                ESP_LOGI(TAG, "run/save vpost → %s", esp_err_to_name(post_err));

                if (post_err == ESP_OK) {
                    s_pending_run = false;
                    s_pending_run_tries = 0;
                    s_pending_run_retry_after = 0;
                } else {
                    s_pending_run_tries++;
                    if (s_pending_run_tries >= 5) {
                        ESP_LOGE(TAG, "run/save dropped after %d attempts (id=%s)",
                                 s_pending_run_tries, s_pending_run_id);
                        s_pending_run = false;
                        s_pending_run_tries = 0;
                        s_pending_run_retry_after = 0;
                    } else {
                        TickType_t backoff = pdMS_TO_TICKS(300U * (uint32_t)s_pending_run_tries);
                        s_pending_run_retry_after = xTaskGetTickCount() + backoff;
                        ESP_LOGW(TAG, "run/save retry %d scheduled in %lu ms (id=%s)",
                                 s_pending_run_tries,
                                 (unsigned long)(backoff * portTICK_PERIOD_MS),
                                 s_pending_run_id);
                    }
                }
            }

            vTaskDelay(pdMS_TO_TICKS(200));   /* poll every 200 ms */

            if (!s_sd_mounted && s_mp3_next_mount_retry != 0 &&
                xTaskGetTickCount() >= s_mp3_next_mount_retry) {
                ESP_LOGI(TAG, "sd: retrying deferred mount");
                rebuild_mp3_folder_index();
            }

#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
            if (s_bt_pending_reconnect &&
                xTaskGetTickCount() >= s_bt_reconnect_after) {
                s_bt_pending_reconnect = false;
                if (s_bt.has_bda && !s_bt.connected && !s_bt.connecting && !s_bt.discovering) {
                    (void)bt_start_connect_now("retry timer");
                }
            }

            if (s_bt.connecting && !s_bt.connected && s_bt_connect_started_at != 0 &&
                (xTaskGetTickCount() - s_bt_connect_started_at) >=
                    pdMS_TO_TICKS(BT_CONNECT_TIMEOUT_MS)) {
                s_bt.connecting = false;
                s_bt_connect_started_at = 0;
                ESP_LOGW(TAG, "bt: connect timeout after %u ms", (unsigned)BT_CONNECT_TIMEOUT_MS);
                bt_schedule_reconnect("connect timeout", BT_RECONNECT_DELAY_HARD_DROP_MS);
            }
#endif

            /* EIO keepalive: send "2" ping every 20 s so the server doesn't
             * close the socket after its 60-second pingTimeout. */
            if ((xTaskGetTickCount() - last_ping_tick) >= pdMS_TO_TICKS(20000)) {
                socketio_send_eio_ping();
                last_ping_tick = xTaskGetTickCount();
            }

            if (!socketio_connected()) {
                ESP_LOGW(TAG, "Socket.IO disconnected — reconnecting");
                pf_status_draw("Reconnecting...");
                vTaskDelay(pdMS_TO_TICKS(5000));
                socketio_disconnect();
                break;
            }
        }
    }
}
