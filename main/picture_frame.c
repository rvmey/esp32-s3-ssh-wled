/*
 * picture_frame.c
 *
 * TriggerCMD Picture Frame firmware loop for the Guition JC3248W535.
 *
 * Boot sequence:
 *   1. screen_init()
 *   2. WiFi — Improv-WiFi BLE provisioning if no stored credentials.
 *   3. User JWT — obtained via pair code flow:
 *        GET /pair?model=TCMDCORE2 → {pairCode, pairToken}
 *        Display code; poll GET /pair/lookup every 5 s (up to 10 min).
 *        On authorisation, token is saved to NVS and device reboots.
 *        On timeout, a fresh pair code is fetched automatically.
 *   4. Provisioning — POST /api/computer/save with name TCMDCORE2-<MAC>,
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
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <math.h>
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
#include "esp_sntp.h"
#include "mbedtls/base64.h"

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
#include "esp_avrc_api.h"
#endif
#endif

#if CONFIG_ESP_COEX_ENABLED
#include "esp_coexist.h"
#endif

#if CONFIG_HARDWARE_CORE2
/* SoftAP provisioning for Core2 (classic ESP32 — no USB-JTAG Improv) */
#include "core2_audio.h"
#include "core2_mic.h"
#include "core2_adc_mic.h"
#include "mpu6886.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
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
    "<hr style='border-color:#2d3148;margin:1.25rem 0 .5rem'>"
    "<p style='color:#94a3b8;font-size:.85rem;margin:0 0 .25rem'>"
    "Secondary Network (optional)</p>"
    "<label>SSID 2</label><input name='ssid2'>"
    "<label>Password 2</label><input name='pass2' type='password'>"
    "<label>SSID 3</label><input name='ssid3'>"
    "<label>Password 3</label><input name='pass3' type='password'>"
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

    char ssid[64]  = {0};
    char pass[64]  = {0};
    char ssid2[64] = {0};
    char pass2[64] = {0};
    char ssid3[64] = {0};
    char pass3[64] = {0};
    c2_form_get_field(body, "ssid",  ssid,  sizeof(ssid));
    c2_form_get_field(body, "pass",  pass,  sizeof(pass));
    c2_form_get_field(body, "ssid2", ssid2, sizeof(ssid2));
    c2_form_get_field(body, "pass2", pass2, sizeof(pass2));
    c2_form_get_field(body, "ssid3", ssid3, sizeof(ssid3));
    c2_form_get_field(body, "pass3", pass3, sizeof(pass3));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_c2_saved_html, HTTPD_RESP_USE_STRLEN);

    if (ssid[0]) {
        wifi_save_credentials(ssid, pass);
        ESP_LOGI("pf_c2", "WiFi credentials saved for SSID: %s", ssid);
    }
    wifi_save_credentials2(ssid2, pass2);
    if (ssid2[0]) {
        ESP_LOGI("pf_c2", "Secondary WiFi credentials saved for SSID: %s", ssid2);
    }
    wifi_save_credentials3(ssid3, pass3);
    if (ssid3[0]) {
        ESP_LOGI("pf_c2", "Tertiary WiFi credentials saved for SSID: %s", ssid3);
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
#include "core2_leds.h"
#include "socketio.h"
#include "http_pf_config.h"
#include "jpeg_decoder.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "mp3dec.h"

#if CONFIG_HARDWARE_CORE2
#include "core2_audio.h"
#endif

/* True only for the M5Stack Core2, which has the AXP192 PMU, an I2S
 * speaker on GPIO2 (conflicts with CYD's LCD_DC), MPU6886 IMU, and a
 * SK6812 LED bar. CYD reuses Core2's CONFIG_HARDWARE_CORE2 build for
 * UI/command parity but lacks all of this peripheral hardware. */
#define CONFIG_CORE2_HW (CONFIG_HARDWARE_CORE2 && !CONFIG_HARDWARE_CYD)

static const char *TAG = "pf";
extern const char g_firmware_version[];

#define MP3_ROOT_PATH        "/sdcard"
#define SD_SETTINGS_PATH     MP3_ROOT_PATH "/core2_config.txt"
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
    char trigger[MP3_MAX_TRIGGER_LEN];
    char folder_path[MP3_MAX_PATH_LEN];
    int  jpeg_count;
} jpeg_folder_t;

typedef struct {
    bool       active;
    bool       paused;
    bool       shuffle;
    bool       repeat_track;
    bool       repeat_playlist;
    bool       visualizer;
    uint8_t    visualizer_style; /* 1-100, see docs/CORE2_VISUALIZER.md for the full list */
    int        volume;            /* 0..100 */
    bool       muted;            /* toggled by "mute" command; not persisted */
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
#define NVS_KEY_REPEAT_TRACK "mp3_rpt_trk"
#define NVS_KEY_REPEAT_PLAYLIST "mp3_rpt_list"
#define NVS_KEY_VISUALIZER      "mp3_viz"
#define NVS_KEY_VISUALIZER_STYLE "mp3_viz_sty"
#define VISUALIZER_STYLE_MIN 1
#define VISUALIZER_STYLE_MAX 100
#define NVS_KEY_VOLUME  "mp3_volume"
#define NVS_KEY_MP3_MODE "mp3_mode"
#define NVS_KEY_BT_BDA  "bt_bda"
#define NVS_KEY_BT_NAME "bt_name"
#define NVS_KEY_SLEEP_MIN "sleep_min"
#define NVS_KEY_SLEEP_ON_PWR "sleep_on_pwr"
#define NVS_KEY_OPENAI  "stt_key"   /* shared OpenAI key (also used by askpic) */
#define NVS_KEY_AI_TTS  "ai_tts"
#define NVS_KEY_MIC_SRC "mic_src"  /* 0 = built-in PDM (default), 1 = Grove ADC */
#define NVS_KEY_CLOCK_MODE "clock_mode" /* 0=off, 1=digital, 2=analog */
#define NVS_KEY_BOOT_SHOW  "boot_show"  /* 1=clock-digital, 2=clock-analog, 3=music, 4=jpeg, 5=text */
#define NVS_KEY_TIMEZONE   "tz"         /* POSIX TZ string, default "EST5EDT,M3.2.0,M11.1.0" */

#define HW_TOKEN_MAX_LEN    513   /* 512 payload + NUL */
#define COMPUTER_ID_MAX_LEN  33   /* 32 payload + NUL  */
#define COMPUTER_NAME_LEN    32   /* "TCMDCORE2-AABBCCDDEEFF" + NUL */

/* ── Module-level statics shared with event handler ────────────────────── */
static char s_hw_token[HW_TOKEN_MAX_LEN]      __attribute__((unused)) = {0};
static char s_computer_id[COMPUTER_ID_MAX_LEN] __attribute__((unused)) = {0};

#if CONFIG_HARDWARE_CORE2
/* Timestamp of last user activity; reset by touch and received commands.
 * Used by the main loop to trigger deep sleep. */
static TickType_t s_last_activity_tick = 0;
/* Runtime sleep timeout in seconds; 0 = disabled.  Defaults to the Kconfig
 * value but can be overridden at runtime by the "sleeptimer" command. */
static uint32_t   s_sleep_timeout_s    = CONFIG_CORE2_SLEEP_TIMEOUT_S;
#endif
#if CONFIG_CORE2_HW
/* If false (default), the idle-sleep timer is suppressed while running on
 * external power. Toggled by the "sleeponpower" command. */
static bool       s_sleep_while_powered = false;
#endif
#if CONFIG_CORE2_HW
/* Set while the battery readout is on screen; tap refreshes the reading. */
static volatile bool s_battery_display_active = false;

/* On-screen command menu, opened/closed by a long-press anywhere. */
static volatile bool s_menu_active           = false;
static int           s_menu_page             = 0;
static int           s_menu_saved_font_scale = -1;
/* Set by the touch task on tap; consumed by the main task's loop, since
 * pf_menu_execute_item() can run pf_event_handler (SD card I/O, etc.) which
 * needs more stack than touch_poll_task's 3KB. */
static volatile int  s_menu_pending_item = -1;
/* Set by pf_menu_execute_item() for items that draw their own result text
 * (folders, battery, BT status) so pf_menu_close() doesn't immediately
 * overwrite that result with the pre-menu screen contents. */
static bool          s_menu_skip_close_redraw = false;
/* Set while one of those menu-action result screens is on screen, so the
 * mp3 now-playing UI doesn't immediately redraw over it. Cleared when the
 * next command/tap dismisses the result screen. */
static bool          s_menu_result_active = false;
/* When the "save" result screen forces font scale to 1, this holds the original
 * scale so it can be restored when the result is dismissed. -1 = not overridden. */
static int           s_save_result_saved_font_scale = -1;
#endif
/* Set while a JPEG-folder image is on screen; swipe navigates within the folder. */
static volatile bool s_jpeg_folder_display_active = false;
static int           s_jpeg_folder_display_idx    = -1;
static int           s_jpeg_folder_image_idx      = 0;

#if CONFIG_CORE2_HW
/* Set while the SD-card folder list (from the "folders" command) is on
 * screen; tapping a folder name runs "files" for that folder. Names are
 * stored in the same top-to-bottom order they were drawn, one per line
 * below the "Folders:" header line. */
#define PF_FOLDER_LIST_MAX 16
static volatile bool s_folder_list_display_active = false;
static char          s_folder_list_names[PF_FOLDER_LIST_MAX][MP3_MAX_TRIGGER_LEN];
static int           s_folder_list_count = 0;
/* Set by the touch task on tap; consumed by the main task's loop, since
 * dispatching "files" runs pf_event_handler (SD card I/O) which needs more
 * stack than touch_poll_task's 3KB. -1 means no tap pending. */
static volatile int  s_pending_folder_tap_idx = -1;

/* Set while the SD-card file list (from the "files" command) is on screen;
 * tapping an .mp3 plays it and tapping a .jpg/.jpeg displays it. Names,
 * types, and per-type indices are stored in the same top-to-bottom order
 * they were drawn, one per line below the "<folder>:" header line. */
#define PF_FILE_LIST_MAX 16
typedef enum { PF_FILE_OTHER, PF_FILE_MP3, PF_FILE_JPEG } pf_file_type_t;
static volatile bool  s_file_list_display_active = false;
static char           s_file_list_folder_trigger[MP3_MAX_TRIGGER_LEN];
static char           s_file_list_folder_path[MP3_MAX_PATH_LEN];
static char           s_file_list_names[PF_FILE_LIST_MAX][MP3_MAX_FILE_LEN];
static pf_file_type_t s_file_list_types[PF_FILE_LIST_MAX];
static int            s_file_list_subidx[PF_FILE_LIST_MAX];
static int            s_file_list_count = 0;

/* Font scale saved when entering the folder/file list display, like the
 * menu's own saved scale; -1 means not currently saved. */
static int            s_list_saved_font_scale = -1;
#endif

/* Pending run/save — set by the WS event task, consumed by the main loop */
static char          s_pending_run_id[33]  __attribute__((unused)) = {0};
static char          s_pending_result[512] __attribute__((unused)) = {0};
static volatile bool s_pending_has_result  __attribute__((unused)) = false;
static volatile bool s_pending_run        = false;
static volatile bool s_pending_save       = false;  /* deferred to main loop — NVS+SD writes overflow WS task stack */
static volatile bool s_pending_vibrate    __attribute__((unused)) = false;
static volatile bool s_pending_voice_query __attribute__((unused)) = false;
static volatile bool s_pending_speak       __attribute__((unused)) = false;
static char          s_pending_speak_text[256] __attribute__((unused)) = {0};
static int           s_pending_run_tries  __attribute__((unused)) = 0;
static TickType_t    s_pending_run_retry_after __attribute__((unused)) = 0;

/* Pending "askpic" question — set by the WS event task, consumed by the main loop */
static char          s_pending_ask_text[256]   __attribute__((unused)) = {0};
static char          s_pending_ask_run_id[33]  __attribute__((unused)) = {0};
static volatile bool s_pending_ask_pic         __attribute__((unused)) = false;

/* Pending "askgpt" question — set by the WS event task, consumed by the main loop */
static char          s_pending_askgpt_text[256]   __attribute__((unused)) = {0};
static char          s_pending_askgpt_run_id[33]  __attribute__((unused)) = {0};
static volatile bool s_pending_askgpt             __attribute__((unused)) = false;

/* Pending "backup" — set by the WS event task, consumed by the main loop, which
 * spawns a dedicated task.  The upload of a whole SD card can take minutes, so it
 * must NOT run on the main loop: that would starve the Socket.IO keepalive and
 * run/save acks, making the server drop the connection mid-backup. */
static char          s_pending_backup_run_id[33]  __attribute__((unused)) = {0};
static volatile bool s_pending_backup             __attribute__((unused)) = false;
static volatile bool s_backup_running             __attribute__((unused)) = false;
#if CONFIG_CORE2_HW
/* Whether "askpic" answers are also spoken aloud via TTS. Default on,
 * overridden from NVS at boot, toggled by the "aitts" command. */
static bool          s_ai_tts_enabled = true;
/* Microphone source: false = built-in PDM (SPM1423), true = Grove ADC (MAX4466).
 * Overridden from NVS at boot, toggled by the "micsrc" command. */
static bool          s_mic_src_grove  = false;
#endif

/* Pending AVRCP actions — set by bt_avrc_tg_cb, consumed by the main loop */
#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
static volatile bool  s_avrc_pending_play_pause  = false;
static volatile int   s_avrc_pending_track_step  = 0;   /* +1 next, -1 prev */
static volatile bool  s_avrc_pending_voice       = false;
static TickType_t     s_avrc_play_pressed_tick   = 0;
#define AVRC_VOICE_LONG_PRESS_MS 500U
#endif

/* Pending JPEG URL — set by the WS event task, consumed by the main loop */
static char          s_pending_jpeg_url[512]      __attribute__((unused)) = {0};
static volatile bool s_pending_jpeg               = false;
/* Pending local JPEG file path — set by WS event task, consumed by the main loop */
static char          s_pending_jpeg_file_path[512] __attribute__((unused)) = {0};
static volatile bool s_pending_jpeg_file           = false;

/* Pending display updates — set by WS callback, applied by main loop. */
static char          s_pending_text[512] __attribute__((unused)) = {0};
static volatile bool s_pending_text_draw = false;
static volatile uint8_t s_pending_text_redraw_retries = 0;
static volatile int  s_pending_orientation = -1; /* -1 none, 0 portrait, 1 landscape */
static volatile int  s_pending_font_scale = 0;   /* 0 none, 1..4 valid */
static volatile bool s_pending_bg_color = false;
static uint8_t       s_pending_bg_r = 0, s_pending_bg_g = 0, s_pending_bg_b = 0;
static volatile bool s_pending_fg_color = false;
static uint8_t       s_pending_fg_r = 255, s_pending_fg_g = 255, s_pending_fg_b = 255;

/* Cached compressed JPEG — kept in PSRAM so orientation changes can redraw
 * without re-downloading.  Freed when a non-jpeg command replaces the display. */
static uint8_t      *s_jpeg_cache     __attribute__((unused)) = NULL;
static int           s_jpeg_cache_len __attribute__((unused)) = 0;
static volatile bool s_pending_jpeg_redraw = false;

#if CONFIG_CORE2_HW
/* Decoded RGB565 image kept alongside s_jpeg_cache so pinch-to-zoom can
 * crop/rescale on every touch sample without re-running the JPEG decoder.
 * Freed (and zoom/pan reset) whenever s_jpeg_cache is freed or replaced. */
static uint8_t *s_jpeg_rgb565 = NULL;
static int      s_jpeg_rgb_w  = 0;
static int      s_jpeg_rgb_h  = 0;

/* Pinch-to-zoom view state: zoom 1.0 = fit-to-screen (no crop); pan_cx/cy
 * are the crop rectangle's centre as a 0..1 fraction of the decoded image. */
#define JPEG_ZOOM_MAX 4.0f
static float s_jpeg_zoom   = 1.0f;
static float s_jpeg_pan_cx = 0.5f;
static float s_jpeg_pan_cy = 0.5f;

/* Pinch gesture baseline, captured on SCREEN_PINCH_BEGIN */
static float s_pinch_base_dist = 0.0f;
static float s_pinch_base_zoom = 1.0f;
static float s_pinch_base_cx   = 0.5f;
static float s_pinch_base_cy   = 0.5f;
static int   s_pinch_base_mx   = 0;
static int   s_pinch_base_my   = 0;

/* Double-tap detection for zoom reset */
#define JPEG_DOUBLE_TAP_MS   400
#define JPEG_DOUBLE_TAP_DIST 30
static TickType_t s_jpeg_last_tap_tick = 0;
static int        s_jpeg_last_tap_x    = 0;
static int        s_jpeg_last_tap_y    = 0;
#endif /* CONFIG_CORE2_HW */

/* Persistable display state (set by commands, committed by 'save'). */
static char s_last_text[512]         __attribute__((unused)) = {0};
static char s_current_jpeg_url[512]  __attribute__((unused)) = {0};

static sdmmc_card_t *s_sd_card __attribute__((unused)) = NULL;
static bool          s_sd_mounted __attribute__((unused)) = false;

/* Set when secrets_config.txt contains secrets_in_sd=1.  Secrets read from secrets_config.txt
 * are kept in these vars and used directly; they are NOT written to NVS. */
static bool s_sd_secrets_only = false;
static char s_sd_wifi_ssid[3][64]  = {{0}};
static char s_sd_wifi_pass[3][128] = {{0}};
static int  s_sd_wifi_count = 0;
#if CONFIG_CORE2_HW
static char s_sd_openai_key[256] = {0};
#endif
static mp3_folder_t  s_mp3_folders[MP3_MAX_FOLDERS] __attribute__((unused));
static size_t        s_mp3_folder_count __attribute__((unused)) = 0;
static jpeg_folder_t s_jpeg_folders[MP3_MAX_FOLDERS] __attribute__((unused));
static size_t        s_jpeg_folder_count __attribute__((unused)) = 0;
static mp3_state_t   s_mp3 = {
    .active = false,
    .paused = false,
    .shuffle = false,
    .repeat_track = false,
    .repeat_playlist = false,
    .visualizer_style = 1,
    .volume = 50,
    .folder_idx = -1,
    .track_idx = -1,
    .duration_ms = 0,
    .position_ms = 0,
    .last_tick = 0,
};
static volatile bool s_mp3_resume_on_bt_reconnect = false;
static TickType_t s_mp3_last_ui_tick = 0;
static volatile bool s_mp3_ui_pending = false;
static bool          s_mp3_ui_override_allowed = true;
static int           s_mp3_saved_font_scale = -1; /* font scale before music UI; -1 = not saved */
static TaskHandle_t s_mp3_task = NULL;
static TickType_t s_mp3_next_mount_retry __attribute__((unused)) = 0;
static bool s_mp3_autostart = false;
static bool s_sd_mount_warned __attribute__((unused)) = false;
static volatile int32_t s_mp3_seek_target_ms = -1;

/* ── On-screen clock — live digital/analog clock view ───────────────────
 * "clock digital" or "clock analog" takes over the display with a
 * once-per-second live clock until any other command/touch interaction
 * runs. Default style (no/unrecognized params) is digital. */
#if CONFIG_CORE2_HW
typedef enum { PF_CLOCK_OFF = 0, PF_CLOCK_DIGITAL, PF_CLOCK_ANALOG } pf_clock_mode_t;
static pf_clock_mode_t s_clock_mode = PF_CLOCK_OFF;
static int             s_clock_last_sec = -1;
static char            s_timezone[64]   = "EST5EDT,M3.2.0,M11.1.0"; /* POSIX TZ, default Eastern */
static void apply_timezone(const char *tz_str);
#endif

/* ── Audio visualizer — Goertzel per-band energy → Core2 side LEDs ─────── */
#if CONFIG_CORE2_HW

#define VIZ_BANDS      10
#define VIZ_BLOCK      256   /* samples per analysis window */

static const float s_viz_freqs[VIZ_BANDS] = {
    60.0f, 120.0f, 250.0f, 500.0f, 1000.0f,
    2000.0f, 4000.0f, 8000.0f, 12000.0f, 16000.0f
};

typedef struct { float coeff; float peak; } viz_band_t;
static viz_band_t s_viz_band[VIZ_BANDS];
static float      s_viz_buf[VIZ_BLOCK];
static int        s_viz_buf_pos = 0;
static int        s_viz_last_fs = 0;

/* Small xorshift PRNG used by the sparkle/confetti visualizer styles. */
static uint32_t s_viz_rand_state = 0x12345678u;
static uint32_t viz_rand(void)
{
    uint32_t x = s_viz_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_viz_rand_state = x;
    return x;
}

/* ── Generic parameterized visualizer engine (styles 21-100) ────────────
 * Each style is a (position pattern, color scheme, FFT reaction, color
 * rotation speed) tuple. viz_render_table_style() turns that tuple plus
 * the current band levels into a CORE2_LED_COUNT*3 RGB frame. */

typedef enum {
    VPOS_BARS_BOTH,   /* zone fill growing inward from both ends (5+5) */
    VPOS_BAR_SINGLE,  /* single bar growing from LED0 toward LED9 */
    VPOS_BLOOM,       /* each side blooms outward from its center LED */
    VPOS_COMET,       /* one lit LED circles the strip */
    VPOS_DARK_COMET,  /* one dark LED circles an otherwise-lit strip */
    VPOS_PULSE,       /* all LEDs pulse together */
    VPOS_SPECTRUM,    /* LED i tracks band i directly */
    VPOS_SPARKLE,     /* random sparkles, spawn rate audio-driven */
    VPOS_CHECKER,     /* even/odd LEDs pulse in opposition */
} viz_pos_t;

typedef enum {
    VCOL_RAINBOW_ROT, /* rotating rainbow gradient across the strip */
    VCOL_VU_RAMP,     /* green/yellow/red ramp by position */
    VCOL_SINGLE_HUE,  /* one base hue, optionally rotating */
    VCOL_TEMP,        /* hue sweeps blue (quiet) to red (loud) */
    VCOL_TWO_TONE,    /* alternates base hue / base hue + 180 */
    VCOL_RANDOM_HUE,  /* random hue per sparkle */
    VCOL_MONO,        /* white, brightness only */
    VCOL_PER_BAND,    /* hue from LED index (red=bass .. violet=treble) */
} viz_col_t;

typedef enum {
    VFFT_OVERALL,   /* loudest of all 10 bands */
    VFFT_BASS,      /* loudest of bands 0-2 (60-250 Hz) */
    VFFT_TREBLE,    /* loudest of bands 7-9 (8-16 kHz) */
    VFFT_MID,       /* loudest of bands 3-6 (500 Hz-4 kHz) */
    VFFT_DOMINANT,  /* level of the single loudest band */
    VFFT_CENTROID,  /* level-weighted average band index, normalized */
    VFFT_BEAT,      /* bass transient/kick envelope */
    VFFT_PEAK,      /* slowly-decaying peak of the overall level */
} viz_fft_t;

typedef enum {
    VROT_NONE,
    VROT_SLOW,   /* ~15 deg/s hue, ~1 LED/s position */
    VROT_MED,    /* ~50 deg/s hue, ~3 LED/s position */
    VROT_FAST,   /* ~120 deg/s hue, ~7 LED/s position */
    VROT_AUDIO,  /* speed scales with the reactive level */
} viz_rot_t;

typedef struct {
    viz_pos_t pos;
    viz_col_t col;
    viz_fft_t fft;
    viz_rot_t rot;
    float     base_hue; /* degrees; used by VCOL_SINGLE_HUE / VCOL_TWO_TONE */
} viz_style_def_t;

/* Styles 21-100: s_viz_table[0] == style 21, s_viz_table[79] == style 100. */
static const viz_style_def_t s_viz_table[80] = {
    /* 21 */ { VPOS_BARS_BOTH,  VCOL_RAINBOW_ROT, VFFT_OVERALL,  VROT_SLOW,  0.0f   },
    /* 22 */ { VPOS_BARS_BOTH,  VCOL_VU_RAMP,     VFFT_BASS,     VROT_NONE,  0.0f   },
    /* 23 */ { VPOS_BARS_BOTH,  VCOL_VU_RAMP,     VFFT_TREBLE,   VROT_NONE,  0.0f   },
    /* 24 */ { VPOS_BARS_BOTH,  VCOL_TEMP,        VFFT_OVERALL,  VROT_NONE,  0.0f   },
    /* 25 */ { VPOS_BARS_BOTH,  VCOL_PER_BAND,    VFFT_OVERALL,  VROT_NONE,  0.0f   },
    /* 26 */ { VPOS_BARS_BOTH,  VCOL_SINGLE_HUE,  VFFT_MID,      VROT_NONE,  240.0f },
    /* 27 */ { VPOS_BARS_BOTH,  VCOL_TWO_TONE,    VFFT_OVERALL,  VROT_MED,   0.0f   },
    /* 28 */ { VPOS_BARS_BOTH,  VCOL_RAINBOW_ROT, VFFT_BEAT,     VROT_FAST,  0.0f   },
    /* 29 */ { VPOS_BARS_BOTH,  VCOL_MONO,        VFFT_PEAK,     VROT_NONE,  0.0f   },
    /* 30 */ { VPOS_BARS_BOTH,  VCOL_VU_RAMP,     VFFT_DOMINANT, VROT_NONE,  0.0f   },
    /* 31 */ { VPOS_BARS_BOTH,  VCOL_MONO,        VFFT_BEAT,     VROT_NONE,  0.0f   },
    /* 32 */ { VPOS_BARS_BOTH,  VCOL_TWO_TONE,    VFFT_CENTROID, VROT_SLOW,  180.0f },

    /* 33 */ { VPOS_BAR_SINGLE, VCOL_RAINBOW_ROT, VFFT_OVERALL,  VROT_SLOW,  0.0f   },
    /* 34 */ { VPOS_BAR_SINGLE, VCOL_VU_RAMP,     VFFT_OVERALL,  VROT_NONE,  0.0f   },
    /* 35 */ { VPOS_BAR_SINGLE, VCOL_TEMP,        VFFT_BASS,     VROT_NONE,  0.0f   },
    /* 36 */ { VPOS_BAR_SINGLE, VCOL_PER_BAND,    VFFT_TREBLE,   VROT_NONE,  0.0f   },
    /* 37 */ { VPOS_BAR_SINGLE, VCOL_SINGLE_HUE,  VFFT_MID,      VROT_NONE,  120.0f },
    /* 38 */ { VPOS_BAR_SINGLE, VCOL_TWO_TONE,    VFFT_BEAT,     VROT_MED,   60.0f  },
    /* 39 */ { VPOS_BAR_SINGLE, VCOL_MONO,        VFFT_PEAK,     VROT_NONE,  0.0f   },
    /* 40 */ { VPOS_BAR_SINGLE, VCOL_RAINBOW_ROT, VFFT_CENTROID, VROT_MED,   0.0f   },
    /* 41 */ { VPOS_BAR_SINGLE, VCOL_VU_RAMP,     VFFT_BASS,     VROT_NONE,  0.0f   },

    /* 42 */ { VPOS_BLOOM,      VCOL_RAINBOW_ROT, VFFT_OVERALL,  VROT_SLOW,  0.0f   },
    /* 43 */ { VPOS_BLOOM,      VCOL_PER_BAND,    VFFT_DOMINANT, VROT_NONE,  0.0f   },
    /* 44 */ { VPOS_BLOOM,      VCOL_TEMP,        VFFT_OVERALL,  VROT_NONE,  0.0f   },
    /* 45 */ { VPOS_BLOOM,      VCOL_SINGLE_HUE,  VFFT_BASS,     VROT_NONE,  300.0f },
    /* 46 */ { VPOS_BLOOM,      VCOL_TWO_TONE,    VFFT_TREBLE,   VROT_MED,   180.0f },
    /* 47 */ { VPOS_BLOOM,      VCOL_VU_RAMP,     VFFT_MID,      VROT_NONE,  0.0f   },
    /* 48 */ { VPOS_BLOOM,      VCOL_MONO,        VFFT_BEAT,     VROT_NONE,  0.0f   },
    /* 49 */ { VPOS_BLOOM,      VCOL_RAINBOW_ROT, VFFT_PEAK,     VROT_FAST,  0.0f   },

    /* 50 */ { VPOS_COMET,      VCOL_RAINBOW_ROT, VFFT_OVERALL,  VROT_SLOW,  0.0f   },
    /* 51 */ { VPOS_COMET,      VCOL_SINGLE_HUE,  VFFT_BASS,     VROT_MED,   180.0f },
    /* 52 */ { VPOS_COMET,      VCOL_SINGLE_HUE,  VFFT_TREBLE,   VROT_MED,   30.0f  },
    /* 53 */ { VPOS_COMET,      VCOL_PER_BAND,    VFFT_CENTROID, VROT_NONE,  0.0f   },
    /* 54 */ { VPOS_COMET,      VCOL_TWO_TONE,    VFFT_BEAT,     VROT_AUDIO, 0.0f   },
    /* 55 */ { VPOS_COMET,      VCOL_MONO,        VFFT_OVERALL,  VROT_FAST,  0.0f   },
    /* 56 */ { VPOS_COMET,      VCOL_VU_RAMP,     VFFT_MID,      VROT_MED,   0.0f   },
    /* 57 */ { VPOS_COMET,      VCOL_TEMP,        VFFT_OVERALL,  VROT_AUDIO, 0.0f   },
    /* 58 */ { VPOS_COMET,      VCOL_TEMP,        VFFT_BEAT,     VROT_MED,   0.0f   },

    /* 59 */ { VPOS_DARK_COMET, VCOL_RAINBOW_ROT, VFFT_OVERALL,  VROT_SLOW,  0.0f   },
    /* 60 */ { VPOS_DARK_COMET, VCOL_SINGLE_HUE,  VFFT_BASS,     VROT_MED,   0.0f   },
    /* 61 */ { VPOS_DARK_COMET, VCOL_SINGLE_HUE,  VFFT_TREBLE,   VROT_MED,   240.0f },
    /* 62 */ { VPOS_DARK_COMET, VCOL_TEMP,        VFFT_OVERALL,  VROT_SLOW,  0.0f   },
    /* 63 */ { VPOS_DARK_COMET, VCOL_PER_BAND,    VFFT_OVERALL,  VROT_MED,   0.0f   },
    /* 64 */ { VPOS_DARK_COMET, VCOL_TWO_TONE,    VFFT_BEAT,     VROT_AUDIO, 60.0f  },
    /* 65 */ { VPOS_DARK_COMET, VCOL_MONO,        VFFT_PEAK,     VROT_FAST,  0.0f   },
    /* 66 */ { VPOS_DARK_COMET, VCOL_VU_RAMP,     VFFT_MID,      VROT_SLOW,  0.0f   },
    /* 67 */ { VPOS_DARK_COMET, VCOL_SINGLE_HUE,  VFFT_DOMINANT, VROT_MED,   120.0f },
    /* 68 */ { VPOS_DARK_COMET, VCOL_RAINBOW_ROT, VFFT_CENTROID, VROT_AUDIO, 0.0f   },
    /* 69 */ { VPOS_DARK_COMET, VCOL_MONO,        VFFT_OVERALL,  VROT_FAST,  0.0f   },
    /* 70 */ { VPOS_DARK_COMET, VCOL_TWO_TONE,    VFFT_OVERALL,  VROT_SLOW,  270.0f },

    /* 71 */ { VPOS_PULSE,      VCOL_SINGLE_HUE,  VFFT_BASS,     VROT_NONE,  0.0f   },
    /* 72 */ { VPOS_PULSE,      VCOL_SINGLE_HUE,  VFFT_TREBLE,   VROT_NONE,  180.0f },
    /* 73 */ { VPOS_PULSE,      VCOL_TEMP,        VFFT_OVERALL,  VROT_NONE,  0.0f   },
    /* 74 */ { VPOS_PULSE,      VCOL_MONO,        VFFT_BEAT,     VROT_NONE,  0.0f   },
    /* 75 */ { VPOS_PULSE,      VCOL_SINGLE_HUE,  VFFT_MID,      VROT_SLOW,  270.0f },
    /* 76 */ { VPOS_PULSE,      VCOL_TWO_TONE,    VFFT_OVERALL,  VROT_MED,   0.0f   },
    /* 77 */ { VPOS_PULSE,      VCOL_PER_BAND,    VFFT_OVERALL,  VROT_NONE,  0.0f   },
    /* 78 */ { VPOS_PULSE,      VCOL_SINGLE_HUE,  VFFT_CENTROID, VROT_AUDIO, 60.0f  },

    /* 79 */ { VPOS_SPECTRUM,   VCOL_PER_BAND,    VFFT_OVERALL,  VROT_NONE,  0.0f   },
    /* 80 */ { VPOS_SPECTRUM,   VCOL_RAINBOW_ROT, VFFT_OVERALL,  VROT_SLOW,  0.0f   },
    /* 81 */ { VPOS_SPECTRUM,   VCOL_TEMP,        VFFT_OVERALL,  VROT_NONE,  0.0f   },
    /* 82 */ { VPOS_SPECTRUM,   VCOL_MONO,        VFFT_OVERALL,  VROT_NONE,  0.0f   },
    /* 83 */ { VPOS_SPECTRUM,   VCOL_TWO_TONE,    VFFT_BEAT,     VROT_NONE,  0.0f   },

    /* 84 */ { VPOS_SPARKLE,    VCOL_RANDOM_HUE,  VFFT_OVERALL,  VROT_NONE,  0.0f   },
    /* 85 */ { VPOS_SPARKLE,    VCOL_MONO,        VFFT_TREBLE,   VROT_NONE,  0.0f   },
    /* 86 */ { VPOS_SPARKLE,    VCOL_SINGLE_HUE,  VFFT_BASS,     VROT_NONE,  0.0f   },
    /* 87 */ { VPOS_SPARKLE,    VCOL_PER_BAND,    VFFT_BEAT,     VROT_NONE,  0.0f   },
    /* 88 */ { VPOS_SPARKLE,    VCOL_TEMP,        VFFT_OVERALL,  VROT_NONE,  0.0f   },
    /* 89 */ { VPOS_SPARKLE,    VCOL_TWO_TONE,    VFFT_MID,      VROT_NONE,  0.0f   },
    /* 90 */ { VPOS_SPARKLE,    VCOL_RANDOM_HUE,  VFFT_PEAK,     VROT_NONE,  0.0f   },
    /* 91 */ { VPOS_SPARKLE,    VCOL_RAINBOW_ROT, VFFT_CENTROID, VROT_MED,   0.0f   },

    /* 92  */ { VPOS_CHECKER,   VCOL_TWO_TONE,    VFFT_OVERALL,  VROT_NONE,  0.0f   },
    /* 93  */ { VPOS_CHECKER,   VCOL_RAINBOW_ROT, VFFT_OVERALL,  VROT_SLOW,  0.0f   },
    /* 94  */ { VPOS_CHECKER,   VCOL_SINGLE_HUE,  VFFT_BASS,     VROT_NONE,  180.0f },
    /* 95  */ { VPOS_CHECKER,   VCOL_SINGLE_HUE,  VFFT_TREBLE,   VROT_NONE,  30.0f  },
    /* 96  */ { VPOS_CHECKER,   VCOL_TEMP,        VFFT_MID,      VROT_NONE,  0.0f   },
    /* 97  */ { VPOS_CHECKER,   VCOL_MONO,        VFFT_BEAT,     VROT_NONE,  0.0f   },
    /* 98  */ { VPOS_CHECKER,   VCOL_PER_BAND,    VFFT_OVERALL,  VROT_NONE,  0.0f   },
    /* 99  */ { VPOS_CHECKER,   VCOL_VU_RAMP,     VFFT_PEAK,     VROT_NONE,  0.0f   },
    /* 100 */ { VPOS_CHECKER,   VCOL_SINGLE_HUE,  VFFT_DOMINANT, VROT_MED,   300.0f },
};

/* Renders styles 21-100. idx = visualizer_style - 21. */
static void viz_render_table_style(int idx, const float levels[VIZ_BANDS],
                                     float v_overall, int v_dom, float v_centroid,
                                     int64_t now_us)
{
    const viz_style_def_t *d = &s_viz_table[idx];

    static float   hue_phase = 0.0f;
    static float   pos_phase = 0.0f;
    static float   beat_avg = 0.0f, beat_flash = 0.0f;
    static float   fft_peak = 0.0f;
    static float   sparkle_bright[CORE2_LED_COUNT] = {0};
    static float   sparkle_hue[CORE2_LED_COUNT] = {0};
    static int64_t last_us = 0;

    float dt = (last_us != 0) ? (float)(now_us - last_us) / 1000000.0f : 0.0f;
    if (dt > 0.25f) dt = 0.25f; /* clamp after long gaps, e.g. a style switch */
    last_us = now_us;

    float bass = 0.0f, mid = 0.0f, treble = 0.0f;
    for (int b = 0; b < 3; b++) if (levels[b] > bass) bass = levels[b];
    for (int b = 3; b < 7; b++) if (levels[b] > mid) mid = levels[b];
    for (int b = 7; b < VIZ_BANDS; b++) if (levels[b] > treble) treble = levels[b];

    if (bass > beat_avg * 1.4f + 0.04f && beat_flash < 0.3f) {
        beat_flash = 1.0f;
    } else if (dt > 0.0f) {
        beat_flash -= dt * 5.0f;
        if (beat_flash < 0.0f) beat_flash = 0.0f;
    }
    beat_avg += (bass - beat_avg) * 0.05f;

    fft_peak *= 0.93f;
    if (v_overall > fft_peak) fft_peak = v_overall;

    float val;
    switch (d->fft) {
        case VFFT_BASS:     val = bass; break;
        case VFFT_TREBLE:   val = treble; break;
        case VFFT_MID:      val = mid; break;
        case VFFT_DOMINANT: val = levels[v_dom]; break;
        case VFFT_CENTROID: val = v_centroid / (float)(VIZ_BANDS - 1); break;
        case VFFT_BEAT:     val = beat_flash; break;
        case VFFT_PEAK:     val = fft_peak; break;
        default:            val = v_overall; break;
    }
    if (val < 0.0f) val = 0.0f;
    if (val > 1.0f) val = 1.0f;

    float rot_rate, pos_rate;
    switch (d->rot) {
        case VROT_SLOW:  rot_rate = 15.0f;  pos_rate = 1.0f; break;
        case VROT_MED:   rot_rate = 50.0f;  pos_rate = 3.0f; break;
        case VROT_FAST:  rot_rate = 120.0f; pos_rate = 7.0f; break;
        case VROT_AUDIO: rot_rate = 20.0f + val * 200.0f; pos_rate = 0.5f + val * 6.0f; break;
        default:         rot_rate = 0.0f;   pos_rate = 1.0f; break;
    }
    hue_phase = fmodf(hue_phase + rot_rate * dt, 360.0f);
    pos_phase = fmodf(pos_phase + pos_rate * dt, (float)CORE2_LED_COUNT);

    float frac[CORE2_LED_COUNT];
    switch (d->pos) {
        case VPOS_BARS_BOTH: {
            int n = (int)(val * 5.0f + 0.5f);
            if (n > 5) n = 5;
            for (int i = 0; i < 5; i++) {
                float f = (i < n) ? 1.0f : 0.0f;
                frac[i] = f; frac[9 - i] = f;
            }
            break;
        }
        case VPOS_BAR_SINGLE: {
            int n = (int)(val * CORE2_LED_COUNT + 0.5f);
            if (n > CORE2_LED_COUNT) n = CORE2_LED_COUNT;
            for (int i = 0; i < CORE2_LED_COUNT; i++) frac[i] = (i < n) ? 1.0f : 0.0f;
            break;
        }
        case VPOS_BLOOM: {
            int n = (int)(val * 3.0f + 0.5f);
            if (n > 3) n = 3;
            for (int i = 0; i < 5; i++) {
                float f = (abs(i - 2) < n) ? 1.0f : 0.0f;
                frac[i] = f; frac[5 + (4 - i)] = f;
            }
            break;
        }
        case VPOS_COMET: {
            int p = (int)(pos_phase + 0.5f) % CORE2_LED_COUNT;
            for (int i = 0; i < CORE2_LED_COUNT; i++) frac[i] = 0.0f;
            frac[p] = 0.3f + 0.7f * val;
            break;
        }
        case VPOS_DARK_COMET: {
            int p = (int)(pos_phase + 0.5f) % CORE2_LED_COUNT;
            for (int i = 0; i < CORE2_LED_COUNT; i++) frac[i] = 0.2f + 0.8f * val;
            frac[p] = 0.0f;
            break;
        }
        case VPOS_PULSE: {
            for (int i = 0; i < CORE2_LED_COUNT; i++) frac[i] = val;
            break;
        }
        case VPOS_SPECTRUM: {
            for (int i = 0; i < CORE2_LED_COUNT; i++) frac[i] = levels[i];
            break;
        }
        case VPOS_SPARKLE: {
            for (int i = 0; i < CORE2_LED_COUNT; i++) sparkle_bright[i] *= 0.90f;
            int spawns = (int)(val * 2.0f + 0.5f);
            for (int n = 0; n < spawns; n++) {
                int sidx = (int)(viz_rand() % CORE2_LED_COUNT);
                sparkle_bright[sidx] = 1.0f;
                sparkle_hue[sidx] = (float)(viz_rand() % 360);
            }
            for (int i = 0; i < CORE2_LED_COUNT; i++) frac[i] = sparkle_bright[i];
            break;
        }
        case VPOS_CHECKER:
        default: {
            for (int i = 0; i < CORE2_LED_COUNT; i++) frac[i] = (i % 2 == 0) ? val : (1.0f - val);
            break;
        }
    }

    uint8_t rgb[CORE2_LED_COUNT * 3];
    for (int i = 0; i < CORE2_LED_COUNT; i++) {
        float level = frac[i];
        if (level < 0.0f) level = 0.0f;
        if (level > 1.0f) level = 1.0f;
        float hue;
        uint8_t *r = &rgb[i * 3 + 0], *g = &rgb[i * 3 + 1], *b = &rgb[i * 3 + 2];
        switch (d->col) {
            case VCOL_RAINBOW_ROT:
                hue = hue_phase + (float)i * 36.0f;
                core2_leds_hsv_to_rgb(hue, 1.0f, level, r, g, b);
                break;
            case VCOL_SINGLE_HUE:
                hue = d->base_hue + hue_phase;
                core2_leds_hsv_to_rgb(hue, 1.0f, level, r, g, b);
                break;
            case VCOL_TEMP:
                hue = 240.0f * (1.0f - level);
                core2_leds_hsv_to_rgb(hue, 1.0f, 0.15f + 0.85f * level, r, g, b);
                break;
            case VCOL_TWO_TONE:
                hue = (i % 2 == 0) ? (d->base_hue + hue_phase) : (d->base_hue + 180.0f + hue_phase);
                core2_leds_hsv_to_rgb(hue, 1.0f, level, r, g, b);
                break;
            case VCOL_PER_BAND:
                hue = (float)i / 9.0f * 270.0f;
                core2_leds_hsv_to_rgb(hue, 1.0f, level, r, g, b);
                break;
            case VCOL_RANDOM_HUE:
                core2_leds_hsv_to_rgb(sparkle_hue[i], 1.0f, level, r, g, b);
                break;
            case VCOL_MONO:
                core2_leds_hsv_to_rgb(0.0f, 0.0f, level, r, g, b);
                break;
            case VCOL_VU_RAMP:
            default: {
                float pf = (float)i / 9.0f;
                uint8_t rr = (pf < 0.6f) ? 0 : 160;
                uint8_t gg = (pf < 0.85f) ? 160 : 0;
                *r = (uint8_t)(rr * level);
                *g = (uint8_t)(gg * level);
                *b = 0;
                break;
            }
        }
    }
    core2_leds_set_pixels_rgb(rgb);
}

static void viz_init_for_rate(int fs)
{
    s_viz_last_fs = fs;
    for (int i = 0; i < VIZ_BANDS; i++) {
        float w = 2.0f * (float)M_PI * s_viz_freqs[i] / (float)fs;
        s_viz_band[i].coeff = 2.0f * cosf(w);
        s_viz_band[i].peak  = 0.0f;
    }
    s_viz_buf_pos = 0;
    ESP_LOGI("viz", "init fs=%d bands=%d block=%d", fs, VIZ_BANDS, VIZ_BLOCK);
}

static void viz_run_block(void)
{
    float levels[VIZ_BANDS];
    for (int b = 0; b < VIZ_BANDS; b++) {
        float coeff = s_viz_band[b].coeff;
        float s1 = 0.0f, s2 = 0.0f;
        for (int n = 0; n < VIZ_BLOCK; n++) {
            float s = s_viz_buf[n] + coeff * s1 - s2;
            s2 = s1;
            s1 = s;
        }
        float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        float mag   = sqrtf(power > 0.0f ? power : 0.0f) * (2.0f / (float)VIZ_BLOCK);
        if (mag > s_viz_band[b].peak) {
            s_viz_band[b].peak = mag;
        } else {
            s_viz_band[b].peak *= 0.88f;
        }
        levels[b] = s_viz_band[b].peak * 5.0f;
        if (levels[b] > 1.0f) levels[b] = 1.0f;
    }

    /* Shared aggregates for styles 7-100 (computed once per block). */
    int64_t now_us = esp_timer_get_time();
    float v_low = 0.0f, v_high = 0.0f;
    for (int b = 0; b < VIZ_BANDS / 2; b++) {
        if (levels[b] > v_low) v_low = levels[b];
    }
    for (int b = VIZ_BANDS / 2; b < VIZ_BANDS; b++) {
        if (levels[b] > v_high) v_high = levels[b];
    }
    float v_overall = (v_low > v_high) ? v_low : v_high;
    int v_dom = 0;
    for (int b = 1; b < VIZ_BANDS; b++) {
        if (levels[b] > levels[v_dom]) v_dom = b;
    }
    float v_centroid_num = 0.0f, v_centroid_den = 0.0f;
    for (int b = 0; b < VIZ_BANDS; b++) {
        v_centroid_num += (float)b * levels[b];
        v_centroid_den += levels[b];
    }
    float v_centroid = (v_centroid_den > 0.001f) ? (v_centroid_num / v_centroid_den) : 4.5f;

    if (s_mp3.visualizer_style == 2) {
        core2_leds_set_bands(levels, VIZ_BANDS);
    } else if (s_mp3.visualizer_style == 3) {
        /* Style 3: chase — single LED steps 0..9, then repeats with the
         * next color in the palette. Paced by wall-clock time, not the
         * (much faster) audio block rate. */
        static const uint8_t chase_colors[][3] = {
            {160, 0,   0  }, /* red */
            {0,   160, 0  }, /* green */
            {0,   0,   160}, /* blue */
            {160, 160, 0  }, /* yellow */
            {0,   160, 160}, /* cyan */
            {160, 0,   160}, /* magenta */
        };
        static int     chase_pos   = -1;
        static int     chase_color = 0;
        static int64_t chase_last_us = 0;

        if (chase_pos < 0 || now_us - chase_last_us >= 100000) {
            chase_last_us = now_us;
            chase_pos++;
            if (chase_pos >= CORE2_LED_COUNT) {
                chase_pos = 0;
                chase_color = (chase_color + 1) %
                              (int)(sizeof(chase_colors) / sizeof(chase_colors[0]));
            }
            core2_leds_set_chase(chase_pos, chase_colors[chase_color][0],
                                  chase_colors[chase_color][1], chase_colors[chase_color][2]);
        }
    } else if (s_mp3.visualizer_style == 4) {
        /* Style 4: mirrored VU meter — both side faces fill outward from the
         * bottom together, driven by the overall loudness (loudest of all
         * 10 bands). 0..5 LEDs lit per side, red, with brightness used to
         * smooth the gradient at the boundary LED. */
        float level = 0.0f;
        for (int b = 0; b < VIZ_BANDS; b++) {
            if (levels[b] > level) level = levels[b];
        }
        core2_leds_set_vu_mirror(level);
    } else if (s_mp3.visualizer_style == 5) {
        /* Style 5: VU-meter bars, bottom-up — same low/high band split as
         * style 1 but fills from the bottom corners toward the top. */
        float low = 0.0f, high = 0.0f;
        for (int b = 0; b < VIZ_BANDS / 2; b++) {
            if (levels[b] > low) low = levels[b];
        }
        for (int b = VIZ_BANDS / 2; b < VIZ_BANDS; b++) {
            if (levels[b] > high) high = levels[b];
        }
        core2_leds_set_vu_bottomup(low, high);
    } else if (s_mp3.visualizer_style == 6) {
        /* Style 6: VU bars, mixed top-down/bottom-up — highs (blue) fill
         * down from the top, lows (red) fill up from the bottom, mixing
         * into magenta in the middle of each side. */
        float low = 0.0f, high = 0.0f;
        for (int b = 0; b < VIZ_BANDS / 2; b++) {
            if (levels[b] > low) low = levels[b];
        }
        for (int b = VIZ_BANDS / 2; b < VIZ_BANDS; b++) {
            if (levels[b] > high) high = levels[b];
        }
        core2_leds_set_vu_mix(low, high);
    } else if (s_mp3.visualizer_style == 7) {
        /* Style 7: rainbow VU bars — same low/high zone fill as style 1
         * (LEDs 0-4 fill from LED0, LEDs 5-9 fill from LED9), but lit LEDs
         * are colored from a slowly rotating rainbow gradient instead of
         * the fixed green/yellow/red ramp. */
        static float hue_phase = 0.0f;
        static int64_t last_us = 0;
        if (last_us != 0) {
            hue_phase += (float)(now_us - last_us) * (60.0f / 1000000.0f); /* 60 deg/sec */
        }
        last_us = now_us;

        const int zone = CORE2_LED_COUNT / 2;
        int low_n  = (int)(v_low  * zone + 0.5f);
        int high_n = (int)(v_high * zone + 0.5f);
        if (low_n  > zone) low_n  = zone;
        if (high_n > zone) high_n = zone;

        uint8_t rgb[CORE2_LED_COUNT * 3] = {0};
        for (int i = 0; i < zone; i++) {
            if (i < low_n) {
                core2_leds_hsv_to_rgb(hue_phase + i * 40.0f, 1.0f, 0.8f,
                                       &rgb[i * 3 + 0], &rgb[i * 3 + 1], &rgb[i * 3 + 2]);
            }
        }
        for (int i = 0; i < zone; i++) {
            int led = CORE2_LED_COUNT - 1 - i;
            if (i < high_n) {
                core2_leds_hsv_to_rgb(hue_phase + (zone + i) * 40.0f, 1.0f, 0.8f,
                                       &rgb[led * 3 + 0], &rgb[led * 3 + 1], &rgb[led * 3 + 2]);
            }
        }
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style == 8) {
        /* Style 8: bass-pulse breathing — all 10 LEDs share one color whose
         * brightness tracks the bass level (60-250 Hz) and whose hue slowly
         * rotates over time. */
        static float hue_phase = 0.0f;
        static int64_t last_us = 0;
        if (last_us != 0) {
            hue_phase += (float)(now_us - last_us) * (20.0f / 1000000.0f); /* 20 deg/sec */
        }
        last_us = now_us;

        float bass = levels[0];
        if (levels[1] > bass) bass = levels[1];
        if (levels[2] > bass) bass = levels[2];

        uint8_t r, g, b;
        core2_leds_hsv_to_rgb(hue_phase, 1.0f, 0.15f + 0.85f * bass, &r, &g, &b);
        uint8_t rgb[CORE2_LED_COUNT * 3];
        for (int i = 0; i < CORE2_LED_COUNT; i++) {
            rgb[i * 3 + 0] = r; rgb[i * 3 + 1] = g; rgb[i * 3 + 2] = b;
        }
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style == 9) {
        /* Style 9: dual mirrored mini-spectrum — each side face is its own
         * 5-band spectrum. Left zone (LED0-4) shows bands 0-4 (bass..mid),
         * right zone (LED5-9) shows bands 9-5 (treble..mid), each band
         * colored by its global hue (red=bass .. violet=treble) with
         * brightness from that band's level. */
        uint8_t rgb[CORE2_LED_COUNT * 3];
        for (int i = 0; i < 5; i++) {
            float hue = (float)i / 9.0f * 270.0f;
            core2_leds_hsv_to_rgb(hue, 1.0f, levels[i],
                                   &rgb[i * 3 + 0], &rgb[i * 3 + 1], &rgb[i * 3 + 2]);
        }
        for (int i = 0; i < 5; i++) {
            int band = VIZ_BANDS - 1 - i; /* 9 .. 5 */
            int led  = 5 + i;             /* LED5 .. LED9 */
            float hue = (float)band / 9.0f * 270.0f;
            core2_leds_hsv_to_rgb(hue, 1.0f, levels[band],
                                   &rgb[led * 3 + 0], &rgb[led * 3 + 1], &rgb[led * 3 + 2]);
        }
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style == 10) {
        /* Style 10: treble sparkle — a dim blue base (brightness from the
         * bass level) with random white sparkles whose count is driven by
         * the treble energy (8-16 kHz bands). */
        float bass = 0.0f;
        for (int b = 0; b < 5; b++) if (levels[b] > bass) bass = levels[b];
        float treble = 0.0f;
        for (int b = 7; b < VIZ_BANDS; b++) if (levels[b] > treble) treble = levels[b];

        uint8_t br, bg, bb;
        core2_leds_hsv_to_rgb(220.0f, 1.0f, 0.08f + 0.12f * bass, &br, &bg, &bb);

        uint8_t rgb[CORE2_LED_COUNT * 3];
        for (int i = 0; i < CORE2_LED_COUNT; i++) {
            rgb[i * 3 + 0] = br; rgb[i * 3 + 1] = bg; rgb[i * 3 + 2] = bb;
        }

        int sparkles = (int)(treble * CORE2_LED_COUNT + 0.5f);
        for (int n = 0; n < sparkles; n++) {
            int idx = (int)(viz_rand() % CORE2_LED_COUNT);
            rgb[idx * 3 + 0] = 200; rgb[idx * 3 + 1] = 200; rgb[idx * 3 + 2] = 200;
        }
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style == 11) {
        /* Style 11: beat-flash strobe — tracks a slow moving average of the
         * bass level (60-250 Hz); when the instantaneous bass level spikes
         * well above that average (a "kick"), flash all LEDs white and let
         * the flash decay over ~200 ms. Otherwise show a dim white glow
         * from the overall level. */
        static float bass_avg = 0.0f;
        static float flash = 0.0f;
        static int64_t last_us = 0;
        float dt = (last_us != 0) ? (float)(now_us - last_us) / 1000000.0f : 0.0f;
        last_us = now_us;

        float bass = 0.0f;
        for (int b = 0; b < 3; b++) if (levels[b] > bass) bass = levels[b];

        if (bass > bass_avg * 1.4f + 0.04f && flash < 0.3f) {
            flash = 1.0f;
        } else if (dt > 0.0f) {
            flash -= dt * 5.0f; /* ~200ms decay */
            if (flash < 0.0f) flash = 0.0f;
        }
        bass_avg += (bass - bass_avg) * 0.05f;

        float val = (flash > v_overall * 0.2f) ? flash : v_overall * 0.2f;
        uint8_t r, g, b;
        core2_leds_hsv_to_rgb(0.0f, 0.0f, val, &r, &g, &b); /* sat=0 -> white */
        uint8_t rgb[CORE2_LED_COUNT * 3];
        for (int i = 0; i < CORE2_LED_COUNT; i++) {
            rgb[i * 3 + 0] = r; rgb[i * 3 + 1] = g; rgb[i * 3 + 2] = b;
        }
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style == 12) {
        /* Style 12: spectral-centroid comet — a bright pixel sweeps along
         * the strip according to the spectral centroid (the level-weighted
         * average band index, 0=bass end .. 9=treble end), leaving a
         * fading trail. Each LED position has a fixed hue (red=bass end ..
         * violet=treble end); brightness comes from the trail decay scaled
         * by the overall level. */
        static float trail[CORE2_LED_COUNT] = {0};

        for (int i = 0; i < CORE2_LED_COUNT; i++) trail[i] *= 0.75f;

        int lo = (int)v_centroid;
        float frac = v_centroid - (float)lo;
        if (lo < 0) lo = 0;
        if (lo > CORE2_LED_COUNT - 1) lo = CORE2_LED_COUNT - 1;
        int hi = lo + 1;
        if (hi > CORE2_LED_COUNT - 1) hi = CORE2_LED_COUNT - 1;

        trail[lo] += (1.0f - frac) * v_overall;
        trail[hi] += frac * v_overall;

        uint8_t rgb[CORE2_LED_COUNT * 3];
        for (int i = 0; i < CORE2_LED_COUNT; i++) {
            float v = trail[i];
            if (v > 1.0f) v = 1.0f;
            float hue = (float)i / 9.0f * 270.0f;
            core2_leds_hsv_to_rgb(hue, 1.0f, v, &rgb[i * 3 + 0], &rgb[i * 3 + 1], &rgb[i * 3 + 2]);
        }
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style == 13) {
        /* Style 13: center bloom — each 5-LED zone blooms outward from its
         * center LED (LED2 left, LED7 right) toward both ends as the
         * overall level rises (0..3 LEDs lit per side of center). The
         * bloom color is the hue of whichever band is currently loudest,
         * so the color shifts with the dominant frequency. */
        int bloom_n = (int)(v_overall * 3.0f + 0.5f); /* 0..3 */
        if (bloom_n > 3) bloom_n = 3;
        float hue = (float)v_dom / 9.0f * 270.0f;

        uint8_t lit_r, lit_g, lit_b;
        core2_leds_hsv_to_rgb(hue, 1.0f, 0.8f, &lit_r, &lit_g, &lit_b);

        uint8_t rgb[CORE2_LED_COUNT * 3] = {0};
        for (int i = 0; i < 5; i++) {
            if (abs(i - 2) < bloom_n) {
                rgb[i * 3 + 0] = lit_r; rgb[i * 3 + 1] = lit_g; rgb[i * 3 + 2] = lit_b;
                int right_led = 5 + (4 - i); /* mirrors left zone about LED7 */
                rgb[right_led * 3 + 0] = lit_r; rgb[right_led * 3 + 1] = lit_g; rgb[right_led * 3 + 2] = lit_b;
            }
        }
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style == 14) {
        /* Style 14: color-temperature VU bars — same corner-fill positions
         * as style 1 (low band fills LEDs 0-4 from LED0, high band fills
         * LEDs 5-9 from LED9), but each zone's lit LEDs share one color
         * whose hue sweeps from blue (quiet) to red (loud) based on that
         * zone's level, instead of a fixed green/yellow/red ramp. */
        const int zone = CORE2_LED_COUNT / 2;
        int low_n  = (int)(v_low  * zone + 0.5f);
        int high_n = (int)(v_high * zone + 0.5f);
        if (low_n  > zone) low_n  = zone;
        if (high_n > zone) high_n = zone;

        uint8_t low_r, low_g, low_b, high_r, high_g, high_b;
        core2_leds_hsv_to_rgb(240.0f * (1.0f - v_low),  1.0f, 0.8f, &low_r,  &low_g,  &low_b);
        core2_leds_hsv_to_rgb(240.0f * (1.0f - v_high), 1.0f, 0.8f, &high_r, &high_g, &high_b);

        uint8_t rgb[CORE2_LED_COUNT * 3] = {0};
        for (int i = 0; i < low_n; i++) {
            rgb[i * 3 + 0] = low_r; rgb[i * 3 + 1] = low_g; rgb[i * 3 + 2] = low_b;
        }
        for (int i = 0; i < high_n; i++) {
            int led = CORE2_LED_COUNT - 1 - i;
            rgb[led * 3 + 0] = high_r; rgb[led * 3 + 1] = high_g; rgb[led * 3 + 2] = high_b;
        }
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style == 15) {
        /* Style 15: confetti — random LEDs flash a random color and fade
         * out; the spawn rate is driven by the overall level (louder music
         * = more frequent sparkles). Paced by wall-clock time (~30ms ticks)
         * so it looks consistent regardless of the audio block rate. */
        static float bright[CORE2_LED_COUNT] = {0};
        static float hue[CORE2_LED_COUNT] = {0};
        static int64_t last_us = 0;

        if (now_us - last_us >= 30000) {
            last_us = now_us;
            for (int i = 0; i < CORE2_LED_COUNT; i++) bright[i] *= 0.85f;

            int spawns = (int)(v_overall * 2.0f + 0.5f);
            for (int n = 0; n < spawns; n++) {
                int idx = (int)(viz_rand() % CORE2_LED_COUNT);
                bright[idx] = 1.0f;
                hue[idx] = (float)(viz_rand() % 360);
            }
        }

        uint8_t rgb[CORE2_LED_COUNT * 3];
        for (int i = 0; i < CORE2_LED_COUNT; i++) {
            core2_leds_hsv_to_rgb(hue[i], 1.0f, bright[i], &rgb[i * 3 + 0], &rgb[i * 3 + 1], &rgb[i * 3 + 2]);
        }
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style == 16) {
        /* Style 16: rainbow wash — a full rainbow gradient continuously
         * rotates across all 10 LEDs (position/color is not audio-driven),
         * while the overall brightness breathes with the music. */
        static float hue_phase = 0.0f;
        static int64_t last_us = 0;
        if (last_us != 0) {
            hue_phase += (float)(now_us - last_us) * (90.0f / 1000000.0f); /* 90 deg/sec */
        }
        last_us = now_us;

        float val = 0.15f + 0.85f * v_overall;
        uint8_t rgb[CORE2_LED_COUNT * 3];
        for (int i = 0; i < CORE2_LED_COUNT; i++) {
            float hue = hue_phase + (float)i * 36.0f;
            core2_leds_hsv_to_rgb(hue, 1.0f, val, &rgb[i * 3 + 0], &rgb[i * 3 + 1], &rgb[i * 3 + 2]);
        }
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style == 17) {
        /* Style 17: peak-hold spectrum — like style 2's per-band spectrum
         * (LED i <-> band i, red=bass .. violet=treble), but each band also
         * tracks a slowly decaying peak so recently-loud bands keep a dim
         * afterglow instead of snapping off instantly. */
        static float peak[VIZ_BANDS] = {0};

        uint8_t rgb[CORE2_LED_COUNT * 3];
        for (int i = 0; i < VIZ_BANDS; i++) {
            peak[i] *= 0.93f;
            if (levels[i] > peak[i]) peak[i] = levels[i];
            float v = levels[i];
            if (peak[i] * 0.6f > v) v = peak[i] * 0.6f;
            float hue = (float)i / 9.0f * 270.0f;
            core2_leds_hsv_to_rgb(hue, 1.0f, v, &rgb[i * 3 + 0], &rgb[i * 3 + 1], &rgb[i * 3 + 2]);
        }
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style == 18) {
        /* Style 18: dominant-band spotlight — the single LED corresponding
         * to the currently loudest frequency band (LED i <-> band i, as in
         * style 2) lights brightly with that band's hue, while every other
         * LED stays at a dim white glow. The spotlight jumps around the
         * strip as the dominant frequency changes. */
        uint8_t rgb[CORE2_LED_COUNT * 3];
        for (int i = 0; i < CORE2_LED_COUNT; i++) {
            core2_leds_hsv_to_rgb(0.0f, 0.0f, 0.04f, &rgb[i * 3 + 0], &rgb[i * 3 + 1], &rgb[i * 3 + 2]);
        }
        float hue = (float)v_dom / 9.0f * 270.0f;
        core2_leds_hsv_to_rgb(hue, 1.0f, 0.3f + 0.7f * levels[v_dom],
                               &rgb[v_dom * 3 + 0], &rgb[v_dom * 3 + 1], &rgb[v_dom * 3 + 2]);
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style == 19) {
        /* Style 19: pulsing VU bars — same low/high zone fill and
         * green/yellow/red ramp as style 1, but the whole strip's
         * brightness additionally breathes with the overall loudness, so
         * quiet passages dim the whole bar and loud passages punch it back
         * up to full brightness. */
        const int zone = CORE2_LED_COUNT / 2;
        int low_n  = (int)(v_low  * zone + 0.5f);
        int high_n = (int)(v_high * zone + 0.5f);
        if (low_n  > zone) low_n  = zone;
        if (high_n > zone) high_n = zone;
        float master = 0.4f + 0.6f * v_overall;

        uint8_t rgb[CORE2_LED_COUNT * 3] = {0};
        for (int i = 0; i < low_n; i++) {
            float frac = (zone <= 1) ? 0.0f : (float)i / (float)(zone - 1);
            uint8_t r = (frac < 0.6f) ? 0 : 160;
            uint8_t g = (frac < 0.85f) ? 160 : 0;
            rgb[i * 3 + 0] = (uint8_t)(r * master);
            rgb[i * 3 + 1] = (uint8_t)(g * master);
        }
        for (int i = 0; i < high_n; i++) {
            int led = CORE2_LED_COUNT - 1 - i;
            float frac = (zone <= 1) ? 0.0f : (float)i / (float)(zone - 1);
            uint8_t r = (frac < 0.6f) ? 0 : 160;
            uint8_t g = (frac < 0.85f) ? 160 : 0;
            rgb[led * 3 + 0] = (uint8_t)(r * master);
            rgb[led * 3 + 1] = (uint8_t)(g * master);
        }
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style == 20) {
        /* Style 20: mirrored chase — a pair of LEDs (LED i and its mirror
         * LED 9-i) step together around the strip, like style 3's chase
         * but doubled and mirrored, with the step speed driven by the
         * overall loudness (louder = faster) and the color cycling each
         * full lap. */
        static const uint8_t chase_colors[][3] = {
            {160, 0,   0  }, /* red */
            {0,   160, 0  }, /* green */
            {0,   0,   160}, /* blue */
            {160, 160, 0  }, /* yellow */
            {0,   160, 160}, /* cyan */
            {160, 0,   160}, /* magenta */
        };
        static int     pos       = 0;
        static int     color_idx = 0;
        static int64_t last_us   = 0;

        int64_t interval_us = 180000 - (int64_t)(v_overall * 120000.0f);
        if (now_us - last_us >= interval_us) {
            last_us = now_us;
            pos++;
            if (pos >= 5) {
                pos = 0;
                color_idx = (color_idx + 1) %
                            (int)(sizeof(chase_colors) / sizeof(chase_colors[0]));
            }
        }

        uint8_t rgb[CORE2_LED_COUNT * 3] = {0};
        rgb[pos * 3 + 0]       = chase_colors[color_idx][0];
        rgb[pos * 3 + 1]       = chase_colors[color_idx][1];
        rgb[pos * 3 + 2]       = chase_colors[color_idx][2];
        rgb[(9 - pos) * 3 + 0] = chase_colors[color_idx][0];
        rgb[(9 - pos) * 3 + 1] = chase_colors[color_idx][1];
        rgb[(9 - pos) * 3 + 2] = chase_colors[color_idx][2];
        core2_leds_set_pixels_rgb(rgb);
    } else if (s_mp3.visualizer_style >= 21 && s_mp3.visualizer_style <= 100) {
        /* Styles 21-100: generic parameterized engine, see s_viz_table. */
        viz_render_table_style(s_mp3.visualizer_style - 21, levels, v_overall,
                                 v_dom, v_centroid, now_us);
    } else {
        /* Style 1 (default): VU-meter bars — first 5 bands drive the low
         * row, last 5 bands drive the high row. */
        float low = 0.0f, high = 0.0f;
        for (int b = 0; b < VIZ_BANDS / 2; b++) {
            if (levels[b] > low) low = levels[b];
        }
        for (int b = VIZ_BANDS / 2; b < VIZ_BANDS; b++) {
            if (levels[b] > high) high = levels[b];
        }
        core2_leds_set_vu(low, high);
    }
}

static void viz_feed(const short *pcm, int total_samps, int channels, int fs)
{
    if (!core2_leds_initialized()) return;
    if (fs != s_viz_last_fs || s_viz_last_fs == 0) viz_init_for_rate(fs);
    int mono_count = total_samps / channels;
    for (int i = 0; i < mono_count; i++) {
        float mono = (channels == 2)
            ? ((float)pcm[i * 2] + (float)pcm[i * 2 + 1]) * (0.5f / 32768.0f)
            : (float)pcm[i] / 32768.0f;
        s_viz_buf[s_viz_buf_pos++] = mono;
        if (s_viz_buf_pos >= VIZ_BLOCK) {
            viz_run_block();
            s_viz_buf_pos = 0;
        }
    }
}

#endif /* CONFIG_CORE2_HW (visualizer) */

static bool nvs_read_str(const char *key, char *out, size_t out_sz);
static esp_err_t nvs_write_str(const char *key, const char *val);
static esp_err_t nvs_erase_key_local(const char *key);
static inline void mp3_request_ui_refresh(void);
static bool pf_touch_handler(int x, int y, screen_gesture_t gesture);
static void pf_event_handler(const char *event_name, const char *payload_json, void *ctx);
#if CONFIG_CORE2_HW
static esp_err_t core2_axp_read_reg(uint8_t reg, uint8_t *out);
static void pf_menu_open(void);
static void pf_menu_close(void);
static bool pf_menu_handle_tap(int x, int y);
static bool pf_menu_handle_swipe(screen_gesture_t gesture);
static void pf_free_jpeg_rgb_cache(void);
static void pf_redraw_jpeg_view(void);
static void pf_pinch_handler(screen_pinch_phase_t phase, int x1, int y1, int x2, int y2);
static void pf_clock_stop(void);
static void pf_clock_start(const char *params);
static void pf_clock_render(void);
#endif

/* Installed only during wifi_connect() -- any tap aborts the retry loop. */
static bool pf_wifi_skip_touch_handler(int x, int y, screen_gesture_t gesture)
{
    (void)x; (void)y;
    if (gesture == SCREEN_GESTURE_TAP) wifi_connect_abort();
    return true;
}

/* Installed after a skipped/failed wifi connect -- any tap reboots. */
static bool pf_reboot_touch_handler(int x, int y, screen_gesture_t gesture)
{
    (void)x; (void)y;
    if (gesture == SCREEN_GESTURE_TAP) esp_restart();
    return true;
}
static bool mp3_advance_track(int step, const char *reason);
static bool mp3_start_track(int folder_idx, int track_idx, bool keep_position);
static int  mp3_find_folder_trigger(const char *trigger);
static int  jpeg_find_folder_trigger(const char *trigger);
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

#define BT_CONNECT_RETRY_MAX          8
#define BT_CONNECT_TIMEOUT_MS          12000U
#define BT_RECONNECT_DELAY_HARD_DROP_MS 1500U

static const uint16_t s_bt_retry_delay_ms[BT_CONNECT_RETRY_MAX] = {
    900, 1800, 3000, 4500, 6000, 8000, 10000, 12000
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
static void bt_update_coex_preference(bool streaming);

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
    bt_update_coex_preference(true);

    esp_err_t err = esp_a2d_source_connect(s_bt.bda);
    if (err != ESP_OK) {
        s_bt.connecting = false;
        bt_update_coex_preference(false);
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

/* LCD (ILI9342C) and SD card share SPI3 on Core2.  A full screen redraw
 * holds SPI3 for ~840 ms in polling mode, blocking fread() and halting
 * MP3 decode for that window.  At 176 KB/s PCM consumption the BT PCM
 * ring must hold > 840 ms worth of audio to avoid underflowing.
 * 192 KB ≈ 1.09 s — leaves ~45 KB headroom after the longest draw. */
#define BT_PCM_RING_BYTES        (256 * 1024)
#define BT_PCM_LOW_WATER_BYTES   ( 40 * 1024)
#define BT_PCM_HIGH_WATER_BYTES  (220 * 1024)
#define BT_PCM_TARGET_FILL_BYTES (192 * 1024)
#define BT_PCM_START_PRIME_BYTES (192 * 1024)
#define BT_PCM_START_PRIME_TIMEOUT_MS 5000
#define BT_PCM_RESUME_PRIME_BYTES ( 96 * 1024)
#define BT_PCM_RESUME_PRIME_TIMEOUT_MS 2000
#define BT_A2DP_TARGET_SAMPLE_RATE 44100
#define BT_PCM_FRAME_BYTES 4
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

    /* Disable WiFi power save while A2DP is streaming.  The ESP32 BT ROM RF
     * coexistence ISR polls a hardware grant flag that the WiFi modem sets.
     * When WiFi is in MIN_MODEM PS it sleeps between beacon intervals; during
     * that sleep it never sets the flag and the BT ISR spins indefinitely,
     * triggering the interrupt watchdog.  WIFI_PS_NONE keeps the modem awake
     * so it responds to BT's RF request immediately. */
    esp_wifi_set_ps(streaming ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);

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
#if CONFIG_CORE2_HW
            /* Hard handoff: stop local I2S speaker path while BT sink is active. */
            core2_audio_deinit();
#endif
            ESP_LOGI(TAG, "bt: A2DP connected to %s", s_bt.selected_bda[0] ? s_bt.selected_bda : connected_bda);

            if (s_bt.pairing_ui_active) {
                const char *pair_name = s_bt.selected_name[0] ? s_bt.selected_name : "(unknown)";
                const char *pair_bda  = s_bt.selected_bda[0]  ? s_bt.selected_bda  : "";
                ESP_LOGI(TAG, "bt: pairing complete name=%s bda=%s", pair_name, pair_bda);
                char pair_msg[128];
                snprintf(pair_msg, sizeof(pair_msg), "BT Paired\n%s\n%s", pair_name, pair_bda);
                screen_draw_text(pair_msg);
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
            /* Capture exhausted state BEFORE any scheduling; bt_schedule_reconnect
             * increments the counter internally. */
            bool retries_exhausted = (s_bt.connect_retries >= BT_CONNECT_RETRY_MAX);
            if (s_bt.has_bda && !s_bt.discovering && allow_runtime_retry &&
                !retries_exhausted && !s_bt_pending_reconnect) {
                uint32_t min_delay_ms = s_bt_recent_acl_drop ? BT_RECONNECT_DELAY_HARD_DROP_MS : 0;
                bt_schedule_reconnect("A2DP disconnected", min_delay_ms);
                scheduled_retry = true;
            } else if (!s_bt_pending_reconnect || !allow_runtime_retry || retries_exhausted) {
                /* Reset retries when giving up or when no pending retry is
                 * keeping the budget alive.  Do NOT reset when a pending retry
                 * exists within the budget — that would allow the counter to
                 * wrap back to 0 and restart infinite retry loops. */
                s_bt.connect_retries = 0;
            }
            if (s_bt.pairing_ui_active) {
                /* Defer the failure message when retry work is still in flight.
                 * Important: only defer on s_bt_pending_reconnect when retries
                 * are not yet exhausted — otherwise the pending timer could fire,
                 * reset the counter to 0, and restart an infinite retry loop. */
                bool defer = scheduled_retry
                             || s_bt.connecting
                             || (s_bt_pending_reconnect && !retries_exhausted);
                if (defer) {
                    ESP_LOGI(TAG, "bt: pairing retry pending (%d/%d)",
                             s_bt.connect_retries, BT_CONNECT_RETRY_MAX);
                } else {
                    ESP_LOGW(TAG, "bt: pairing failed");
                    s_bt.pairing_ui_active = false;
                    s_bt_hold_local_speaker = false;
                    screen_draw_text("BT pairing\nfailed");
                }
            } else if (!scheduled_retry && !s_bt_pending_reconnect && !s_bt.connecting) {
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

#if CONFIG_BT_A2DP_ENABLE
static void bt_avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    if (!param) return;
    if (event == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
        ESP_LOGI(TAG, "avrc ct: %s",
                 param->conn_stat.connected ? "connected" : "disconnected");
    }
}

static void bt_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    if (!param) return;

    if (event == ESP_AVRC_TG_CONNECTION_STATE_EVT) {
        ESP_LOGI(TAG, "avrc tg: %s",
                 param->conn_stat.connected ? "connected" : "disconnected");
        return;
    }

    if (event != ESP_AVRC_TG_PASSTHROUGH_CMD_EVT) return;

    uint8_t cmd   = param->psth_cmd.key_code;
    uint8_t state = param->psth_cmd.key_state;

    ESP_LOGI(TAG, "avrc tg: cmd=0x%02X %s", cmd,
             state == ESP_AVRC_PT_CMD_STATE_PRESSED ? "PRESSED" : "RELEASED");

    if (cmd == ESP_AVRC_PT_CMD_PLAY || cmd == ESP_AVRC_PT_CMD_PAUSE) {
        if (state == ESP_AVRC_PT_CMD_STATE_PRESSED) {
            s_avrc_play_pressed_tick = xTaskGetTickCount();
        } else {
            uint32_t held_ms = s_avrc_play_pressed_tick
                ? (uint32_t)((xTaskGetTickCount() - s_avrc_play_pressed_tick) * portTICK_PERIOD_MS)
                : 0;
            s_avrc_play_pressed_tick = 0;
            ESP_LOGI(TAG, "avrc tg: play/pause held %lu ms (threshold %u ms)",
                     (unsigned long)held_ms, AVRC_VOICE_LONG_PRESS_MS);
            if (held_ms >= AVRC_VOICE_LONG_PRESS_MS) {
                s_avrc_pending_voice = true;
                ESP_LOGI(TAG, "avrc tg: long press → voice prompt");
            } else {
                s_avrc_pending_play_pause = true;
            }
        }
    } else if (cmd == ESP_AVRC_PT_CMD_STOP && state == ESP_AVRC_PT_CMD_STATE_PRESSED) {
        s_avrc_pending_play_pause = true;
    } else if (cmd == ESP_AVRC_PT_CMD_FORWARD && state == ESP_AVRC_PT_CMD_STATE_PRESSED) {
        s_avrc_pending_track_step = 1;
    } else if (cmd == ESP_AVRC_PT_CMD_BACKWARD && state == ESP_AVRC_PT_CMD_STATE_PRESSED) {
        s_avrc_pending_track_step = -1;
    } else if (cmd == ESP_AVRC_PT_CMD_VENDOR && state == ESP_AVRC_PT_CMD_STATE_PRESSED) {
        /* Many earbuds map their long-press button to the vendor-unique AVRCP
         * command (0x7E) rather than holding PLAY long enough to trip the timer. */
        s_avrc_pending_voice = true;
        ESP_LOGI(TAG, "avrc tg: vendor command → voice prompt");
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
                    /* bt_start_connect_now returns false both when it cannot
                     * initiate a new connection AND when one is already in
                     * progress (s_bt.connecting).  Only show failure when we
                     * are genuinely stuck — not when a pending connection may
                     * still succeed. */
                    if (s_bt.pairing_ui_active && !s_bt_pending_reconnect
                            && !s_bt.connecting) {
                        ESP_LOGW(TAG, "bt: pairing connect failed");
                        s_bt.pairing_ui_active = false;
                        s_bt_hold_local_speaker = false;
                        screen_draw_text("BT pairing\nfailed");
                    }
                }
            } else if (s_bt.pairing_ui_active && !s_bt.has_bda) {
                ESP_LOGW(TAG, "bt: pairing no audio device found");
                s_bt.pairing_ui_active = false;
                s_bt_hold_local_speaker = false;
                screen_draw_text("No audio\ndevice found");
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
    /* Bluedroid requires AVRCP to be registered before A2DP source init.
     * Initializing after A2DP causes "expected to be initialized in advance"
     * warnings and breaks both the AVRCP channel and A2DP audio quality. */
    err = esp_avrc_ct_register_callback(bt_avrc_ct_cb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bt: AVRCP CT callback registration failed: %s", esp_err_to_name(err));
    } else {
        err = esp_avrc_ct_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bt: AVRCP CT init failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "bt: AVRCP CT ready");
        }
    }

    err = esp_avrc_tg_register_callback(bt_avrc_tg_cb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "bt: AVRCP TG callback registration failed: %s", esp_err_to_name(err));
    } else {
        err = esp_avrc_tg_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bt: AVRCP TG init failed: %s", esp_err_to_name(err));
        } else {
            /* Register the passthrough commands we want delivered to our callback.
             * Bluedroid's SUPPORTED_CMD filter defaults to empty; commands not in it
             * are handled internally and never reach ESP_AVRC_TG_PASSTHROUGH_CMD_EVT. */
            esp_avrc_psth_bit_mask_t psth_mask;
            memset(&psth_mask, 0, sizeof(psth_mask));
            esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth_mask, ESP_AVRC_PT_CMD_PLAY);
            esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth_mask, ESP_AVRC_PT_CMD_PAUSE);
            esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth_mask, ESP_AVRC_PT_CMD_STOP);
            esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth_mask, ESP_AVRC_PT_CMD_FORWARD);
            esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &psth_mask, ESP_AVRC_PT_CMD_BACKWARD);
            /* VENDOR (0x7E) is NOT in Bluedroid's allowed set — including it causes
             * esp_avrc_tg_set_psth_cmd_filter to return ESP_ERR_NOT_SUPPORTED and
             * leave the entire filter empty, silently dropping all passthrough cmds. */
            esp_err_t psth_err = esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &psth_mask);
            if (psth_err != ESP_OK) {
                ESP_LOGW(TAG, "bt: AVRCP psth filter set failed: %s", esp_err_to_name(psth_err));
            }
            ESP_LOGI(TAG, "bt: AVRCP TG ready (earbud controls enabled)");
        }
    }

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
#if CONFIG_CORE2_HW
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
#if CONFIG_CORE2_HW
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
        "jpeg", "save", "play", "pause", "stop", "next", "previous", "forward", "reverse", "volumeup",
        "volumedown", "volumelevel", "shuffle", "repeattrack", "repeatplaylist",
        "pair", "btstatus", "btdisconnect", "btforget", "reboot",
        "mute"
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

static bool is_jpeg_file_name(const char *name)
{
    return str_ends_with_ci(name, ".jpg") || str_ends_with_ci(name, ".jpeg");
}

static int jpeg_count_in_folder(const char *folder_path) __attribute__((unused));
static int jpeg_count_in_folder(const char *folder_path)
{
    DIR *d = opendir(folder_path);
    if (!d) return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (is_jpeg_file_name(e->d_name)) count++;
    }
    closedir(d);
    return count;
}

static bool jpeg_get_nth_file(const char *folder_path,
                               int target_idx,
                               char *out_name,
                               size_t out_sz,
                               int *total_out) __attribute__((unused));
static bool jpeg_get_nth_file(const char *folder_path,
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
        if (!is_jpeg_file_name(e->d_name)) continue;
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

/* Fast duration estimate: reads only the first frame header + file size.
 * Accurate for CBR; approximate (±10%) for VBR.  Never blocks more than
 * a handful of SD sector reads. */
static uint32_t mp3_estimate_duration_ms(const char *file_path)
{
    FILE *fp = fopen(file_path, "rb");
    if (!fp) return 0;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 0; }
    long file_size = ftell(fp);
    if (file_size <= 0) { fclose(fp); return 0; }

    rewind(fp);

    /* Skip ID3v2 tag if present */
    long audio_start = 0;
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, fp) == 10 &&
        hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        uint32_t tag_size = ((uint32_t)(hdr[6] & 0x7F) << 21) |
                            ((uint32_t)(hdr[7] & 0x7F) << 14) |
                            ((uint32_t)(hdr[8] & 0x7F) << 7)  |
                            (uint32_t)(hdr[9] & 0x7F);
        audio_start = 10 + (long)tag_size;
        if (fseek(fp, audio_start, SEEK_SET) != 0) { fclose(fp); return 0; }
    } else {
        rewind(fp);
    }

    mp3_frame_info_t info;
    if (!mp3_stream_next_frame(fp, &info) || info.frame_bytes <= 0 || info.sample_rate <= 0) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    /* Audio bytes: subtract header area and 128-byte ID3v1 tail reservation */
    long audio_size = file_size - audio_start - 128;
    if (audio_size < info.frame_bytes) return 0;

    uint64_t total_frames  = (uint64_t)audio_size / (uint64_t)info.frame_bytes;
    uint64_t total_samples = total_frames * (uint64_t)info.samples_per_frame;
    uint64_t ms            = total_samples * 1000ULL / (uint64_t)info.sample_rate;

    if (ms < 1000ULL) return 0;
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
             "mp3: %s -> shuffle=%s repeattrack=%s repeatplaylist=%s visualizer=%s style=%u",
             reason ? reason : "mode update",
             s_mp3.shuffle ? "on" : "off",
             s_mp3.repeat_track ? "on" : "off",
             s_mp3.repeat_playlist ? "on" : "off",
             s_mp3.visualizer ? "on" : "off",
             (unsigned)s_mp3.visualizer_style);
}

#ifndef MP3_INPUT_BUF_BYTES
#define MP3_INPUT_BUF_BYTES (32 * 1024)
#endif

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
#if CONFIG_CORE2_HW
            if (s_mp3.visualizer) { s_viz_buf_pos = 0; core2_leds_off(); }
#endif
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
#if CONFIG_CORE2_HW
            if (s_mp3.visualizer) { s_viz_buf_pos = 0; core2_leds_off(); }
#endif
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
            uint32_t probed_ms = mp3_estimate_duration_ms(s_mp3.file_path);
            if (probed_ms > 0) {
                s_mp3.duration_ms = probed_ms;
                mp3_request_ui_refresh();
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

        /* Hold off SD-card SPI reads while BT is establishing a connection.
         * The sdspi interrupt-mode SPI ISR adds timing pressure that can push
         * the BT HCI ISR past the interrupt WDT threshold during pairing. */
#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
        if (s_bt.connecting || s_bt.discovering) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
#endif

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
                                       s_mp3.muted ? 0 : s_mp3.volume);
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
            core2_audio_write_pcm(pcm, (size_t)fi.outputSamps, fi.nChans, s_mp3.muted ? 0 : s_mp3.volume);
        }

#if CONFIG_CORE2_HW
        /* Visualizer: drive side LEDs with per-band frequency energy */
        if (s_mp3.visualizer && s_mp3.active && !s_mp3.paused) {
            viz_feed(pcm, fi.outputSamps, fi.nChans, fi.samprate);
        }
#endif

        /* Position tracking and periodic UI refresh (Core2 path) */
        {
            uint32_t mono_samples = (uint32_t)(fi.outputSamps / fi.nChans);
            uint32_t frame_ms = (uint32_t)(((uint64_t)mono_samples * 1000ULL) /
                                           (uint64_t)fi.samprate);
            if (frame_ms == 0) frame_ms = 1;

            if (s_mp3.active && !s_mp3.paused && opened_token == s_mp3.play_token) {
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
            }
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
#if CONFIG_CORE2_HW
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

    if (!s_mp3_ui_override_allowed) return;

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

    char file_short[44];
    if (strlen(s_mp3.file_name) > 40) {
        memcpy(file_short, s_mp3.file_name, 37);
        strcpy(file_short + 37, "...");
    } else {
        strncpy(file_short, s_mp3.file_name, sizeof(file_short) - 1);
        file_short[sizeof(file_short) - 1] = '\0';
    }

    char vol_str[12];
    if (s_mp3.muted) {
        strcpy(vol_str, "MUTE");
    } else {
        snprintf(vol_str, sizeof(vol_str), "%d%%", s_mp3.volume);
    }
    char msg[1000];
    snprintf(msg, sizeof(msg),
             "MUSIC %s\n"
             "Song: %s\n"
             "Folder: %s\n"
             "[%s]\n"
             "%s / %s\n"
             "Vol:%s  Shuffle:%s\n"
             "RptTrack:%s  RptList:%s\n"
             "Visualizer:%s  Style:%u\n"
             "\n"
             "[      %s      ]\n"
             "[ VOLUME  ^  v ]\n"
             "[ TRACK  <  > ]",
             s_mp3.paused ? "Paused" : "Playing",
             file_short[0] ? file_short : "(none)",
             s_mp3.folder_name[0] ? s_mp3.folder_name : "(none)",
             bar,
             cur,
             total,
             vol_str,
             s_mp3.shuffle ? "on" : "off",
             s_mp3.repeat_track ? "on" : "off",
             s_mp3.repeat_playlist ? "on" : "off",
             s_mp3.visualizer ? "on" : "off",
             (unsigned)s_mp3.visualizer_style,
             s_mp3.paused ? "PLAY" : "PAUSE");
    if (s_mp3_saved_font_scale < 0) {
        int cur = 2;
        screen_get_font_scale(&cur);
        s_mp3_saved_font_scale = cur;
    }
    screen_set_font_scale_silent(1);
    screen_draw_text(msg);
}

/* ── On-screen command menu (Core2 touch) ───────────────────────────────
 * Long-press anywhere opens an overlay listing the local commands from
 * picture_frame_commands.json that need no typed input. Tap an item to
 * run it; swipe left/right to change pages; long-press again to close. */
#if CONFIG_CORE2_HW

/* At font scale 2 (32px rows) landscape (240px tall) fits 7 rows total,
 * so 5 items per page leaves room for the header and footer lines. */
#define PF_MENU_ITEMS_PER_PAGE 5

typedef enum {
    PF_MENU_REBOOT, PF_MENU_SLEEP_NOW, PF_MENU_SAVE, PF_MENU_SAVEPIC,
    PF_MENU_LANDSCAPE, PF_MENU_PORTRAIT, PF_MENU_FONTSIZE,
    PF_MENU_FOLDERS, PF_MENU_BATTERY,
    PF_MENU_BT_STATUS, PF_MENU_BT_PAIR, PF_MENU_BT_DISCONNECT,
    PF_MENU_BT_FORGET, PF_MENU_PLAY_PAUSE, PF_MENU_STOP,
    PF_MENU_SEEK_FWD, PF_MENU_SEEK_REV, PF_MENU_VOL_UP, PF_MENU_VOL_DOWN,
    PF_MENU_MUTE, PF_MENU_SHUFFLE, PF_MENU_REPEAT_TRACK, PF_MENU_REPEAT_PLAYLIST,
    PF_MENU_VISUALIZER, PF_MENU_VIZ_NEXT, PF_MENU_VIZ_PREV,
    PF_MENU_LEDCOLOR, PF_MENU_SLEEP_TIMER, PF_MENU_SLEEP_ON_POWER, PF_MENU_AI_SPEECH,
    PF_MENU_CLOCK,
    PF_MENU_ITEM_COUNT
} pf_menu_item_id_t;

#define PF_MENU_PAGE_COUNT \
    ((PF_MENU_ITEM_COUNT + PF_MENU_ITEMS_PER_PAGE - 1) / PF_MENU_ITEMS_PER_PAGE)

/* Cycling presets for menu items that need a value but have no on-screen
 * keyboard. Each tap advances to the next value and dispatches/labels it. */
static const int PF_MENU_FONTSIZES[]  = { 1, 2, 3, 4 };
static const int PF_MENU_SLEEP_MINS[] = { 0, 5, 10, 15, 30, 60 };
static const char *const PF_MENU_LED_COLORS[] = {
    "red", "green", "blue", "yellow", "purple", "cyan", "white", "off"
};
#define PF_MENU_FONTSIZE_COUNT  (sizeof(PF_MENU_FONTSIZES)  / sizeof(PF_MENU_FONTSIZES[0]))
#define PF_MENU_SLEEP_MIN_COUNT (sizeof(PF_MENU_SLEEP_MINS) / sizeof(PF_MENU_SLEEP_MINS[0]))
#define PF_MENU_LED_COLOR_COUNT (sizeof(PF_MENU_LED_COLORS) / sizeof(PF_MENU_LED_COLORS[0]))

/* Indices into the cycling-preset tables above. Each tap advances the index
 * *then* dispatches/labels that entry, so the on-screen label always matches
 * the value that was just applied. Start at the last entry so the first tap
 * lands on index 0. */
static int s_menu_fontsize_idx   = PF_MENU_FONTSIZE_COUNT - 1;
static int s_menu_sleeptimer_idx = PF_MENU_SLEEP_MIN_COUNT - 1;
static int s_menu_ledcolor_idx   = PF_MENU_LED_COLOR_COUNT - 1;

/* Build a {"trigger":"...","params":"..."} payload and run it through the
 * normal command dispatcher, exactly as if it had arrived over the socket --
 * this reuses every command's existing behaviour (incl. NVS persistence).
 * An empty/absent "id" suppresses the run/save report back to the cloud. */
static void pf_menu_dispatch(const char *trigger, const char *params)
{
    char payload[320];
    snprintf(payload, sizeof(payload), "{\"trigger\":\"%s\",\"params\":\"%s\"}",
             trigger, params ? params : "");
    pf_event_handler("message", payload, NULL);
}

static void pf_menu_item_label(int idx, char *out, size_t out_sz)
{
    switch ((pf_menu_item_id_t)idx) {
        case PF_MENU_REBOOT:        snprintf(out, out_sz, "Reboot"); break;
        case PF_MENU_SLEEP_NOW:     snprintf(out, out_sz, "Sleep Now"); break;
        case PF_MENU_SAVE:          snprintf(out, out_sz, "Save Settings"); break;
        case PF_MENU_SAVEPIC:       snprintf(out, out_sz, "Save Picture"); break;
        case PF_MENU_LANDSCAPE:     snprintf(out, out_sz, "Landscape"); break;
        case PF_MENU_PORTRAIT:      snprintf(out, out_sz, "Portrait"); break;
        case PF_MENU_FONTSIZE:
            snprintf(out, out_sz, "Font Size: %d",
                     PF_MENU_FONTSIZES[s_menu_fontsize_idx % PF_MENU_FONTSIZE_COUNT]);
            break;
        case PF_MENU_FOLDERS:       snprintf(out, out_sz, "List Folders"); break;
        case PF_MENU_BATTERY:       snprintf(out, out_sz, "Battery Status"); break;
        case PF_MENU_BT_STATUS:     snprintf(out, out_sz, "Bluetooth Status"); break;
        case PF_MENU_BT_PAIR:       snprintf(out, out_sz, "Bluetooth Pair"); break;
        case PF_MENU_BT_DISCONNECT: snprintf(out, out_sz, "Bluetooth Disconnect"); break;
        case PF_MENU_BT_FORGET:     snprintf(out, out_sz, "Bluetooth Forget"); break;
        case PF_MENU_PLAY_PAUSE:
            snprintf(out, out_sz, "%s", (s_mp3.active && !s_mp3.paused) ? "Pause" : "Play");
            break;
        case PF_MENU_STOP:          snprintf(out, out_sz, "Stop"); break;
        case PF_MENU_SEEK_FWD:      snprintf(out, out_sz, "Seek +10s"); break;
        case PF_MENU_SEEK_REV:      snprintf(out, out_sz, "Seek -10s"); break;
        case PF_MENU_VOL_UP:        snprintf(out, out_sz, "Volume Up"); break;
        case PF_MENU_VOL_DOWN:      snprintf(out, out_sz, "Volume Down"); break;
        case PF_MENU_MUTE:
            snprintf(out, out_sz, "Mute: %s", s_mp3.muted ? "On" : "Off");
            break;
        case PF_MENU_SHUFFLE:
            snprintf(out, out_sz, "Shuffle: %s", s_mp3.shuffle ? "On" : "Off");
            break;
        case PF_MENU_REPEAT_TRACK:
            snprintf(out, out_sz, "Repeat Track: %s", s_mp3.repeat_track ? "On" : "Off");
            break;
        case PF_MENU_REPEAT_PLAYLIST:
            snprintf(out, out_sz, "Repeat List: %s", s_mp3.repeat_playlist ? "On" : "Off");
            break;
        case PF_MENU_VISUALIZER:
            snprintf(out, out_sz, "Visualizer: %s", s_mp3.visualizer ? "On" : "Off");
            break;
        case PF_MENU_VIZ_NEXT:      snprintf(out, out_sz, "Viz Style Next"); break;
        case PF_MENU_VIZ_PREV:      snprintf(out, out_sz, "Viz Style Prev"); break;
        case PF_MENU_LEDCOLOR:
            snprintf(out, out_sz, "LED Color: %s",
                     PF_MENU_LED_COLORS[s_menu_ledcolor_idx % PF_MENU_LED_COLOR_COUNT]);
            break;
        case PF_MENU_SLEEP_TIMER:
            snprintf(out, out_sz, "Sleep Timer: %d min",
                     PF_MENU_SLEEP_MINS[s_menu_sleeptimer_idx % PF_MENU_SLEEP_MIN_COUNT]);
            break;
        case PF_MENU_SLEEP_ON_POWER:
            snprintf(out, out_sz, "Sleep On Power: %s", s_sleep_while_powered ? "On" : "Off");
            break;
        case PF_MENU_AI_SPEECH:
            snprintf(out, out_sz, "AI Speech: %s", s_ai_tts_enabled ? "On" : "Off");
            break;
        case PF_MENU_CLOCK:
            snprintf(out, out_sz, "Clock: %s",
                     s_clock_mode == PF_CLOCK_ANALOG ? "Analog" :
                     s_clock_mode == PF_CLOCK_DIGITAL ? "Digital" : "Off");
            break;
        default:
            snprintf(out, out_sz, "?");
            break;
    }
}

/* Run the action for menu item `idx`. Returns true if the menu should close
 * (the action draws its own result on screen), false to stay open and
 * re-render with refreshed item labels. */
static bool pf_menu_execute_item(int idx)
{
    char params[8];
    switch ((pf_menu_item_id_t)idx) {
        case PF_MENU_REBOOT:          pf_menu_dispatch("reboot", "");        return true;
        case PF_MENU_SLEEP_NOW:       pf_menu_dispatch("sleep", "");         return true;
        case PF_MENU_SAVE:
            pf_menu_dispatch("save", "");
            s_menu_skip_close_redraw = true;
            return true;
        case PF_MENU_SAVEPIC:
            pf_menu_dispatch("savepic", "");
            s_menu_skip_close_redraw = true;
            return true;
        case PF_MENU_LANDSCAPE:       pf_menu_dispatch("landscape", "");     return true;
        case PF_MENU_PORTRAIT:        pf_menu_dispatch("portrait", "");      return true;
        case PF_MENU_FONTSIZE:
            s_menu_fontsize_idx = (s_menu_fontsize_idx + 1) % PF_MENU_FONTSIZE_COUNT;
            snprintf(params, sizeof(params), "%d",
                     PF_MENU_FONTSIZES[s_menu_fontsize_idx % PF_MENU_FONTSIZE_COUNT]);
            pf_menu_dispatch("fontsize", params);
            /* The screen's real font size just changed; update the saved
             * scale so pf_menu_close() restores this new size instead of
             * reverting to whatever was active before the menu opened. */
            s_menu_saved_font_scale = PF_MENU_FONTSIZES[s_menu_fontsize_idx % PF_MENU_FONTSIZE_COUNT];
            return false;
        case PF_MENU_FOLDERS:
            pf_menu_dispatch("folders", "");
            s_menu_skip_close_redraw = true;
            return true;
        case PF_MENU_BATTERY:
            pf_menu_dispatch("battery", "");
            s_menu_skip_close_redraw = true;
            return true;
        case PF_MENU_BT_STATUS:
            pf_menu_dispatch("btstatus", "");
            s_menu_skip_close_redraw = true;
            return true;
        case PF_MENU_BT_PAIR:
            pf_menu_dispatch("pair", "");
            s_menu_skip_close_redraw = true;
            return true;
        case PF_MENU_BT_DISCONNECT:
            pf_menu_dispatch("btdisconnect", "");
            s_menu_skip_close_redraw = true;
            return true;
        case PF_MENU_BT_FORGET:
            pf_menu_dispatch("btforget", "");
            s_menu_skip_close_redraw = true;
            return true;
        case PF_MENU_PLAY_PAUSE:      pf_menu_dispatch("pause", "");         return true;
        case PF_MENU_STOP:            pf_menu_dispatch("stop", "");          return true;
        case PF_MENU_SEEK_FWD:        pf_menu_dispatch("forward", "");       return true;
        case PF_MENU_SEEK_REV:        pf_menu_dispatch("reverse", "");       return true;
        case PF_MENU_VOL_UP:          pf_menu_dispatch("volumeup", "");      return true;
        case PF_MENU_VOL_DOWN:        pf_menu_dispatch("volumedown", "");    return true;
        case PF_MENU_MUTE:            pf_menu_dispatch("mute", "");          return false;
        case PF_MENU_SHUFFLE:         pf_menu_dispatch("shuffle", "");       return false;
        case PF_MENU_REPEAT_TRACK:    pf_menu_dispatch("repeattrack", "");   return false;
        case PF_MENU_REPEAT_PLAYLIST: pf_menu_dispatch("repeatplaylist", ""); return false;
        case PF_MENU_VISUALIZER:      pf_menu_dispatch("visualizer", "");    return false;
        case PF_MENU_VIZ_NEXT:        pf_menu_dispatch("visualizernext", ""); return false;
        case PF_MENU_VIZ_PREV:        pf_menu_dispatch("visualizerprevious", ""); return false;
        case PF_MENU_LEDCOLOR:
            s_menu_ledcolor_idx = (s_menu_ledcolor_idx + 1) % PF_MENU_LED_COLOR_COUNT;
            pf_menu_dispatch("ledcolor", PF_MENU_LED_COLORS[s_menu_ledcolor_idx]);
            return false;
        case PF_MENU_SLEEP_TIMER:
            s_menu_sleeptimer_idx = (s_menu_sleeptimer_idx + 1) % PF_MENU_SLEEP_MIN_COUNT;
            snprintf(params, sizeof(params), "%d",
                     PF_MENU_SLEEP_MINS[s_menu_sleeptimer_idx % PF_MENU_SLEEP_MIN_COUNT]);
            pf_menu_dispatch("sleeptimer", params);
            return false;
        case PF_MENU_SLEEP_ON_POWER:  pf_menu_dispatch("sleeponpower", "");  return false;
        case PF_MENU_AI_SPEECH:       pf_menu_dispatch("aitts", "");         return false;
        case PF_MENU_CLOCK:           pf_menu_dispatch("clock", "");         return true;
        default: return true;
    }
}

static void pf_menu_render(void)
{
    char buf[1024];
    int  len   = 0;
    int  start = s_menu_page * PF_MENU_ITEMS_PER_PAGE;
    /* The menu is always shown at font scale 2, independent of the user's
     * font-size setting (which may have just been changed via this menu). */
    screen_set_font_scale_silent(2);
    int  end   = start + PF_MENU_ITEMS_PER_PAGE;
    if (end > PF_MENU_ITEM_COUNT) end = PF_MENU_ITEM_COUNT;

    len += snprintf(buf + len, sizeof(buf) - len, "== Menu %d/%d ==\n",
                     s_menu_page + 1, PF_MENU_PAGE_COUNT);
    for (int i = start; i < end; i++) {
        char label[40];
        pf_menu_item_label(i, label, sizeof(label));
        len += snprintf(buf + len, sizeof(buf) - len, "%s\n", label);
    }
    /* Kept <= 20 chars so it doesn't wrap to a 2nd line in portrait at
     * font scale 2 (320px / 16px-per-char = 20 cols), which would throw
     * off pf_menu_handle_tap()'s row math. */
    snprintf(buf + len, sizeof(buf) - len, "Swipe=pg Hold=close");
    screen_draw_text(buf);
}

/* The folder/file list screens are always shown at font scale 2, like the
 * menu, independent of the user's font-size setting. Call before drawing
 * either list; saves the current scale on first entry (no-op on the
 * folders -> files transition, which stays in list mode). */
static void pf_list_enter_scale2(void)
{
    if (s_list_saved_font_scale < 0) {
        screen_get_font_scale(&s_list_saved_font_scale);
    }
    screen_set_font_scale_silent(2);
}

/* Restore the font scale saved by pf_list_enter_scale2(), if any. */
static void pf_list_restore_scale(void)
{
    if (s_list_saved_font_scale >= 1) {
        screen_set_font_scale_silent(s_list_saved_font_scale);
        s_list_saved_font_scale = -1;
    }
}

/* Max characters per line at the font-scale-2 list display, so callers can
 * truncate names before appending them to the list text. A name longer than
 * this would wrap to a 2nd display line, throwing off the folder/file list
 * tap handlers' one-line-per-entry row math. */
static int pf_list_max_cols(void)
{
    bool landscape = true;
    screen_get_landscape(&landscape);
    return landscape ? 20 : 15;
}

static void pf_menu_open(void)
{
    pf_clock_stop();
    s_menu_active = true;
    s_menu_page = 0;
    screen_get_font_scale(&s_menu_saved_font_scale);
    /* Sync the cycling index to the actual current font size so the label
     * shown on open matches reality, even on first open after boot. */
    for (size_t i = 0; i < PF_MENU_FONTSIZE_COUNT; i++) {
        if (PF_MENU_FONTSIZES[i] == s_menu_saved_font_scale) {
            s_menu_fontsize_idx = (int)i;
            break;
        }
    }
    screen_set_font_scale_silent(2);
    pf_menu_render();
}

static void pf_menu_close(void)
{
    s_menu_active = false;
    if (s_menu_saved_font_scale >= 1) {
        screen_set_font_scale_silent(s_menu_saved_font_scale);
        s_menu_saved_font_scale = -1;
    }
    if (s_menu_skip_close_redraw) {
        /* The action just drawn its own result text; leave it on screen
         * instead of immediately overwriting it below. */
        s_menu_skip_close_redraw = false;
        s_menu_result_active = true;
        return;
    }
    /* Closing the menu without picking a result-screen action: drop any
     * lingering result screen so playback/text UI can resume below. */
    s_menu_result_active = false;
    s_battery_display_active = false;
    s_folder_list_display_active = false;
    s_file_list_display_active = false;
    pf_list_restore_scale();
    if (s_jpeg_cache) {
        s_pending_jpeg_redraw = true;
    } else if (s_mp3.active && s_mp3_ui_override_allowed) {
        mp3_request_ui_refresh();
    } else {
        screen_draw_text(s_last_text[0] ? s_last_text : " ");
    }
}

/* Map a tap's y-coordinate to a menu row, using the same centred layout
 * that pf_menu_render()'s screen_draw_text() call produces at scale 1. */
static bool pf_menu_handle_tap(int x, int y)
{
    (void)x;
    bool landscape = true;
    screen_get_landscape(&landscape);
    int lcd_h_val = landscape ? 240 : 320;
    int ch = 32; /* 32px rows at font scale 2, used while the menu is open */

    int start = s_menu_page * PF_MENU_ITEMS_PER_PAGE;
    int end   = start + PF_MENU_ITEMS_PER_PAGE;
    if (end > PF_MENU_ITEM_COUNT) end = PF_MENU_ITEM_COUNT;
    int total_lines = (end - start) + 2; /* header + items + footer */

    int start_y = (lcd_h_val - total_lines * ch) / 2;
    if (start_y < 0) start_y = 0;
    int row = (y - start_y) / ch;

    if (row >= 1 && row <= (end - start)) {
        int item = start + (row - 1);
        /* Defer to the main task: pf_menu_execute_item() can call
         * pf_event_handler(), which does SD card I/O etc. and needs more
         * stack than this task has. */
        s_menu_pending_item = item;
    }
    return true;
}

static bool pf_menu_handle_swipe(screen_gesture_t gesture)
{
    if (gesture == SCREEN_GESTURE_SWIPE_LEFT) {
        s_menu_page = (s_menu_page + 1) % PF_MENU_PAGE_COUNT;
        pf_menu_render();
    } else if (gesture == SCREEN_GESTURE_SWIPE_RIGHT) {
        s_menu_page = (s_menu_page - 1 + PF_MENU_PAGE_COUNT) % PF_MENU_PAGE_COUNT;
        pf_menu_render();
    }
    /* swipe up/down: ignored, but still consumed while the menu is open */
    return true;
}

#endif /* CONFIG_CORE2_HW */

static bool pf_touch_handler(int x, int y, screen_gesture_t gesture)
{
#if !CONFIG_CORE2_HW
    (void)x; (void)y;
#endif
#if CONFIG_HARDWARE_CORE2
    s_last_activity_tick = xTaskGetTickCount();
#endif
#if CONFIG_CORE2_HW
    if (gesture == SCREEN_GESTURE_LONG_PRESS) {
        if (s_menu_active) {
            pf_menu_close();
        } else {
            pf_menu_open();
        }
        return true;
    }
    if (s_menu_active) {
        if (gesture == SCREEN_GESTURE_TAP) return pf_menu_handle_tap(x, y);
        return pf_menu_handle_swipe(gesture);
    }
    if (s_clock_mode != PF_CLOCK_OFF && gesture == SCREEN_GESTURE_TAP) {
        /* Tap toggles between digital and analog views; the next main-loop
         * tick redraws via pf_clock_render(). */
        s_clock_mode = (s_clock_mode == PF_CLOCK_ANALOG) ? PF_CLOCK_DIGITAL : PF_CLOCK_ANALOG;
        s_clock_last_sec = -1;
        nvs_write_u8(NVS_KEY_CLOCK_MODE, (uint8_t)s_clock_mode);
        return true;
    }
    if (s_battery_display_active && gesture == SCREEN_GESTURE_TAP) {
        uint8_t vbat_h = 0, vbat_l = 0;
        if (core2_axp_read_reg(0x78, &vbat_h) == ESP_OK &&
            core2_axp_read_reg(0x79, &vbat_l) == ESP_OK) {
            int adc   = ((int)vbat_h << 4) | ((int)vbat_l & 0x0F);
            int vbat  = (int)((float)adc * 1.1f);
            int level = (vbat - 3300) * 100 / 900;
            if (level < 0)   level = 0;
            if (level > 100) level = 100;
            char scr[64];
            snprintf(scr, sizeof(scr), "Battery: %d%%\n%d mV", level, vbat);
            screen_draw_text(scr);
            ESP_LOGI(TAG, "battery tap refresh: %d%% %d mV", level, vbat);
        }
        return true;
    }
    if (s_folder_list_display_active && gesture == SCREEN_GESTURE_TAP) {
        /* The folder list was drawn at font scale 2 (32px rows), with the
         * "Folders:" header on row 0 and folder names on the rows below,
         * centred vertically the same way pf_menu_render()'s text is. */
        bool landscape = true;
        screen_get_landscape(&landscape);
        int lcd_h_val = landscape ? 240 : 320;
        int ch = 32;
        int total_lines = 1 + s_folder_list_count;
        int start_y = (lcd_h_val - total_lines * ch) / 2;
        if (start_y < 0) start_y = 0;
        int row = (y - start_y) / ch;
        int idx = row - 1;
        if (idx >= 0 && idx < s_folder_list_count) {
            s_pending_folder_tap_idx = idx;
        }
        return true;
    }
    if (s_file_list_display_active && gesture == SCREEN_GESTURE_TAP) {
        /* The file list was drawn at font scale 2 (32px rows), with the
         * "<folder>:" header on row 0 and file names on the rows below. */
        bool landscape = true;
        screen_get_landscape(&landscape);
        int lcd_h_val = landscape ? 240 : 320;
        int ch = 32;
        int total_lines = 1 + s_file_list_count;
        int start_y = (lcd_h_val - total_lines * ch) / 2;
        if (start_y < 0) start_y = 0;
        int row = (y - start_y) / ch;
        int idx = row - 1;
        if (idx >= 0 && idx < s_file_list_count) {
            s_file_list_display_active = false;
            pf_list_restore_scale();
            if (s_file_list_types[idx] == PF_FILE_MP3) {
                int folder_idx = mp3_find_folder_trigger(s_file_list_folder_trigger);
                if (folder_idx >= 0) {
                    mp3_start_track(folder_idx, s_file_list_subidx[idx], false);
                }
            } else if (s_file_list_types[idx] == PF_FILE_JPEG) {
                int jpeg_folder_idx = jpeg_find_folder_trigger(s_file_list_folder_trigger);
                char file_path[MP3_MAX_PATH_LEN + MP3_MAX_FILE_LEN + 4];
                snprintf(file_path, sizeof(file_path), "%s/%s",
                         s_file_list_folder_path, s_file_list_names[idx]);
                s_mp3_ui_override_allowed = false;
                s_pending_jpeg = false;
                if (jpeg_folder_idx >= 0) {
                    s_jpeg_folder_display_active = true;
                    s_jpeg_folder_display_idx    = jpeg_folder_idx;
                    s_jpeg_folder_image_idx       = s_file_list_subidx[idx];
                }
                strncpy(s_pending_jpeg_file_path, file_path, sizeof(s_pending_jpeg_file_path) - 1);
                s_pending_jpeg_file_path[sizeof(s_pending_jpeg_file_path) - 1] = '\0';
                s_pending_jpeg_file = true;
            }
        }
        return true;
    }
    if (s_menu_result_active && gesture == SCREEN_GESTURE_TAP) {
        /* Dismiss a plain-text menu-action result (e.g. BT status/pair/save) that
         * has no dedicated tap handler of its own. */
        s_menu_result_active = false;
        if (s_save_result_saved_font_scale >= 1) {
            screen_set_font_scale_silent(s_save_result_saved_font_scale);
            s_save_result_saved_font_scale = -1;
        }
        if (s_mp3.active && s_mp3_ui_override_allowed) {
            mp3_request_ui_refresh();
        } else {
            screen_draw_text(s_last_text[0] ? s_last_text : " ");
        }
        return true;
    }
    if (s_jpeg_rgb565 && gesture == SCREEN_GESTURE_TAP) {
        TickType_t now = xTaskGetTickCount();
        int dx = x - s_jpeg_last_tap_x;
        int dy = y - s_jpeg_last_tap_y;
        int abs_dx = dx < 0 ? -dx : dx;
        int abs_dy = dy < 0 ? -dy : dy;
        bool is_double_tap = (now - s_jpeg_last_tap_tick) <= pdMS_TO_TICKS(JPEG_DOUBLE_TAP_MS) &&
                              abs_dx <= JPEG_DOUBLE_TAP_DIST && abs_dy <= JPEG_DOUBLE_TAP_DIST;
        s_jpeg_last_tap_tick = now;
        s_jpeg_last_tap_x    = x;
        s_jpeg_last_tap_y    = y;
        if (is_double_tap && s_jpeg_zoom > 1.0f) {
            s_jpeg_zoom   = 1.0f;
            s_jpeg_pan_cx = 0.5f;
            s_jpeg_pan_cy = 0.5f;
            pf_redraw_jpeg_view();
            s_jpeg_last_tap_tick = 0; /* don't chain into a 3rd tap */
            return true;
        }
    }
    if (s_jpeg_rgb565 && s_jpeg_zoom > 1.0f &&
            (gesture == SCREEN_GESTURE_SWIPE_LEFT || gesture == SCREEN_GESTURE_SWIPE_RIGHT ||
             gesture == SCREEN_GESTURE_SWIPE_UP   || gesture == SCREEN_GESTURE_SWIPE_DOWN)) {
        /* While zoomed in, single-finger swipes pan the view instead of
         * navigating to the next/previous image in the folder. */
        float step = 0.25f / s_jpeg_zoom;
        switch (gesture) {
            case SCREEN_GESTURE_SWIPE_LEFT:  s_jpeg_pan_cx += step; break;
            case SCREEN_GESTURE_SWIPE_RIGHT: s_jpeg_pan_cx -= step; break;
            case SCREEN_GESTURE_SWIPE_UP:    s_jpeg_pan_cy += step; break;
            case SCREEN_GESTURE_SWIPE_DOWN:  s_jpeg_pan_cy -= step; break;
            default: break;
        }
        float half = 0.5f / s_jpeg_zoom;
        if (s_jpeg_pan_cx < half)        s_jpeg_pan_cx = half;
        if (s_jpeg_pan_cx > 1.0f - half) s_jpeg_pan_cx = 1.0f - half;
        if (s_jpeg_pan_cy < half)        s_jpeg_pan_cy = half;
        if (s_jpeg_pan_cy > 1.0f - half) s_jpeg_pan_cy = 1.0f - half;
        pf_redraw_jpeg_view();
        return true;
    }
#endif
    if (s_jpeg_folder_display_active &&
            (gesture == SCREEN_GESTURE_SWIPE_LEFT || gesture == SCREEN_GESTURE_SWIPE_RIGHT)) {
        int folder_idx = s_jpeg_folder_display_idx;
        if (folder_idx >= 0 && (size_t)folder_idx < s_jpeg_folder_count) {
            /* Scan the directory fresh so newly saved files are always reachable. */
            char tmp[MP3_MAX_FILE_LEN] = {0};
            int actual_total = 0;
            jpeg_get_nth_file(s_jpeg_folders[folder_idx].folder_path,
                              0, tmp, sizeof(tmp), &actual_total);
            s_jpeg_folders[folder_idx].jpeg_count = actual_total; /* keep in sync */
            if (actual_total > 1) {
                int new_idx;
                if (gesture == SCREEN_GESTURE_SWIPE_LEFT) {
                    new_idx = (s_jpeg_folder_image_idx + 1) % actual_total;
                } else {
                    new_idx = (s_jpeg_folder_image_idx - 1 + actual_total) % actual_total;
                }
                char file_name[MP3_MAX_FILE_LEN] = {0};
                if (jpeg_get_nth_file(s_jpeg_folders[folder_idx].folder_path,
                                      new_idx, file_name, sizeof(file_name), NULL)
                        && file_name[0]) {
                    char file_path[MP3_MAX_PATH_LEN + MP3_MAX_FILE_LEN + 4];
                    snprintf(file_path, sizeof(file_path), "%s/%s",
                             s_jpeg_folders[folder_idx].folder_path, file_name);
                    s_jpeg_folder_image_idx = new_idx;
                    strncpy(s_pending_jpeg_file_path, file_path,
                            sizeof(s_pending_jpeg_file_path) - 1);
                    s_pending_jpeg_file_path[sizeof(s_pending_jpeg_file_path) - 1] = '\0';
                    s_pending_jpeg_file = true;
                }
            }
        }
        return true;
    }
    if (!s_mp3.active || !s_mp3_ui_override_allowed) return false;

    s_mp3_ui_override_allowed = true;
    bool handled = false;

    switch (gesture) {
        case SCREEN_GESTURE_TAP:
            if (s_mp3.paused) {
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
            } else {
                s_mp3.paused = true;
                s_mp3_resume_on_bt_reconnect = false;
#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
                s_bt_media_prime_pending = false;
                s_bt_media_prime_deadline = 0;
                s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
                bt_media_stop_if_needed();
#endif
            }
            handled = true;
            break;
        case SCREEN_GESTURE_SWIPE_LEFT:
            /* Right-to-left: restart the current song, unless we're still in
             * the first 5 seconds, in which case go to the previous song. */
            if (s_mp3.position_ms < 5000) {
                handled = mp3_advance_track(-1, "swipe previous");
            } else {
                handled = mp3_start_track(s_mp3.folder_idx, s_mp3.track_idx, false);
                if (handled) {
                    ESP_LOGI(TAG, "mp3: swipe restart -> track %d", s_mp3.track_idx + 1);
                }
            }
            break;
        case SCREEN_GESTURE_SWIPE_RIGHT:
            handled = mp3_advance_track(1, "swipe next");
            break;
        case SCREEN_GESTURE_SWIPE_UP:
            s_mp3.muted = false;
            s_mp3.volume += 5;
            if (s_mp3.volume > 100) s_mp3.volume = 100;
            nvs_write_u8(NVS_KEY_VOLUME, (uint8_t)s_mp3.volume);
            handled = true;
            break;
        case SCREEN_GESTURE_SWIPE_DOWN:
            s_mp3.muted = false;
            s_mp3.volume -= 5;
            if (s_mp3.volume < 0) s_mp3.volume = 0;
            nvs_write_u8(NVS_KEY_VOLUME, (uint8_t)s_mp3.volume);
            handled = true;
            break;
        case SCREEN_GESTURE_LONG_PRESS:
            /* Handled earlier (menu open/close); never reached. */
            break;
    }

    if (handled) {
        mp3_request_ui_refresh();
    }
    return handled;
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

    s_mp3_ui_override_allowed = true;
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

#if CONFIG_CORE2_HW
#define CORE2_AXP_I2C_NUM   I2C_NUM_0
#define CORE2_AXP_I2C_ADDR  0x34

static esp_err_t core2_axp_read_reg(uint8_t reg, uint8_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    /* portMAX_DELAY: BT A2DP + WiFi coexistence can delay the I2C ISR 50+ ms.
     * A finite timeout triggers i2c_hw_fsm_reset which deadlocks with the ISR
     * on the driver spinlock → INT WDT crash (same fix applied in touch_read_point). */
    return i2c_master_write_read_device(CORE2_AXP_I2C_NUM,
                                        CORE2_AXP_I2C_ADDR,
                                        &reg, 1,
                                        out, 1,
                                        portMAX_DELAY);
}

static esp_err_t core2_axp_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(CORE2_AXP_I2C_NUM,
                                      CORE2_AXP_I2C_ADDR,
                                      buf, sizeof(buf),
                                      portMAX_DELAY);
}

/* AXP192 reg 0x00 bit2: battery charge current direction (1 = charging
 * current flowing into the battery, i.e. external power is connected). */
static bool core2_axp_on_external_power(void)
{
    uint8_t reg0 = 0;
    if (core2_axp_read_reg(0x00, &reg0) != ESP_OK) return false;
    return (reg0 & 0x04) != 0;
}

/* For idle-sleep purposes, also treat a near-full battery as "on power":
 * once the battery is full the AXP192 stops sourcing charge current, so
 * core2_axp_on_external_power() drops to 0 even with USB still connected.
 * A battery that's actually discharging on its own won't sit at >=95%. */
static bool core2_axp_treat_as_powered(void)
{
    if (core2_axp_on_external_power()) return true;

    uint8_t vbat_h = 0, vbat_l = 0;
    if (core2_axp_read_reg(0x78, &vbat_h) != ESP_OK ||
        core2_axp_read_reg(0x79, &vbat_l) != ESP_OK) {
        return false;
    }
    int adc   = ((int)vbat_h << 4) | ((int)vbat_l & 0x0F);
    int vbat  = (int)((float)adc * 1.1f);
    int level = (vbat - 3300) * 100 / 900;
    return level >= 95;
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

/* ── Core2 voice query (Whisper STT → TRIGGERcmd Chat API) ──────────────── */

static bool json_extract_str(const char *json, const char *key,
                              char *out, size_t out_sz);   /* defined later */

#define CORE2_STT_URL       "https://api.openai.com/v1/audio/transcriptions"
#define CORE2_STT_KEY_MAX   256
#define CORE2_TRANSCRIPT_MAX 512
#define CORE2_STT_MAX_BODY  2048
#define CORE2_STT_TIMEOUT_MS 30000
#define NVS_KEY_STT         "stt_key"
#define NVS_KEY_VOICE_CONV  "voice_conv"

static char s_voice_conv_id[64] = {0};   /* conversation context across queries */

/* Read the OpenAI key: SD-only mode takes priority over NVS. */
static bool read_openai_key(char *buf, size_t len)
{
    if (s_sd_openai_key[0]) {
        strncpy(buf, s_sd_openai_key, len - 1);
        buf[len - 1] = '\0';
        return true;
    }
    return nvs_read_str(NVS_KEY_STT, buf, len) && buf[0];
}

static bool core2_stt_transcribe(const uint8_t *wav, size_t wav_len,
                                 const char *api_key,
                                 char *transcript_out)
{
    transcript_out[0] = '\0';

    static const char BOUNDARY[]    = "----TCMDCore2Boundary8a4b1c";
    static const char MODEL_PART[]  =
        "------TCMDCore2Boundary8a4b1c\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "whisper-1\r\n";
    static const char FILE_PART_HDR[] =
        "------TCMDCore2Boundary8a4b1c\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    static const char FINAL_BOUNDARY[] =
        "\r\n------TCMDCore2Boundary8a4b1c--\r\n";

    size_t total_len = strlen(MODEL_PART) + strlen(FILE_PART_HDR)
                     + wav_len + strlen(FINAL_BOUNDARY);

    char bearer[CORE2_STT_KEY_MAX + 10];
    snprintf(bearer, sizeof(bearer), "Bearer %s", api_key);

    char content_type[80];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", BOUNDARY);

    bool ok = false;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
        esp_http_client_config_t cfg = {
            .url               = CORE2_STT_URL,
            .method            = HTTP_METHOD_POST,
            .timeout_ms        = CORE2_STT_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size       = 4096,
            .buffer_size_tx    = 4096,
            .keep_alive_enable = false,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) break;

        esp_http_client_set_header(client, "Authorization", bearer);
        esp_http_client_set_header(client, "Content-Type", content_type);

        if (esp_http_client_open(client, (int)total_len) != ESP_OK) {
            esp_http_client_cleanup(client);
            if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        int sent = 0;
        bool write_ok = true;
        int n;
        n = esp_http_client_write(client, MODEL_PART, (int)strlen(MODEL_PART));
        if (n > 0) sent += n; else write_ok = false;
        n = esp_http_client_write(client, FILE_PART_HDR, (int)strlen(FILE_PART_HDR));
        if (n > 0) sent += n; else write_ok = false;
        n = esp_http_client_write(client, (const char *)wav, (int)wav_len);
        if (n > 0) sent += n; else write_ok = false;
        n = esp_http_client_write(client, FINAL_BOUNDARY, (int)strlen(FINAL_BOUNDARY));
        if (n > 0) sent += n; else write_ok = false;

        if (write_ok && sent == (int)total_len) {
            int64_t cl = esp_http_client_fetch_headers(client);
            int max_resp = CORE2_STT_MAX_BODY;
            if (cl > 0 && cl < max_resp) max_resp = (int)cl;
            char *resp = malloc(max_resp + 1);
            if (resp) {
                int total = 0;
                while (total < max_resp) {
                    n = esp_http_client_read(client, resp + total, max_resp - total);
                    if (n <= 0) break;
                    total += n;
                }
                resp[total] = '\0';
                int status = esp_http_client_get_status_code(client);
                ESP_LOGI(TAG, "STT HTTP %d: %.120s", status, resp);
                if (status == 200) {
                    ok = json_extract_str(resp, "text", transcript_out, CORE2_TRANSCRIPT_MAX);
                }
                free(resp);
            }
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (!ok && attempt < 3) vTaskDelay(pdMS_TO_TICKS(250));
    }
    return ok;
}

#define CORE2_TTS_URL        "https://api.openai.com/v1/audio/speech"
#define CORE2_TTS_MAX_BODY   (256 * 1024)
#define CORE2_TTS_TIMEOUT_MS 30000
#define TTS_TEMP_FILE        "/sdcard/tmp_tts.mp3"

static bool core2_tts_speak(const char *text)
{
    if (!text || !text[0]) return false;

    /* Build JSON body with text escaped for embedding in a JSON string */
    size_t text_len = strlen(text);
    char *json = malloc(text_len * 2 + 64);
    if (!json) return false;
    {
        static const char prefix[] = "{\"model\":\"tts-1\",\"input\":\"";
        static const char suffix[] = "\",\"voice\":\"alloy\"}";
        char *p = json;
        memcpy(p, prefix, sizeof(prefix) - 1); p += sizeof(prefix) - 1;
        for (const char *t = text; *t; t++) {
            if (*t == '"' || *t == '\\') { *p++ = '\\'; *p++ = *t; }
            else if (*t == '\n')          { *p++ = '\\'; *p++ = 'n'; }
            else if (*t == '\r')          { /* skip */ }
            else                          { *p++ = *t; }
        }
        memcpy(p, suffix, sizeof(suffix)); /* includes NUL */
    }

    /* Suspend mp3 task and pause I2S while making HTTP call */
    if (s_mp3.active && !s_mp3.paused) s_mp3.paused = true;
    if (s_mp3_task) vTaskSuspend(s_mp3_task);
    core2_audio_pause();

    char api_key[CORE2_STT_KEY_MAX] = {0};
    bool ok = false;
    char *mp3_buf = NULL;
    int mp3_len = 0;

    if (!read_openai_key(api_key, sizeof(api_key))) {
        ESP_LOGW(TAG, "TTS: no API key configured");
        screen_draw_text("TTS: no API key\nVisit device IP\nto configure");
        goto tts_done;
    }

    {
        char bearer[CORE2_STT_KEY_MAX + 10];
        snprintf(bearer, sizeof(bearer), "Bearer %s", api_key);
        int body_len = (int)strlen(json);

        esp_http_client_config_t cfg = {
            .url               = CORE2_TTS_URL,
            .method            = HTTP_METHOD_POST,
            .timeout_ms        = CORE2_TTS_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size       = 4096,
            .buffer_size_tx    = 4096,
            .keep_alive_enable = false,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) goto tts_done;

        esp_http_client_set_header(client, "Authorization", bearer);
        esp_http_client_set_header(client, "Content-Type", "application/json");

        if (esp_http_client_open(client, body_len) != ESP_OK) {
            esp_http_client_cleanup(client);
            goto tts_done;
        }
        esp_http_client_write(client, json, body_len);

        int64_t cl = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "TTS: HTTP %d content-length=%lld", status, (long long)cl);

        if (status == 200) {
            int max_body = CORE2_TTS_MAX_BODY;
            if (cl > 0 && cl < max_body) max_body = (int)cl;
            mp3_buf = malloc(max_body);
            if (mp3_buf) {
                int total = 0, n;
                while (total < max_body) {
                    n = esp_http_client_read(client, mp3_buf + total, max_body - total);
                    if (n <= 0) break;
                    total += n;
                }
                mp3_len = total;
                ok = (mp3_len > 0);
            }
        } else {
            ESP_LOGW(TAG, "TTS: unexpected HTTP %d", status);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    if (ok) {
        FILE *fp = fopen(TTS_TEMP_FILE, "wb");
        if (fp) {
            fwrite(mp3_buf, 1, (size_t)mp3_len, fp);
            fclose(fp);
            s_mp3.active        = true;
            s_mp3.paused        = false;
            s_mp3.folder_idx    = -1;   /* no folder — plays once then stops */
            s_mp3.track_idx     = -1;
            s_mp3.duration_ms   = 60000;
            s_mp3.position_ms   = 0;
            s_mp3.muted         = false;
            s_mp3.last_tick     = xTaskGetTickCount();
            s_mp3.play_token++;
            s_mp3_ui_override_allowed = false;
            snprintf(s_mp3.file_path, sizeof(s_mp3.file_path), TTS_TEMP_FILE);
            s_mp3.folder_name[0] = '\0';
            snprintf(s_mp3.file_name, sizeof(s_mp3.file_name), "tts");
            ESP_LOGI(TAG, "TTS: %d bytes -> playback started", mp3_len);
        } else {
            ESP_LOGW(TAG, "TTS: fopen(%s) failed — SD card present?", TTS_TEMP_FILE);
            screen_draw_text("TTS: no SD card");
            ok = false;
        }
    }

tts_done:
    free(json);
    free(mp3_buf);
    core2_audio_resume();
    if (s_mp3_task) vTaskResume(s_mp3_task);
    return ok;
}

static int core2_https_post_json(const char *url, const char *token,
                                 const char *json_body, char **resp_out)
{
    if (resp_out) *resp_out = NULL;
    char bearer[HW_TOKEN_MAX_LEN + 10];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);
    int body_len = (int)strlen(json_body);

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_http_client_set_header(client, "Authorization", bearer);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");

    if (esp_http_client_open(client, body_len) != ESP_OK) {
        esp_http_client_cleanup(client);
        return -1;
    }
    esp_http_client_write(client, json_body, body_len);

    int64_t cl = esp_http_client_fetch_headers(client);
    int max_body = CORE2_STT_MAX_BODY;
    if (cl > 0 && cl < max_body) max_body = (int)cl;

    if (resp_out) {
        char *buf = malloc(max_body + 1);
        if (buf) {
            int total = 0, n;
            while (total < max_body) {
                n = esp_http_client_read(client, buf + total, max_body - total);
                if (n <= 0) break;
                total += n;
            }
            buf[total] = '\0';
            *resp_out = buf;
        }
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

static void do_core2_voice_query(void)
{
    ESP_LOGI(TAG, "Voice query: starting");
    pf_clock_stop();
    if (s_mp3_saved_font_scale >= 0) {
        screen_set_font_scale_silent(s_mp3_saved_font_scale);
        s_mp3_saved_font_scale = -1;
    }
    screen_draw_text("Listening...");

    /* Suspend the mp3 player task and pause I2S before recording.
     * Both stay suspended/paused for the ENTIRE voice query — recording AND
     * all HTTP calls (STT + Chat API) — so mp3_play and core2_i2s do not
     * compete with the WiFi/TLS stack on CPU0, which would cause TLS failures
     * and task WDT warnings.  Audio hardware and mp3 task are restored at the
     * single exit point at the bottom after all network work is done. */
    if (s_mp3.active && !s_mp3.paused) s_mp3.paused = true;
    if (s_mp3_task) vTaskSuspend(s_mp3_task);
    core2_audio_pause();

    uint8_t *wav    = NULL;
    size_t   wav_len = 0;
    if (s_mic_src_grove) {
        if (core2_adc_mic_init() == ESP_OK) {
            wav_len = core2_adc_mic_record(&wav, 4000);
            core2_adc_mic_deinit();
        } else {
            ESP_LOGW(TAG, "Voice query: ADC mic init failed");
            screen_draw_text("Voice: mic init\nfailed");
        }
    } else {
        if (core2_mic_init() == ESP_OK) {
            wav_len = core2_mic_record(&wav, 4000);
            core2_mic_deinit();
        } else {
            ESP_LOGW(TAG, "Voice query: PDM mic init failed (DMA alloc)");
            screen_draw_text("Voice: mic init\nfailed");
        }
    }
    /* I2S1 stays disabled; mp3 task stays suspended. */

    /* ── All HTTP work happens here with audio fully quiescent ──────────── */

    char stt_key[CORE2_STT_KEY_MAX] = {0};
    char transcript[CORE2_TRANSCRIPT_MAX] = {0};
    char *resp = NULL;

    if (wav_len == 0 || !wav) {
        ESP_LOGW(TAG, "Voice query: no audio captured");
        screen_draw_text("Voice: no audio\ncaptured");
        goto voice_done;
    }

    screen_draw_text("Processing...");

    if (!read_openai_key(stt_key, sizeof(stt_key))) {
        ESP_LOGW(TAG, "Voice query: no STT API key");
        screen_draw_text("Voice: no API key\nVisit device IP\nto configure");
        goto voice_done;
    }

    if (!core2_stt_transcribe(wav, wav_len, stt_key, transcript) || !transcript[0]) {
        ESP_LOGW(TAG, "Voice query: STT failed or empty");
        screen_draw_text("Voice: not heard\nor STT failed");
        goto voice_done;
    }
    ESP_LOGI(TAG, "Voice transcript: %s", transcript);

    {
        char msg[96];
        snprintf(msg, sizeof(msg), "Heard:\n%.88s", transcript);
        screen_draw_text(msg);
    }

    {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char computer_name[32];
        snprintf(computer_name, sizeof(computer_name),
                 "TCMDCORE2-%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        char json_body[CORE2_TRANSCRIPT_MAX + 256];
        if (s_voice_conv_id[0]) {
            snprintf(json_body, sizeof(json_body),
                     "{\"message\":\"%s\",\"conversationId\":\"%s\",\"computerName\":\"%s\"}",
                     transcript, s_voice_conv_id, computer_name);
        } else {
            snprintf(json_body, sizeof(json_body),
                     "{\"message\":\"%s\",\"computerName\":\"%s\"}",
                     transcript, computer_name);
        }
        ESP_LOGI(TAG, "Chat API body: %s", json_body);

        char chat_url[128];
        snprintf(chat_url, sizeof(chat_url), "%s/api/v1/chat/message", TCMD_BASE_URL);

        int status = core2_https_post_json(chat_url, s_hw_token, json_body, &resp);
        ESP_LOGI(TAG, "Chat API HTTP %d  resp: %.200s", status, resp ? resp : "(none)");

        if (status == 200 && resp) {
            char new_conv[64] = {0};
            json_extract_str(resp, "conversationId", new_conv, sizeof(new_conv));
            if (new_conv[0] && strcmp(new_conv, s_voice_conv_id) != 0) {
                strncpy(s_voice_conv_id, new_conv, sizeof(s_voice_conv_id) - 1);
                nvs_write_str(NVS_KEY_VOICE_CONV, s_voice_conv_id);
            }
            ESP_LOGI(TAG, "Voice query: chat API success");
        } else {
            ESP_LOGW(TAG, "Voice query: chat API returned HTTP %d", status);
            screen_draw_text("Voice: chat API\nfailed");
        }
    }

voice_done:
    /* Restore audio hardware and mp3 player task AFTER all HTTP is done.
     * Do NOT restore s_mp3.paused — the command that arrived via Socket.IO
     * during the query has already set the correct audio state. */
    if (wav) { free(wav); }
    if (resp) { free(resp); }
    core2_audio_resume();
    if (s_mp3_task) vTaskResume(s_mp3_task);
    s_pending_vibrate = true;
}

/* ── AXP192 PEK (power key) short-press detection ───────────────────────── */

static void core2_poll_pwr_key(void)
{
    uint8_t irq = 0;
    if (core2_axp_read_reg(0x46, &irq) != ESP_OK) return;
    if (!(irq & 0x02)) return;                    /* bit1 = PEK short press */
    (void)core2_axp_write_reg(0x46, 0x02);        /* clear bit by writing 1 */
    ESP_LOGI(TAG, "PWR key short press detected — starting voice query");
    do_core2_voice_query();
}
#endif /* CONFIG_CORE2_HW */

static int mp3_find_folder_trigger(const char *trigger)
{
    for (size_t i = 0; i < s_mp3_folder_count; i++) {
        if (strcasecmp(trigger, s_mp3_folders[i].trigger) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int jpeg_find_folder_trigger(const char *trigger)
{
    for (size_t i = 0; i < s_jpeg_folder_count; i++) {
        if (strcasecmp(trigger, s_jpeg_folders[i].trigger) == 0) {
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

#if CONFIG_CORE2_HW
    /* Core2: SPI3 is already initialized for the LCD; SD CS = GPIO4.
     * Keep CS high and strengthen marginal bus lines. */
    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_4, 1);
    gpio_pullup_en(GPIO_NUM_4);
    gpio_pullup_en(GPIO_NUM_18);
    gpio_pullup_en(GPIO_NUM_23);
    gpio_set_drive_capability(GPIO_NUM_4, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(GPIO_NUM_18, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(GPIO_NUM_23, GPIO_DRIVE_CAP_3);
#elif CONFIG_HARDWARE_CYD
    /* CYD (ESP32-2432S028R): onboard microSD slot is on its own SPI bus
     * (SPI2/HSPI), separate from the LCD's SPI3. Pins per the common
     * ESP32-2432S028R schematic: MOSI=23, MISO=19, SCLK=18, CS=5. */
    gpio_set_direction(GPIO_NUM_5, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_5, 1);
    {
        spi_bus_config_t sd_bus = {
            .mosi_io_num   = GPIO_NUM_23,
            .miso_io_num   = GPIO_NUM_19,
            .sclk_io_num   = GPIO_NUM_18,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = 4096,
        };
        esp_err_t bus_err = spi_bus_initialize(SPI2_HOST, &sd_bus, SPI_DMA_CH_AUTO);
        if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "sd: SPI2 init failed: %s", esp_err_to_name(bus_err));
            if (spi_locked) screen_spi_unlock();
            return false;
        }
    }
#else
    /* JC3248W535: built-in TF slot on dedicated SPI3 pins (schematic SD_Car block).
     * SPI3 is not used by the LCD (which is on SPI2), so initialize it here. */
    gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_10, 1);
    {
        spi_bus_config_t sd_bus = {
            .mosi_io_num   = GPIO_NUM_11,
            .miso_io_num   = GPIO_NUM_13,
            .sclk_io_num   = GPIO_NUM_12,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = 4096,
        };
        esp_err_t bus_err = spi_bus_initialize(SPI3_HOST, &sd_bus, SPI_DMA_CH_AUTO);
        if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "sd: SPI3 init failed: %s", esp_err_to_name(bus_err));
            if (spi_locked) screen_spi_unlock();
            return false;
        }
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(5));

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

#if CONFIG_HARDWARE_CYD
    const spi_host_device_t host_candidates[] = {SPI2_HOST};
#else
    const spi_host_device_t host_candidates[] = {SPI3_HOST};
#endif
    const int freq_candidates_khz[] = {4000, 1000, 400, 200};
    esp_err_t err = ESP_FAIL;
#if CONFIG_CORE2_HW
    const int power_passes = 2;
#else
    const int power_passes = 1;
#endif

    for (int pass = 0; pass < power_passes && err != ESP_OK; pass++) {
#if CONFIG_CORE2_HW
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
#if CONFIG_CORE2_HW
            slot_config.gpio_cs = GPIO_NUM_4;
#elif CONFIG_HARDWARE_CYD
            slot_config.gpio_cs = GPIO_NUM_5;  /* CYD TF_CS */
#else
            slot_config.gpio_cs = GPIO_NUM_10;  /* JC3248W535 TF_CS */
#endif
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
        if (spi_locked) {
            screen_spi_unlock();
            spi_locked = false;
            screen_reinit_display();  /* core2_cycle_sd_power() cut LDO2; must reinit LCD */
        }
        /* Always redraw after reinit — GRAM is wiped by the LDO2 power cycle. */
        pf_status_draw("No SD card\nDevice online\nMP3 unavailable");
        s_sd_mount_warned = true;
        return false;
    }

    s_sd_mounted = true;
    s_sd_mount_warned = false;
    s_mp3_next_mount_retry = 0;
    if (spi_locked) {
        screen_spi_unlock();
    }
    ESP_LOGI(TAG, "sd: mounted at %s", MP3_ROOT_PATH);
    screen_reinit_display();
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

static void rebuild_jpeg_folder_index(void)
{
    s_jpeg_folder_count = 0;

    if (!mount_sd_card_if_needed()) return;

    DIR *d = opendir(MP3_ROOT_PATH);
    if (!d) {
        ESP_LOGW(TAG, "sd: cannot open root %s for jpeg scan", MP3_ROOT_PATH);
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (s_jpeg_folder_count >= MP3_MAX_FOLDERS) break;
        if (trigger_reserved(e->d_name)) continue;

        char folder_path[MP3_MAX_PATH_LEN];
        int max_name = (int)sizeof(folder_path) - (int)strlen(MP3_ROOT_PATH) - 2;
        if (max_name <= 0) continue;
        int n = snprintf(folder_path, sizeof(folder_path), "%s/%.*s",
                 MP3_ROOT_PATH, max_name, e->d_name);
        if (n <= 0 || n >= (int)sizeof(folder_path)) continue;

        struct stat st;
        if (stat(folder_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        int jpeg_count = jpeg_count_in_folder(folder_path);
        if (jpeg_count <= 0) continue;

        jpeg_folder_t *f = &s_jpeg_folders[s_jpeg_folder_count++];
        strncpy(f->trigger, e->d_name, sizeof(f->trigger) - 1);
        f->trigger[sizeof(f->trigger) - 1] = '\0';
        strncpy(f->folder_path, folder_path, sizeof(f->folder_path) - 1);
        f->folder_path[sizeof(f->folder_path) - 1] = '\0';
        f->jpeg_count = jpeg_count;
    }
    closedir(d);

    ESP_LOGI(TAG, "jpeg: discovered %u folders with jpeg content", (unsigned)s_jpeg_folder_count);
}

/* Encode embedded newline bytes as the two-char sequence \n for line-based storage. */
static void text_encode_newlines(const char *src, char *dst, size_t dst_sz)
{
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < dst_sz; i++) {
        if (src[i] == '\n') {
            if (out + 2 >= dst_sz) break;
            dst[out++] = '\\';
            dst[out++] = 'n';
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

/* Decode two-char \n sequence back to a newline byte. */
static void text_decode_newlines(const char *src, char *dst, size_t dst_sz)
{
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < dst_sz; i++) {
        if (src[i] == '\\' && src[i + 1] == 'n') {
            dst[out++] = '\n';
            i++;
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

/* Apply a fully-populated display state — shared by SD and NVS restore paths. */
static void apply_restored_display_state(uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                                          uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                                          uint8_t orient, uint8_t font_scale,
                                          const char *text, uint8_t mp3_mode,
                                          const char *jpeg_url)
{
    /* Guard against invisible text when saved fg/bg are identical or near-identical. */
    {
        int dr = (int)fg_r - (int)bg_r; if (dr < 0) dr = -dr;
        int dg = (int)fg_g - (int)bg_g; if (dg < 0) dg = -dg;
        int db = (int)fg_b - (int)bg_b; if (db < 0) db = -db;
        if (dr < 24 && dg < 24 && db < 24) {
            int luma = ((int)bg_r * 299 + (int)bg_g * 587 + (int)bg_b * 114) / 1000;
            if (luma >= 128) { fg_r = fg_g = fg_b = 0; }
            else             { fg_r = fg_g = fg_b = 255; }
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

    if (mp3_mode) {
        if (s_sd_mounted && s_mp3_folder_count > 0) {
            mp3_start_track(0, -1, false);
            ESP_LOGI(TAG, "Restored saved display state (music mode — started immediately)");
        } else {
            s_mp3_autostart = true;
            ESP_LOGI(TAG, "Restored saved display state (music mode — autostart pending SD mount)");
        }
        return;
    }

    if (jpeg_url[0]) {
        strncpy(s_current_jpeg_url, jpeg_url, sizeof(s_current_jpeg_url) - 1);
        s_current_jpeg_url[sizeof(s_current_jpeg_url) - 1] = '\0';
        if (jpeg_url[0] == '/') {
            strncpy(s_pending_jpeg_file_path, jpeg_url, sizeof(s_pending_jpeg_file_path) - 1);
            s_pending_jpeg_file_path[sizeof(s_pending_jpeg_file_path) - 1] = '\0';
            s_pending_jpeg_file = true;
        } else {
            strncpy(s_pending_jpeg_url, jpeg_url, sizeof(s_pending_jpeg_url) - 1);
            s_pending_jpeg_url[sizeof(s_pending_jpeg_url) - 1] = '\0';
            s_pending_jpeg = true;
        }
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
}

/* Write display settings to SD card as a plain-text key=value file.
 * Called alongside NVS save so settings survive a reflash or card swap. */
static bool save_display_state_to_sd(void)
{
    if (!mount_sd_card_if_needed()) return false;

    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    uint8_t fg_r = 255, fg_g = 255, fg_b = 255;
    bool landscape = false;
    int font_scale = 2;
    screen_get_color(&bg_r, &bg_g, &bg_b);
    screen_get_text_color(&fg_r, &fg_g, &fg_b);
    screen_get_landscape(&landscape);
    screen_get_font_scale(&font_scale);

    bool music_active = s_mp3.active && s_mp3_ui_override_allowed;
    const char *jpeg_url = "";
    if (!music_active && s_jpeg_cache && s_jpeg_cache_len > 0 && s_current_jpeg_url[0]) {
        jpeg_url = s_current_jpeg_url;
    }

    char text_enc[sizeof(s_last_text) * 2 + 1];
    text_encode_newlines(s_last_text, text_enc, sizeof(text_enc));

    FILE *f = fopen(SD_SETTINGS_PATH, "w");
    if (!f) {
        ESP_LOGW(TAG, "save_sd: cannot create %s", SD_SETTINGS_PATH);
        return false;
    }
    fprintf(f, "bg=%u,%u,%u\n",  bg_r, bg_g, bg_b);
    fprintf(f, "fg=%u,%u,%u\n",  fg_r, fg_g, fg_b);
    fprintf(f, "orient=%u\n",    (unsigned)(landscape ? 1 : 0));
    fprintf(f, "font=%d\n",      font_scale);
    fprintf(f, "mp3=%u\n",       (unsigned)(music_active ? 1 : 0));
    fprintf(f, "jpeg=%s\n",      jpeg_url);
    fprintf(f, "text=%s\n",      text_enc);
#if CONFIG_CORE2_HW
    fprintf(f, "clock=%u\n",     (unsigned)s_clock_mode);
    {
        uint8_t bs = (s_clock_mode == PF_CLOCK_ANALOG)  ? 2 :
                     (s_clock_mode == PF_CLOCK_DIGITAL)  ? 1 :
                     music_active                         ? 3 :
                     (jpeg_url[0])                        ? 4 : 5;
        fprintf(f, "boot_show=%u\n", (unsigned)bs);
    }
    fprintf(f, "timezone=%s\n", s_timezone);
#endif
    fclose(f);
    ESP_LOGI(TAG, "save_sd: settings written to %s", SD_SETTINGS_PATH);
    return true;
}

/* Try to read display settings from the SD card.  Returns true and applies
 * the state when the file is found; returns false if no SD or no file. */
static bool restore_display_state_from_sd(void)
{
    if (!mount_sd_card_if_needed()) return false;

    FILE *f = fopen(SD_SETTINGS_PATH, "r");
    if (!f) return false;

    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    uint8_t fg_r = 255, fg_g = 255, fg_b = 255;
    uint8_t orient = 0, font_scale = 2, mp3_mode = 0;
#if CONFIG_CORE2_HW
    uint8_t clock_mode_sd = 0;
    uint8_t boot_show_sd = 0;
    char    tz_sd[sizeof(s_timezone)] = {0};
#endif
    /* Encode buffer: worst case every char in s_last_text is a newline (2× size) */
    char text_enc[sizeof(s_last_text) * 2 + 1] = {0};
    char jpeg_url[sizeof(s_current_jpeg_url)] = {0};

    /* Line buffer: big enough for the text= line (encoded + key + newline) */
    char line[sizeof(s_last_text) * 2 + 16];
    while (fgets(line, (int)sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        unsigned int a, b, c;
        if      (sscanf(line, "bg=%u,%u,%u", &a, &b, &c) == 3) {
            bg_r = (uint8_t)a; bg_g = (uint8_t)b; bg_b = (uint8_t)c;
        } else if (sscanf(line, "fg=%u,%u,%u", &a, &b, &c) == 3) {
            fg_r = (uint8_t)a; fg_g = (uint8_t)b; fg_b = (uint8_t)c;
        } else if (sscanf(line, "orient=%u", &a) == 1) {
            orient = (uint8_t)a;
        } else if (sscanf(line, "font=%u",   &a) == 1) {
            font_scale = (uint8_t)a;
        } else if (sscanf(line, "mp3=%u",    &a) == 1) {
            mp3_mode = (uint8_t)a;
#if CONFIG_CORE2_HW
        } else if (sscanf(line, "clock=%u",     &a) == 1) {
            clock_mode_sd = (uint8_t)a;
        } else if (sscanf(line, "boot_show=%u", &a) == 1) {
            boot_show_sd = (uint8_t)a;
        } else if (strncmp(line, "timezone=", 9) == 0) {
            strncpy(tz_sd, line + 9, sizeof(tz_sd) - 1);
            tz_sd[sizeof(tz_sd) - 1] = '\0';
#endif
        } else if (strncmp(line, "jpeg=", 5) == 0) {
            strncpy(jpeg_url, line + 5, sizeof(jpeg_url) - 1);
            jpeg_url[sizeof(jpeg_url) - 1] = '\0';
        } else if (strncmp(line, "text=", 5) == 0) {
            strncpy(text_enc, line + 5, sizeof(text_enc) - 1);
            text_enc[sizeof(text_enc) - 1] = '\0';
        }
    }
    fclose(f);

    char text[sizeof(s_last_text)] = {0};
    text_decode_newlines(text_enc, text, sizeof(text));

    apply_restored_display_state(bg_r, bg_g, bg_b, fg_r, fg_g, fg_b,
                                  orient, font_scale, text, mp3_mode, jpeg_url);
#if CONFIG_CORE2_HW
    /* Also persist to NVS so the boot clock-start path picks it up uniformly. */
    nvs_write_u8(NVS_KEY_CLOCK_MODE, clock_mode_sd);
    nvs_write_u8(NVS_KEY_BOOT_SHOW, boot_show_sd);
    if (tz_sd[0]) {
        apply_timezone(tz_sd);
        nvs_write_str(NVS_KEY_TIMEZONE, s_timezone);
    }
#endif
    ESP_LOGI(TAG, "restore: display state loaded from SD card (%s)", SD_SETTINGS_PATH);
    return true;
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

    bool music_active = s_mp3.active && s_mp3_ui_override_allowed;
    if ((err = nvs_write_u8(NVS_KEY_MP3_MODE, music_active ? 1 : 0)) != ESP_OK) return err;

    if (music_active) {
        if ((err = nvs_erase_key_local(NVS_KEY_JPEGURL)) != ESP_OK) return err;
    } else if (s_jpeg_cache && s_jpeg_cache_len > 0 && s_current_jpeg_url[0]) {
        if ((err = nvs_write_str(NVS_KEY_JPEGURL, s_current_jpeg_url)) != ESP_OK) return err;
    } else {
        if ((err = nvs_erase_key_local(NVS_KEY_JPEGURL)) != ESP_OK) return err;
    }

#if CONFIG_CORE2_HW
    if ((err = nvs_write_u8(NVS_KEY_CLOCK_MODE, (uint8_t)s_clock_mode)) != ESP_OK) return err;
    {
        uint8_t boot_show = (s_clock_mode == PF_CLOCK_ANALOG)  ? 2 :
                            (s_clock_mode == PF_CLOCK_DIGITAL)  ? 1 :
                            music_active                         ? 3 :
                            (s_jpeg_cache && s_jpeg_cache_len > 0 && s_current_jpeg_url[0]) ? 4 : 5;
        if ((err = nvs_write_u8(NVS_KEY_BOOT_SHOW, boot_show)) != ESP_OK) return err;
    }
    if ((err = nvs_write_str(NVS_KEY_TIMEZONE, s_timezone)) != ESP_OK) return err;
#endif

    esp_err_t result = nvs_write_u8(NVS_KEY_SAVED, 1);
    if (result == ESP_OK) {
        save_display_state_to_sd();   /* best-effort; failures are logged and ignored */
    }
    return result;
}

static void restore_display_state_from_nvs(void)
{
    /* SD card takes priority — settings there survive reflashing or device swap. */
    if (restore_display_state_from_sd()) return;

    uint8_t saved = 0;
    if (!nvs_read_u8(NVS_KEY_SAVED, &saved) || saved != 1) {
        /* Nothing saved yet (e.g. right after pairing) — show the idle
         * status so the screen doesn't stay stuck on "Syncing commands...". */
        strncpy(s_last_text, "Connected!\nWaiting for\ncommands...", sizeof(s_last_text) - 1);
        s_last_text[sizeof(s_last_text) - 1] = '\0';
        screen_draw_text(s_last_text);
        return;
    }

    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
    uint8_t fg_r = 255, fg_g = 255, fg_b = 255;
    uint8_t orient = 0, font_scale = 2;
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
    nvs_read_u8(NVS_KEY_BG_R,  &bg_r);
    nvs_read_u8(NVS_KEY_BG_G,  &bg_g);
    nvs_read_u8(NVS_KEY_BG_B,  &bg_b);
    nvs_read_u8(NVS_KEY_FG_R,  &fg_r);
    nvs_read_u8(NVS_KEY_FG_G,  &fg_g);
    nvs_read_u8(NVS_KEY_FG_B,  &fg_b);
    nvs_read_u8(NVS_KEY_ORIENT, &orient);
    nvs_read_u8(NVS_KEY_FONT,   &font_scale);
    nvs_read_str(NVS_KEY_TEXT,    text,     sizeof(text));
    nvs_read_str(NVS_KEY_JPEGURL, jpeg_url, sizeof(jpeg_url));

    uint8_t mp3_mode = 0;
    nvs_read_u8(NVS_KEY_MP3_MODE, &mp3_mode);

    apply_restored_display_state(bg_r, bg_g, bg_b, fg_r, fg_g, fg_b,
                                  orient, font_scale, text, mp3_mode, jpeg_url);
    ESP_LOGI(TAG, "Restored saved display state from NVS");
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
    /* TLS: skip server cert verification for triggercmd.com — avoids the
     * mbedtls_x509_crt_parse heap cost of loading a CA cert on no-PSRAM
     * boards (relies on CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY, enabled for
     * all variants using this file); crt_bundle for other HTTPS hosts;
     * plain HTTP needs nothing. */
    if (strncmp(TCMD_BASE_URL, "https://", 8) == 0 &&
        !strstr(TCMD_BASE_URL, "triggercmd.com")) {
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
    int max_body = 65536;   /* command list: each record embeds full user object (~1.2 KB × 33+ cmds ≈ 40 KB) */
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
    if (strncmp(TCMD_BASE_URL, "https://", 8) == 0 &&
        !strstr(TCMD_BASE_URL, "triggercmd.com")) {
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
    /* TLS: skip server cert verification for triggercmd.com — avoids the
     * mbedtls_x509_crt_parse heap cost of loading a CA cert on no-PSRAM
     * boards (relies on CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY, enabled for
     * all variants using this file); crt_bundle for other HTTPS hosts;
     * plain HTTP needs nothing. */
    if (strncmp(TCMD_BASE_URL, "https://", 8) == 0 &&
        !strstr(TCMD_BASE_URL, "triggercmd.com")) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    /* DIAG: mbedtls_calloc (used by MBEDTLS_DYNAMIC_BUFFER) goes through
     * heap_caps_malloc_default(), which requires MALLOC_CAP_8BIT. On ESP32
     * (no PSRAM), spare IRAM is added to the MALLOC_CAP_INTERNAL pool as
     * 32-bit-only memory (no MALLOC_CAP_8BIT) — so the INTERNAL "largest"
     * figure can look much bigger than what calloc() can actually use.
     * Log both to see whether that's why alloc(5473) fails despite a large
     * INTERNAL block. */
    ESP_LOGI(TAG, "https_get_simple: heap before connect: free=%u largest=%u (8bit largest=%u)",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "https_get_simple open failed: %s (heap free=%u largest=%u 8bit largest=%u)",
                 esp_err_to_name(ret),
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
    /* extract key from the sub-object.  Sized to hold a full PF_ASK_ANSWER_MAX
     * answer string (with worst-case \" / \\ / \n escaping doubling its size)
     * plus the surrounding "role":"assistant","content":"...","refusal":null
     * wrapper from OpenAI's message object — 256 was too small and silently
     * truncated long askpic answers mid-word. */
    char sub[1536];
    size_t sub_len = (size_t)(end - p + 1);
    if (sub_len >= sizeof(sub)) sub_len = sizeof(sub) - 1;
    memcpy(sub, p, sub_len);
    sub[sub_len] = '\0';
    return json_extract_str(sub, key, out, out_sz);
}

#if !CONFIG_HARDWARE_CYD
#define PF_VISION_URL        "https://api.openai.com/v1/chat/completions"
#define PF_VISION_TIMEOUT_MS 60000
#define PF_VISION_MAX_RESP   4096
#define PF_ASK_ANSWER_MAX    512

/*
 * Reports an AI-generated answer back to TRIGGERcmd (JSON-escaped into
 * s_pending_result) and, on Core2 with aitts enabled, queues it for TTS on a
 * later main-loop tick (after the result POST, so blocking TTS doesn't push
 * the result past the server's 15s voiceReply window).
 */
static void pf_report_ai_result(const char *answer, const char *run_id)
{
    strncpy(s_pending_run_id, run_id, sizeof(s_pending_run_id) - 1);
    s_pending_run_id[sizeof(s_pending_run_id) - 1] = '\0';
    {
        char *out = s_pending_result;
        const char *end = s_pending_result + sizeof(s_pending_result) - 1;
        for (const char *a = answer; *a && out < end - 1; a++) {
            if (*a == '"' || *a == '\\') { *out++ = '\\'; *out++ = *a; }
            else if (*a == '\n')          { *out++ = '\\'; *out++ = 'n'; }
            else if (*a == '\r')          { /* skip */ }
            else                          { *out++ = *a; }
        }
        *out = '\0';
    }
    s_pending_has_result = true;
    s_pending_run = true;

#if CONFIG_CORE2_HW
    if (s_ai_tts_enabled) {
        strncpy(s_pending_speak_text, answer, sizeof(s_pending_speak_text) - 1);
        s_pending_speak_text[sizeof(s_pending_speak_text) - 1] = '\0';
        s_pending_speak = true;
    }
#endif
}

/* ── SD-card backup: upload every file to a user-configured HTTP(S) server ── */

#define PF_BACKUP_BOUNDARY "----TCMDCore2BackupBoundary7f3a"
#define PF_BACKUP_CHUNK    4096

/* Read the "backup_url" value from /sdcard/secrets_config.txt into out.
 * Uses the same line-parsing style as sd_apply_config_if_present().
 * Returns true if the key is present and non-empty. */
static bool pf_read_backup_url(char *out, size_t out_sz)
{
    out[0] = '\0';
    FILE *f = fopen("/sdcard/secrets_config.txt", "r");
    if (!f) return false;
    char line[384];
    while (fgets(line, sizeof(line), f)) {
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln - 1] == '\r' || line[ln - 1] == '\n'))
            line[--ln] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        char *ke = key + strlen(key) - 1;
        while (ke >= key && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';
        while (*val == ' ' || *val == '\t') val++;
        if (strcmp(key, "backup_url") == 0) {
            snprintf(out, out_sz, "%s", val);
            break;
        }
    }
    fclose(f);
    return out[0] != '\0';
}

/* Upload a single file via multipart/form-data POST.  The relative path is
 * sent as a "path" form field (so the server can recreate the directory tree)
 * and the file body is streamed from SD in PF_BACKUP_CHUNK-sized reads — the
 * file is never loaded whole into RAM.  Returns true on HTTP 2xx. */
static bool pf_upload_file(const char *fullpath, const char *relpath, const char *url)
{
    struct stat st;
    if (stat(fullpath, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    long fsize = (long)st.st_size;

    /* Allocate the multipart header from PSRAM (with internal fallback) so it
     * does not consume the internal stack, which is already deep by the time
     * we reach this function (pf_backup_task → pf_backup_sd → pf_backup_dir
     * → pf_upload_file) and esp_http_client_open adds several more KB. */
    char *part_hdr = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!part_hdr) part_hdr = heap_caps_malloc(1024, MALLOC_CAP_8BIT);
    if (!part_hdr) return false;
    int hdr_len = snprintf(part_hdr, 1024,
        "--" PF_BACKUP_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"path\"\r\n\r\n"
        "%s\r\n"
        "--" PF_BACKUP_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n",
        relpath, relpath);
    static const char FOOTER[] = "\r\n--" PF_BACKUP_BOUNDARY "--\r\n";
    int footer_len = (int)strlen(FOOTER);
    if (hdr_len < 0 || hdr_len >= 1024) { free(part_hdr); return false; }

    /* ── Phase 1: read the WHOLE file into PSRAM before any HTTP connection
     * exists.  On the Core2 the SD card shares the SPI bus and needs internal
     * DMA RAM for every read; the WiFi/HTTP stack competes for that same scarce
     * internal RAM.  The old design interleaved SD reads with writes on an open
     * socket, so both heavy consumers were active at once — on large files that
     * starved the SD driver (sdmmc_read_sectors: not enough mem) which sent a
     * truncated body (server then hung) and ultimately crashed the SPI master
     * (setup_dma_priv_buffer: Failed to allocate priv RX buffer → null deref).
     * Loading the body into PSRAM first, then POSTing it while the SD card is
     * idle, keeps the two consumers from overlapping.  A file too large for
     * PSRAM is skipped (counted as a failure) rather than crashing the run. */
    uint8_t *body = NULL;
    if (fsize > 0) {
        body = heap_caps_malloc((size_t)fsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!body) {
            ESP_LOGW(TAG, "backup: %s skipped — no PSRAM for %ld bytes", relpath, fsize);
            free(part_hdr);
            return false;
        }
        FILE *fp = fopen(fullpath, "rb");
        if (!fp) { free(body); free(part_hdr); return false; }
        size_t got = fread(body, 1, (size_t)fsize, fp);
        fclose(fp);
        if (got != (size_t)fsize) {
            ESP_LOGW(TAG, "backup: %s short read %u/%ld — skipped",
                     relpath, (unsigned)got, fsize);
            free(body);
            free(part_hdr);
            return false;
        }
    }

    /* ── Phase 2: POST the buffered body.  The SD card is now idle, so the
     * HTTP/WiFi stack has the internal RAM to itself. */
    int total_len = hdr_len + (int)fsize + footer_len;
    char content_type[] = "multipart/form-data; boundary=" PF_BACKUP_BOUNDARY;

    bool ok = false;
    for (int attempt = 1; attempt <= 2 && !ok; attempt++) {
        esp_http_client_config_t cfg = {
            .url               = url,
            .method            = HTTP_METHOD_POST,
            .timeout_ms        = 20000,
            /* Keep the HTTP buffers small: they come from internal RAM. */
            .buffer_size       = 1024,
            .buffer_size_tx    = 1024,
            .keep_alive_enable = false,
        };
        if (strncmp(url, "https://", 8) == 0)
            cfg.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) break;
        esp_http_client_set_header(client, "Content-Type", content_type);

        if (esp_http_client_open(client, total_len) != ESP_OK) {
            esp_http_client_cleanup(client);
            if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        bool write_ok = (esp_http_client_write(client, part_hdr, hdr_len) == hdr_len);
        /* Stream the PSRAM body to the socket in chunks. */
        size_t off = 0;
        while (write_ok && off < (size_t)fsize) {
            int n = (int)(((size_t)fsize - off > PF_BACKUP_CHUNK)
                          ? PF_BACKUP_CHUNK : ((size_t)fsize - off));
            if (esp_http_client_write(client, (const char *)body + off, n) != n)
                write_ok = false;
            off += n;
        }
        if (write_ok)
            write_ok = (esp_http_client_write(client, FOOTER, footer_len) == footer_len);

        if (write_ok) {
            esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            ok = (status >= 200 && status < 300);
            if (!ok) ESP_LOGW(TAG, "backup: %s HTTP %d", relpath, status);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (!ok && attempt < 2) vTaskDelay(pdMS_TO_TICKS(300));
    }
    free(body);
    free(part_hdr);
    return ok;
}

/* Recursively walk dirpath (e.g. "/sdcard"), uploading every regular file.
 * relbase is the path relative to the SD root ("" at the top level), used to
 * preserve the tree on the server.  *uploaded / *fail accumulate the global
 * counts across the whole tree so the on-screen total never resets when the
 * walk crosses into a subfolder.  The screen is updated only every few files to
 * limit SPI/DMA contention with the main loop. */
static void pf_backup_dir(const char *dirpath, const char *relbase,
                          const char *url, int *uploaded, int *fail)
{
    DIR *d = opendir(dirpath);
    if (!d) return;

    /* Allocate path buffers from PSRAM so they don't eat the task stack at
     * every recursion level (pf_backup_task → pf_backup_sd → pf_backup_dir
     * is already several hundred bytes before we reach this call). */
    size_t path_buf_sz = (size_t)(MP3_MAX_PATH_LEN + 64);
    char *child = heap_caps_malloc(path_buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!child) child = heap_caps_malloc(path_buf_sz, MALLOC_CAP_8BIT);
    char *rel   = heap_caps_malloc(path_buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rel)   rel   = heap_caps_malloc(path_buf_sz, MALLOC_CAP_8BIT);
    if (!child || !rel) {
        free(child); free(rel); closedir(d); return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;                    /* . .. and dotfiles */
        if (strcmp(e->d_name, "tmp_tts.mp3") == 0) continue;  /* transient TTS file */

        snprintf(child, path_buf_sz, "%s/%s", dirpath, e->d_name);
        if (relbase[0])
            snprintf(rel, path_buf_sz, "%s/%s", relbase, e->d_name);
        else
            snprintf(rel, path_buf_sz, "%s", e->d_name);

        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            pf_backup_dir(child, rel, url, uploaded, fail);
        } else if (S_ISREG(st.st_mode)) {
            if (pf_upload_file(child, rel, url)) (*uploaded)++;
            else                                 (*fail)++;
            if ((*uploaded + *fail) % 5 == 0) {
                char scr[48];
                snprintf(scr, sizeof(scr), "Backing up...\n%d files", *uploaded);
                screen_draw_text(scr);
            }
            vTaskDelay(pdMS_TO_TICKS(10));   /* yield SPI/CPU between files */
        }
    }
    free(child);
    free(rel);
    closedir(d);
}

/* Back up the entire SD card to the server in backup_url (secrets_config.txt).
 * Runs on its own task (see pf_backup_task) so the main loop stays free to
 * service Socket.IO.  Reports the outcome back to TRIGGERcmd via
 * pf_report_ai_result(). */
static void pf_backup_sd(const char *run_id)
{
    if (!mount_sd_card_if_needed()) {
        screen_draw_text("No SD card");
        pf_report_ai_result("No SD card", run_id);
        return;
    }
    char url[256];
    if (!pf_read_backup_url(url, sizeof(url))) {
        screen_draw_text("No backup_url\nin secrets_config.txt");
        pf_report_ai_result("No backup_url in secrets_config.txt", run_id);
        return;
    }

    /* Ack the run immediately so TRIGGERcmd marks the command as run and does
     * not re-dispatch it (which would otherwise start the backup over) while
     * the upload — potentially several minutes — is still in progress. */
    strncpy(s_pending_run_id, run_id, sizeof(s_pending_run_id) - 1);
    s_pending_run_id[sizeof(s_pending_run_id) - 1] = '\0';
    s_pending_run = true;

    ESP_LOGI(TAG, "backup: starting → %s", url);
    screen_draw_text("Backing up...");

    int fail = 0;
    int uploaded = 0;
    pf_backup_dir(MP3_ROOT_PATH, "", url, &uploaded, &fail);

    char msg[64];
    if (fail)
        snprintf(msg, sizeof(msg), "Backup done\n%d ok, %d failed", uploaded, fail);
    else
        snprintf(msg, sizeof(msg), "Backup done\n%d files", uploaded);
    screen_draw_text(msg);
    pf_report_ai_result(msg, run_id);
    ESP_LOGI(TAG, "backup: %d uploaded, %d failed", uploaded, fail);
}

/* Dedicated task that runs one backup then exits.  s_backup_running guards
 * against a second backup starting while one is in progress. */
static void pf_backup_task(void *arg)
{
    char run_id[33];
    strncpy(run_id, s_pending_backup_run_id, sizeof(run_id) - 1);
    run_id[sizeof(run_id) - 1] = '\0';

    pf_backup_sd(run_id);

    s_backup_running = false;
    vTaskDelete(NULL);
}

/*
 * Sends the currently displayed JPEG plus a question to OpenAI's vision-
 * capable chat endpoint, shows the answer on screen, optionally speaks it
 * (Core2), and reports it back to TRIGGERcmd via s_pending_result.
 */
static void pf_ask_picture(const char *question, const char *run_id)
{
    char answer[PF_ASK_ANSWER_MAX] = {0};

    if (!s_jpeg_cache || s_jpeg_cache_len <= 0) {
        strncpy(answer, "No picture is\ncurrently displayed.", sizeof(answer) - 1);
        goto ask_done;
    }

    {
        char api_key[256] = {0};
        if (!read_openai_key(api_key, sizeof(api_key))) {
            strncpy(answer, "No OpenAI API key\nconfigured.\nVisit device IP\nto configure.",
                    sizeof(answer) - 1);
            goto ask_done;
        }

        screen_draw_text("Thinking...");

        /* JSON-escape the question for embedding in the request body */
        char esc_q[513];
        {
            const char *q = (question && question[0]) ? question : "What is in this picture?";
            char *p = esc_q;
            for (; *q && (size_t)(p - esc_q) < sizeof(esc_q) - 2; q++) {
                if (*q == '"' || *q == '\\') { *p++ = '\\'; *p++ = *q; }
                else if (*q == '\n')          { *p++ = '\\'; *p++ = 'n'; }
                else if (*q == '\r')          { /* skip */ }
                else                          { *p++ = *q; }
            }
            *p = '\0';
        }

        static const char prefix[] =
            "{\"model\":\"gpt-4o-mini\",\"messages\":[{\"role\":\"user\",\"content\":["
            "{\"type\":\"text\",\"text\":\"";
        static const char mid[] =
            "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":"
            "\"data:image/jpeg;base64,";
        static const char suffix[] = "\"}}]}],\"max_tokens\":200}";

        /* mbedtls_base64_encode requires dlen >= 4*ceil(slen/3) + 1 (it also
         * writes a trailing NUL within that space), even though *olen on
         * success is just 4*ceil(slen/3). */
        size_t b64_cap    = 4 * (((size_t)s_jpeg_cache_len + 2) / 3) + 1;
        size_t prefix_len = strlen(prefix);
        size_t esc_q_len  = strlen(esc_q);
        size_t mid_len    = strlen(mid);
        size_t suffix_len = strlen(suffix);
        size_t total      = prefix_len + esc_q_len + mid_len + b64_cap + suffix_len + 1;

        char *body = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!body) body = malloc(total);
        if (!body) {
            ESP_LOGE(TAG, "askpic: no memory for %u byte request", (unsigned)total);
            strncpy(answer, "Out of memory", sizeof(answer) - 1);
            goto ask_done;
        }

        char *p = body;
        memcpy(p, prefix, prefix_len); p += prefix_len;
        memcpy(p, esc_q, esc_q_len);   p += esc_q_len;
        memcpy(p, mid, mid_len);       p += mid_len;
        size_t b64_olen = 0;
        mbedtls_base64_encode((unsigned char *)p, b64_cap, &b64_olen,
                               (const unsigned char *)s_jpeg_cache, (size_t)s_jpeg_cache_len);
        p += b64_olen;
        memcpy(p, suffix, suffix_len); p += suffix_len;
        *p = '\0';
        int body_len = (int)(p - body);

        char bearer[280];
        snprintf(bearer, sizeof(bearer), "Bearer %s", api_key);

        esp_http_client_config_t cfg = {
            .url               = PF_VISION_URL,
            .method            = HTTP_METHOD_POST,
            .timeout_ms        = PF_VISION_TIMEOUT_MS,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size       = 4096,
            .buffer_size_tx    = 4096,
            .keep_alive_enable = false,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            free(body);
            strncpy(answer, "AI request failed\n(init)", sizeof(answer) - 1);
            goto ask_done;
        }

        esp_http_client_set_header(client, "Authorization", bearer);
        esp_http_client_set_header(client, "Content-Type", "application/json");

        if (esp_http_client_open(client, body_len) != ESP_OK) {
            esp_http_client_cleanup(client);
            free(body);
            strncpy(answer, "AI request failed\n(network)", sizeof(answer) - 1);
            goto ask_done;
        }
        esp_http_client_write(client, body, body_len);
        free(body);

        int64_t cl = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        int max_resp = PF_VISION_MAX_RESP;
        if (cl > 0 && cl < max_resp) max_resp = (int)cl;

        char *resp = malloc(max_resp + 1);
        if (resp) {
            int total_read = 0, n;
            while (total_read < max_resp) {
                n = esp_http_client_read(client, resp + total_read, max_resp - total_read);
                if (n <= 0) break;
                total_read += n;
            }
            resp[total_read] = '\0';
            ESP_LOGI(TAG, "askpic: HTTP %d resp: %.200s", status, resp);
            if (status == 200) {
                if (!json_extract_nested(resp, "message", "content", answer, sizeof(answer))) {
                    strncpy(answer, "AI gave no answer", sizeof(answer) - 1);
                }
            } else {
                snprintf(answer, sizeof(answer), "AI request failed\n(HTTP %d)", status);
            }
            free(resp);
        } else {
            strncpy(answer, "AI request failed\n(no memory)", sizeof(answer) - 1);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

ask_done:
    answer[sizeof(answer) - 1] = '\0';
    screen_draw_text(answer);
    pf_report_ai_result(answer, run_id);
}

#define PF_GPT_MODEL "gpt-5.4-mini"

/*
 * Sends a general text question (no picture context) to OpenAI's chat
 * completions endpoint using PF_GPT_MODEL, shows the answer on screen,
 * optionally speaks it (Core2), and reports it back to TRIGGERcmd via
 * s_pending_result.
 */
static void pf_ask_gpt(const char *question, const char *run_id)
{
    char answer[PF_ASK_ANSWER_MAX] = {0};

    char api_key[256] = {0};
    if (!read_openai_key(api_key, sizeof(api_key))) {
        strncpy(answer, "No OpenAI API key\nconfigured.\nVisit device IP\nto configure.",
                sizeof(answer) - 1);
        goto gpt_done;
    }

    screen_draw_text("Thinking...");

    {
        /* JSON-escape the question for embedding in the request body */
        char esc_q[513];
        {
            const char *q = (question && question[0]) ? question : "What would you like to know?";
            char *p = esc_q;
            for (; *q && (size_t)(p - esc_q) < sizeof(esc_q) - 2; q++) {
                if (*q == '"' || *q == '\\') { *p++ = '\\'; *p++ = *q; }
                else if (*q == '\n')          { *p++ = '\\'; *p++ = 'n'; }
                else if (*q == '\r')          { /* skip */ }
                else                          { *p++ = *q; }
            }
            *p = '\0';
        }

        char body[1024];
        int body_len = snprintf(body, sizeof(body),
            "{\"model\":\"" PF_GPT_MODEL "\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],\"max_completion_tokens\":300}",
            esc_q);

        char bearer[280];
        snprintf(bearer, sizeof(bearer), "Bearer %s", api_key);

        esp_http_client_config_t cfg = {
            .url               = PF_VISION_URL,
            .method            = HTTP_METHOD_POST,
            .timeout_ms        = 30000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size       = 4096,
            .buffer_size_tx    = 1280,
            .keep_alive_enable = false,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            strncpy(answer, "AI request failed\n(init)", sizeof(answer) - 1);
            goto gpt_done;
        }

        esp_http_client_set_header(client, "Authorization", bearer);
        esp_http_client_set_header(client, "Content-Type", "application/json");

        if (esp_http_client_open(client, body_len) != ESP_OK) {
            esp_http_client_cleanup(client);
            strncpy(answer, "AI request failed\n(network)", sizeof(answer) - 1);
            goto gpt_done;
        }
        esp_http_client_write(client, body, body_len);

        int64_t cl = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        int max_resp = PF_VISION_MAX_RESP;
        if (cl > 0 && cl < max_resp) max_resp = (int)cl;

        char *resp = malloc(max_resp + 1);
        if (resp) {
            int total_read = 0, n;
            while (total_read < max_resp) {
                n = esp_http_client_read(client, resp + total_read, max_resp - total_read);
                if (n <= 0) break;
                total_read += n;
            }
            resp[total_read] = '\0';
            ESP_LOGI(TAG, "askgpt: HTTP %d resp: %.200s", status, resp);
            if (status == 200) {
                if (!json_extract_nested(resp, "message", "content", answer, sizeof(answer))) {
                    strncpy(answer, "AI gave no answer", sizeof(answer) - 1);
                }
            } else {
                snprintf(answer, sizeof(answer), "AI request failed\n(HTTP %d)", status);
            }
            free(resp);
        } else {
            strncpy(answer, "AI request failed\n(no memory)", sizeof(answer) - 1);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

gpt_done:
    answer[sizeof(answer) - 1] = '\0';
    screen_draw_text(answer);
    pf_report_ai_result(answer, run_id);
}
#endif /* !CONFIG_HARDWARE_CYD */

/* Decode a possibly-quoted JSON string literal into plain text. */
static bool json_unescape_string_literal(const char *in, char *out, size_t out_sz)
{
    if (!in || !out || out_sz == 0) return false;

    while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n') in++;
    bool quoted = (*in == '"');
    if (quoted) in++;

    size_t i = 0;
    while (*in && i < out_sz - 1) {
        if (quoted && *in == '"') break;
        if (*in == '\\') {
            in++;
            if (!*in) break;
            switch (*in) {
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                case '\\': out[i++] = '\\'; break;
                case '"': out[i++] = '"'; break;
                case '/': out[i++] = '/'; break;
                default: out[i++] = *in; break;
            }
            in++;
            continue;
        }
        out[i++] = *in++;
    }

    out[i] = '\0';
    return i > 0;
}

/* Extract trigger/id/params from known TriggerCMD message payload formats. */
static void extract_message_fields(const char *payload_json,
                                   char *trigger,
                                   size_t trigger_sz,
                                   char *run_id,
                                   size_t run_id_sz,
                                   char *params,
                                   size_t params_sz)
{
    if (!payload_json || !trigger || !run_id || !params) return;

    trigger[0] = '\0';
    run_id[0] = '\0';
    params[0] = '\0';

    json_extract_str(payload_json, "trigger", trigger, trigger_sz);
    json_extract_str(payload_json, "id", run_id, run_id_sz);
    json_extract_str(payload_json, "params", params, params_sz);

    if (!trigger[0]) json_extract_str(payload_json, "name", trigger, trigger_sz);
    if (!trigger[0]) json_extract_str(payload_json, "command", trigger, trigger_sz);
    if (!run_id[0]) json_extract_str(payload_json, "runId", run_id, run_id_sz);
    if (!run_id[0]) json_extract_str(payload_json, "run_id", run_id, run_id_sz);
    if (!params[0]) json_extract_str(payload_json, "param", params, params_sz);
    if (!params[0]) json_extract_str(payload_json, "value", params, params_sz);

    if (!trigger[0]) json_extract_nested(payload_json, "data", "trigger", trigger, trigger_sz);
    if (!trigger[0]) json_extract_nested(payload_json, "message", "trigger", trigger, trigger_sz);
    if (!run_id[0]) json_extract_nested(payload_json, "data", "id", run_id, run_id_sz);
    if (!params[0]) json_extract_nested(payload_json, "data", "params", params, params_sz);

    if (!trigger[0]) {
        char decoded[768] = {0};
        if (json_unescape_string_literal(payload_json, decoded, sizeof(decoded)) && decoded[0] == '{') {
            json_extract_str(decoded, "trigger", trigger, trigger_sz);
            if (!trigger[0]) json_extract_str(decoded, "name", trigger, trigger_sz);
            json_extract_str(decoded, "id", run_id, run_id_sz);
            json_extract_str(decoded, "params", params, params_sz);
        }
    }

    if (!trigger[0]) {
        const char *p = payload_json;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '"') p++;

        size_t ti = 0;
        while (*p && !isspace((unsigned char)*p) && *p != '"' && ti < trigger_sz - 1) {
            trigger[ti++] = (char)tolower((unsigned char)*p);
            p++;
        }
        trigger[ti] = '\0';

        while (*p == ' ' || *p == '\t') p++;
        if (*p && params_sz > 0) {
            strncpy(params, p, params_sz - 1);
            params[params_sz - 1] = '\0';
        }
    }

    size_t plen = strlen(params);
    if (plen >= 2 && params[0] == '"' && params[plen - 1] == '"') {
        memmove(params, params + 1, plen - 2);
        params[plen - 2] = '\0';
    }
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
    const char *voice_reply;    /* NULL → "" (server default); "{{result}}" → wait for run/save status */
} pf_cmd_t;

static const pf_cmd_t s_pf_cmds[] = {
    { "text",      "text",      "true",  "Update the display text. Example: 'Hello world!'",           "\xF0\x9F\x93\x9D" /* 📝 */, NULL },
#if !CONFIG_HARDWARE_CYD
    /* speak needs the Core2 speaker (TTS) — on the no-PSRAM CYD it would only
     * echo the text, so it is not registered there. */
    { "speak",     "speak",     "true",  "Speak text aloud via TTS. Example: 'Hello world!'",           "\xF0\x9F\x94\x8A" /* 🔊 */, NULL },
#endif
    { "color",     "color",     "true",  "Change the display color. Example: 'red' or '#FF0000'", "\xF0\x9F\x94\xA4" /* 🔤 */, NULL },
    { "textcolor", "textcolor", "true",  "Change the text color. Example: 'blue' or '#0000FF'", "\xF0\x9F\x8E\xA8" /* 🎨 */, NULL },
    { "fontsize",  "fontsize",  "true",  "Change the font size (1-4). Example: '3'",                     "\xF0\x9F\x94\xA1" /* 🔡 */, NULL },
    { "landscape", "landscape", "false", "Set the display to landscape orientation.",                        "\xE2\x86\x94\xEF\xB8\x8F" /* ↔️ */, NULL },
    { "portrait",  "portrait",  "false", "Set the display to portrait orientation.",                         "\xE2\x86\x95\xEF\xB8\x8F" /* ↕️ */, NULL },
#if !CONFIG_HARDWARE_CYD
    /* Image commands need PSRAM (≈150KB RGB565 decode buffer) and, for web
     * URLs, a second TLS context — neither is available on the CYD. */
    { "jpeg",      "jpeg",      "true",  "Display a JPEG picture for the user when they say something like, 'Picture of a cat'. Use loremflickr.com by default. If multiple words (Example: cat,dog), use a comma. The command parameter should always be a URL like this: 'https://loremflickr.com/320/240/dog,cat/all' or this if single word: https://loremflickr.com/320/240/dog", "\xF0\x9F\x96\xBC\xEF\xB8\x8F" /* 🖼️ */, NULL },
#endif
    { "save",      "save",      "false", "Save the screen settings to non-volatile memory.", "\xF0\x9F\x92\xBE" /* 💾 */, NULL },
#if !CONFIG_HARDWARE_CYD
    { "savepic",   "savepic",   "false", "Save the currently displayed JPEG to the SD card in the 'saved-jpegs' folder.", "\xF0\x9F\x93\xB7" /* 📷 */, NULL },
#endif
    { "folders",   "folders",   "false", "List the folders on the SD card.", "\xF0\x9F\x93\x82" /* 📂 */, "{{result}}" },
    { "files",     "files",     "true",  "List files in a folder on the SD card. Example: 'music'", "\xF0\x9F\x93\x84" /* 📄 */, "{{result}}" },
    { "backup",    "backup",    "false", "Back up the entire SD card to the server set by 'backup_url' in secrets_config.txt (one upload per file).", "\xF0\x9F\x92\xBE" /* 💾 */, "{{result}}" },
    { "reboot",    "reboot",    "false", "Reboot the device.", "\xF0\x9F\x94\x81" /* 🔁 */, NULL },
    { "sleeptimer","sleeptimer","true",  "Set minutes of inactivity before the device sleeps (0 = never). Example: '10'", "\xF0\x9F\x98\xB4" /* 😴 */, NULL },
    { "sleep",     "sleep",     "false", "Put the device into deep sleep immediately. Wake by touching the screen.", "\xF0\x9F\x92\xA4" /* 💤 */, NULL },
#if !CONFIG_HARDWARE_CYD
    /* battery (AXP192) and listen (microphone) are Core2-only hardware. */
    { "battery",   "battery",   "false", "Get the battery level of the user's Core2 device. Returns level (0-100), voltage in mV, and whether it is charging.", "\xF0\x9F\x94\x8B" /* 🔋 */, "{{result}}" },
    { "listen",    "listen",    "false", "Start listening for a voice command on the user's Core2 device (records for 4 seconds then processes as an AI prompt).", "\xF0\x9F\x8E\xA4" /* 🎤 */, NULL },
    { "sleeponpower","sleeponpower","true", "Toggle whether the device can auto-sleep while on USB power (default off = only sleep on battery). Pass 'on'/'off' or omit to toggle.", "\xF0\x9F\x94\x8C" /* 🔌 */, NULL },
    { "askpic",    "askpic",    "true",  "Ask a question about the picture currently displayed, using AI vision. Example: 'What is in this picture?'", "\xE2\x9D\x93" /* ❓ */, "{{result}}" },
    { "askgpt",    "askgpt",    "true",  "Ask GPT a general question (no picture context). Example: 'What is the capital of France?'", "\xF0\x9F\x92\xAC" /* 💬 */, "{{result}}" },
#if CONFIG_CORE2_HW
    { "aitts",     "aitts",     "true",  "Toggle whether AI answers from 'askpic' are spoken aloud via TTS. Default on. Pass 'on'/'off' or omit to toggle.", "\xF0\x9F\x97\xA3\xEF\xB8\x8F" /* 🗣️ */, NULL },
    { "micsrc",    "micsrc",    "true",  "Switch the microphone source between the built-in PDM mic ('pdm') and the external Grove analog mic ('grove'). Omit to toggle.", "\xF0\x9F\x8E\x99\xEF\xB8\x8F" /* 🎙️ */, NULL },
    { "clock",     "clock",     "true",  "Show a live clock on the display. Pass 'digital' or 'analog' to choose the style (default 'digital'). Stays on screen until another command runs. Example: 'analog'", "\xF0\x9F\x95\x90" /* 🕐 */, NULL },
    { "timezone",  "timezone",  "true",  "Set the clock timezone. Examples: 'eastern', 'central', 'mountain', 'pacific', 'alaska', 'hawaii', 'utc', 'london', 'europe', or a raw POSIX TZ string. Default: eastern.", "\xF0\x9F\x8C\x90" /* 🌐 */, NULL },
#endif
#endif
};
#define PF_CMD_COUNT  (sizeof(s_pf_cmds) / sizeof(s_pf_cmds[0]))

static const pf_cmd_t s_pf_media_cmds[] = {
    { "play",        "play",        "false", "Resume paused MP3 playback.", "\xE2\x96\xB6\xEF\xB8\x8F" /* ▶️ */, NULL },
    { "pause",       "pause",       "false", "Toggle MP3 playback: plays if paused, pauses if playing.", "\xE2\x8F\xAF\xEF\xB8\x8F" /* ⏯️ */, NULL },
    { "stop",        "stop",        "false", "Pause MP3 playback and keep the current position visible.", "\xE2\x8F\xB8\xEF\xB8\x8F" /* ⏸️ */, NULL },
    { "next",        "next",        "false", "Skip to the next MP3 file in the current folder.", "\xE2\x8F\xA9" /* ⏩ */, NULL },
    { "previous",    "previous",    "false", "Go to the previous MP3 file in the current folder.", "\xE2\x8F\xAA" /* ⏪ */, NULL },
    { "forward",     "forward",     "false", "Skip forward 10 seconds within the current MP3.", "\xE2\x8F\xA9" /* ⏩ */, NULL },
    { "reverse",     "reverse",     "false", "Skip backward 10 seconds within the current MP3.", "\xE2\x8F\xAA" /* ⏪ */, NULL },
    { "volumeup",    "volumeup",    "false", "Increase playback volume.", "\xF0\x9F\x94\x8A" /* 🔊 */, NULL },
    { "volumedown",  "volumedown",  "false", "Decrease playback volume.", "\xF0\x9F\x94\x89" /* 🔉 */, NULL },
    { "volumelevel", "volumelevel", "true",  "Set the playback volume to an exact percentage (0\xe2\x80\x93" "100). Example: '75'", "\xF0\x9F\x94\x8A" /* 🔊 */, NULL },
    { "mute",        "mute",        "false", "Toggle mute on or off. Mute state is not saved across reboots.", "\xF0\x9F\x94\x87" /* 🔇 */, NULL },
    { "shuffle",     "shuffle",     "true",  "Enable or disable shuffle mode. Example: 'on' or 'off'", "\xF0\x9F\x94\x80" /* 🔀 */, NULL },
    { "repeattrack", "repeattrack", "true",  "Enable or disable repeat-track mode. Example: 'on' or 'off'", "\xF0\x9F\x94\x82" /* 🔂 */, NULL },
    { "repeatplaylist", "repeatplaylist", "true",  "Enable or disable repeat-playlist mode. Example: 'on' or 'off'", "\xF0\x9F\x94\x81" /* 🔁 */, NULL },
    { "visualizer",  "visualizer",  "true",  "Enable or disable the LED audio visualizer on the sides of the device, or pick its style (1-100). Example: 'on', 'off', '6'", "\xF0\x9F\x8C\x88" /* 🌈 */, NULL },
    { "visualizernext", "visualizernext", "false", "Switch the LED audio visualizer to the next style (wraps around after the last style) and turn it on.", "\xE2\x8F\xA9" /* ⏩ */, NULL },
    { "visualizerprevious", "visualizerprevious", "false", "Switch the LED audio visualizer to the previous style (wraps around before the first style) and turn it on.", "\xE2\x8F\xAA" /* ⏪ */, NULL },
    { "ledcolor",    "ledcolor",    "true",  "Set all side LEDs to a solid color. Examples: 'red', '#FF0000', 'off'", "\xF0\x9F\x92\xA1" /* 💡 */, NULL },
    { "pair",        "pair",        "true",  "Pair with a Bluetooth headset or speaker. Example: 'pair'", "\xF0\x9F\x8E\xA7" /* 🎧 */, NULL },
    { "btstatus",    "btstatus",    "false", "Show Bluetooth audio connection status.", "\xF0\x9F\x93\xB6" /* 📶 */, NULL },
    { "btdisconnect", "btdisconnect", "false", "Disconnect the current Bluetooth audio device.", "\xF0\x9F\x94\x8C" /* 🔌 */, NULL },
    { "btforget",    "btforget",    "false", "Forget the saved Bluetooth device and stop auto-reconnect.", "\xF0\x9F\xA7\xB9" /* 🧹 */, NULL },
};
#define PF_MEDIA_CMD_COUNT (sizeof(s_pf_media_cmds) / sizeof(s_pf_media_cmds[0]))

static bool command_exists_online(const char *list_body, int list_len, const char *trigger)
{
    if (!list_body || list_len <= 0 || !trigger || !trigger[0]) return false;

    const char *p = list_body;
    while ((p = strstr(p, "\"")) != NULL) {
        const char *q = p + 1;

        bool key_is_name = strncmp(q, "name\"", 5) == 0;
        bool key_is_trigger = strncmp(q, "trigger\"", 8) == 0;
        if (!key_is_name && !key_is_trigger) {
            p = q;
            continue;
        }

        q += key_is_name ? 5 : 8;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q != ':') {
            p = q;
            continue;
        }
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q != '"') {
            p = q;
            continue;
        }
        q++;

        char name_buf[MP3_MAX_TRIGGER_LEN] = {0};
        size_t i = 0;
        bool esc = false;
        while (*q && (esc || *q != '"')) {
            if (!esc && *q == '\\') {
                esc = true;
                q++;
                continue;
            }
            if (i < sizeof(name_buf) - 1) name_buf[i++] = *q;
            esc = false;
            q++;
        }
        name_buf[i] = '\0';
        if (*q == '"') q++;

        if (strcasecmp(name_buf, trigger) == 0) return true;
        p = q;
    }

    return false;
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
    APPEND_FIELD_LOCAL("voiceReply",         cmd->voice_reply ? cmd->voice_reply : "")
    APPEND_FIELD_LOCAL("allowParams",        cmd->allow_params ? cmd->allow_params : "false")
    APPEND_FIELD_LOCAL("mcpToolDescription", cmd->mcp_desc ? cmd->mcp_desc : "")
    APPEND_FIELD_LOCAL("icon",               cmd->icon ? cmd->icon : "")

#undef APPEND_FIELD_LOCAL

    if (p > body && *(p - 1) == '&') p--;
    *p = '\0';

    int cs = https_post_form(cmd_url, s_hw_token, body, NULL);
    ESP_LOGI(TAG, "cmd/save '%s' -> HTTP %d", cmd->trigger, cs);
}

static bool mp3_folder_trigger_exists(const char *trigger)
{
    if (!trigger || !trigger[0]) return false;
    for (size_t i = 0; i < s_mp3_folder_count; i++) {
        if (strcmp(s_mp3_folders[i].trigger, trigger) == 0) return true;
    }
    return false;
}

static bool online_record_is_mp3_dynamic(const char *record_start,
                                         const char *record_end)
{
    if (!record_start || !record_end || record_end <= record_start) return false;
    static const char marker[] = "\"mcpToolDescription\":\"Play mp3 files in the ";
    const char *m = strstr(record_start, marker);
    return (m && m < record_end);
}

static void sync_remove_stale_mp3_commands(const char *del_url,
                                           const char *list_body,
                                           int list_len)
{
    if (!del_url || !list_body || list_len <= 0) return;

    const char *p = list_body;
    static const char name_key[] = "\"name\":\"";

    while ((p = strstr(p, name_key)) != NULL) {
        p += strlen(name_key);

        char trigger[MP3_MAX_TRIGGER_LEN] = {0};
        size_t ti = 0;
        bool esc = false;
        while (*p && (esc || *p != '"')) {
            if (!esc && *p == '\\') {
                esc = true;
                p++;
                continue;
            }
            if (ti < sizeof(trigger) - 1) trigger[ti++] = *p;
            esc = false;
            p++;
        }
        trigger[ti] = '\0';
        if (*p == '"') p++;

        if (!trigger[0]) continue;
        if (trigger_reserved(trigger)) continue;
        if (mp3_folder_trigger_exists(trigger)) continue;

        const char *rec_start = p;
        while (rec_start > list_body && *rec_start != '{') rec_start--;
        if (rec_start < list_body) rec_start = list_body;
        const char *rec_end = strchr(p, '}');
        if (!rec_end) rec_end = p + strlen(p);

        if (!online_record_is_mp3_dynamic(rec_start, rec_end)) continue;

        char form[320];
        char *fp = form;
        char *fend = form + sizeof(form);

#define APPEND_DEL_FIELD(k, v) \
        fp = url_encode_append(fp, (size_t)(fend - fp), (k)); \
        if (fp < fend) *fp++ = '='; \
        fp = url_encode_append(fp, (size_t)(fend - fp), (v)); \
        if (fp < fend) *fp++ = '&';

        APPEND_DEL_FIELD("name", trigger)
        APPEND_DEL_FIELD("computer", s_computer_id)

#undef APPEND_DEL_FIELD

        if (fp > form && *(fp - 1) == '&') fp--;
        *fp = '\0';

        int ds = https_post_form(del_url, s_hw_token, form, NULL);
        ESP_LOGI(TAG, "cmd/delete2 '%s' -> HTTP %d", trigger, ds);
    }
}

static pf_cmd_t mp3_folder_pf_cmd(const mp3_folder_t *folder, char *desc, size_t desc_sz)
{
    snprintf(desc, desc_sz,
             "Play mp3 files in the %s folder. If the parameter is a number from 1 to 100 to specify one of the mp3 files, otherwise, this command will play the first mp3 file, or a random file in the folder if shuffle mode is on.",
             folder->trigger);
    return (pf_cmd_t){
        .trigger = folder->trigger,
        .voice = folder->trigger,
        .allow_params = "true",
        .mcp_desc = desc,
        .icon = "\xF0\x9F\x8E\xB6", /* 🎶 */
    };
}

static pf_cmd_t jpeg_folder_pf_cmd(const jpeg_folder_t *folder, char *desc, size_t desc_sz)
{
    snprintf(desc, desc_sz,
             "Display jpeg files in the %s folder. If the parameter is a number from 1 to 100, it specifies one of the jpeg files, otherwise, this command will display the first jpeg file.",
             folder->trigger);
    return (pf_cmd_t){
        .trigger = folder->trigger,
        .voice = folder->trigger,
        .allow_params = "true",
        .mcp_desc = desc,
        .icon = "\xF0\x9F\x96\xBC\xEF\xB8\x8F", /* 🖼️ */
    };
}

static void sync_all_commands(bool remove_stale_mp3)
{
    if (!s_computer_id[0] || !s_hw_token[0]) return;

    char list_url[192];
    snprintf(list_url, sizeof(list_url),
             "%s/api/command/list?computer_id=%s&limit=200",
             TCMD_BASE_URL, s_computer_id);

    char *list_body = NULL;
    int list_len = https_get_auth(list_url, s_hw_token, &list_body);

    if (!list_body || list_len <= 0) {
        ESP_LOGW(TAG, "cmd sync skipped: command list fetch failed (len=%d)", list_len);
        if (list_body) free(list_body);
        return;
    }

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
        pf_cmd_t dyn_cmd = mp3_folder_pf_cmd(&s_mp3_folders[i], desc, sizeof(desc));
        sync_command_if_missing(&dyn_cmd, cmd_url, list_body, list_len);
    }

    for (size_t i = 0; i < s_jpeg_folder_count; i++) {
        char desc[320];
        pf_cmd_t dyn_cmd = jpeg_folder_pf_cmd(&s_jpeg_folders[i], desc, sizeof(desc));
        sync_command_if_missing(&dyn_cmd, cmd_url, list_body, list_len);
    }

    if (remove_stale_mp3 && list_body && list_len > 0) {
        char del_url[192];
        snprintf(del_url, sizeof(del_url), "%s/api/command/delete2", TCMD_BASE_URL);
        sync_remove_stale_mp3_commands(del_url, list_body, list_len);
    }

    if (list_body) free(list_body);
}

#if CONFIG_HARDWARE_CYD || CONFIG_CORE2_HW
/* ── Command sync over the websocket ─────────────────────────────────────────
 * The no-PSRAM CYD cannot hold two TLS contexts at once: once the persistent
 * Socket.IO websocket is connected the internal heap is too fragmented for a
 * second (HTTPS) TLS handshake — and the websocket must connect FIRST to get
 * the only un-fragmented heap window. So command registration is routed over
 * the already-open websocket via Sails virtual POST (socketio_send_vpost),
 * needing no extra TLS handshake. cmd/save is an idempotent upsert, so every
 * command is saved unconditionally (no list diff). Stale-command removal is
 * skipped here (it would require fetching+reassembling the command list over
 * the small WS buffer).
 *
 * Core2 (PSRAM) has no heap-fragmentation constraint but uses the same path
 * for consistency, registering commands once over the already-open socket
 * instead of opening a second HTTPS connection at boot. The HTTPS list-based
 * sync_all_commands() above is still used for the SD-card-remount reconcile,
 * which needs the list diff to add/remove dynamic MP3/JPEG-folder commands. */

static char *json_escape_append(char *dst, char *end, const char *src)
{
    while (src && *src && dst < end - 1) {
        unsigned char c = (unsigned char)*src++;
        if (c == '"' || c == '\\') {
            if (dst < end - 2) { *dst++ = '\\'; *dst++ = (char)c; }
            else break;
        } else if (c < 0x20) {
            /* control chars not expected in command metadata — drop */
            continue;
        } else {
            *dst++ = (char)c;
        }
    }
    return dst;
}

static char *lit_append(char *dst, char *end, const char *s)
{
    while (*s && dst < end - 1) *dst++ = *s++;
    return dst;
}

static void sync_command_ws(const pf_cmd_t *cmd)
{
    if (!cmd || !cmd->trigger || !cmd->trigger[0]) return;

    char data[768];
    char *d = data;
    char *e = data + sizeof(data);

    d = lit_append(d, e, "{\"name\":\"");
    d = json_escape_append(d, e, cmd->trigger);
    d = lit_append(d, e, "\",\"computer\":\"");
    d = json_escape_append(d, e, s_computer_id);
    d = lit_append(d, e, "\",\"voice\":\"");
    d = json_escape_append(d, e, cmd->voice ? cmd->voice : "");
    d = lit_append(d, e, "\",\"voiceReply\":\"");
    d = json_escape_append(d, e, cmd->voice_reply ? cmd->voice_reply : "");
    d = lit_append(d, e, "\",\"allowParams\":\"");
    d = json_escape_append(d, e, cmd->allow_params ? cmd->allow_params : "false");
    d = lit_append(d, e, "\",\"mcpToolDescription\":\"");
    d = json_escape_append(d, e, cmd->mcp_desc ? cmd->mcp_desc : "");
    d = lit_append(d, e, "\",\"icon\":\"");
    d = json_escape_append(d, e, cmd->icon ? cmd->icon : "");
    d = lit_append(d, e, "\"}");
    *d = '\0';

    esp_err_t r = socketio_send_vpost(
        "/api/command/save?__sails_io_sdk_version=0.11.0", s_hw_token, data);
    ESP_LOGI(TAG, "cmd/save(ws) '%s' -> %s", cmd->trigger, esp_err_to_name(r));
}

/* Sync one command over the websocket, pacing sends so a rapid burst of
 * vposts plus the server's ~460-byte acks doesn't fill the send buffer faster
 * than it drains (the CYD's TCP windows are trimmed to 2880 bytes to save
 * internal RAM; 350ms between sends lets each ack arrive and be consumed
 * before the next request). Returns false if the socket dropped mid-sync. */
static bool sync_one_cmd_ws(const pf_cmd_t *cmd)
{
    if (!socketio_connected()) {
        ESP_LOGW(TAG, "ws command sync aborted: socket dropped");
        return false;
    }
    sync_command_ws(cmd);
    vTaskDelay(pdMS_TO_TICKS(350));
    return true;
}

static bool sync_cmd_array_ws(const pf_cmd_t *cmds, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!sync_one_cmd_ws(&cmds[i])) return false;
    }
    return true;
}

static void sync_all_commands_ws(void)
{
    if (!s_computer_id[0] || !s_hw_token[0]) return;
    if (!socketio_connected()) {
        ESP_LOGW(TAG, "ws command sync skipped: socket not connected");
        return;
    }

    /* CYD registers only the display/system commands (s_pf_cmds, already
     * trimmed for CONFIG_HARDWARE_CYD). The MP3/Bluetooth media commands
     * (s_pf_media_cmds), the dynamic MP3-folder commands, and the dynamic
     * JPEG-folder commands are intentionally NOT registered: this no-PSRAM
     * board has no working audio path (BT A2DP needs a 256KB PCM ring it
     * can't allocate) and cannot decode/display images (~150KB RGB565 buffer,
     * plus web fetch needs a 2nd TLS context). See cyd_commands.json. */
    if (!sync_cmd_array_ws(s_pf_cmds, PF_CMD_COUNT)) return;

#if CONFIG_CORE2_HW
    /* Core2 supports the MP3/Bluetooth media commands and SD-card folder
     * playback/slideshow, so register those too on first sync. Folder
     * indexes were rebuilt just before this call, so s_mp3_folders /
     * s_jpeg_folders already reflect the SD card's current contents. */
    if (!sync_cmd_array_ws(s_pf_media_cmds, PF_MEDIA_CMD_COUNT)) return;

    for (size_t i = 0; i < s_mp3_folder_count; i++) {
        char desc[320];
        pf_cmd_t dyn_cmd = mp3_folder_pf_cmd(&s_mp3_folders[i], desc, sizeof(desc));
        if (!sync_one_cmd_ws(&dyn_cmd)) return;
    }

    for (size_t i = 0; i < s_jpeg_folder_count; i++) {
        char desc[320];
        pf_cmd_t dyn_cmd = jpeg_folder_pf_cmd(&s_jpeg_folders[i], desc, sizeof(desc));
        if (!sync_one_cmd_ws(&dyn_cmd)) return;
    }
#endif

    ESP_LOGI(TAG, "ws command sync complete");
}
#endif /* CONFIG_HARDWARE_CYD || CONFIG_CORE2_HW */

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

#if CONFIG_HARDWARE_CORE2
/* Configure the touch-controller interrupt line as an EXT1 deep-sleep wake
 * source, then enter deep sleep. On Core2 this is the FT6336U INT (GPIO 39,
 * active-low, with a PCB pull-up so the internal pulls stay disabled). On
 * CYD it is the XPT2046 T_IRQ line at CONFIG_CYD_XPT2046_IRQ_GPIO, which is
 * board-dependent and may have no external pull-up, so the internal pull-up
 * is enabled there. If no wake GPIO is configured for this board, deep sleep
 * is entered with no wake source (until reset/power-cycle) rather than risk
 * an immediate wake from a floating pin. */
static void pf_enter_deep_sleep_with_touch_wake(void)
{
    screen_backlight_off();

#if CONFIG_CORE2_HW
    gpio_num_t wake_gpio = GPIO_NUM_39;
    bool enable_internal_pullup = false;
#elif CONFIG_HARDWARE_CYD && CONFIG_CYD_TOUCH_XPT2046 && CONFIG_CYD_XPT2046_IRQ_GPIO >= 0
    gpio_num_t wake_gpio = (gpio_num_t)CONFIG_CYD_XPT2046_IRQ_GPIO;
    bool enable_internal_pullup = true;
#else
    gpio_num_t wake_gpio = GPIO_NUM_NC;
    bool enable_internal_pullup = false;
#endif

    if (wake_gpio >= 0) {
        rtc_gpio_init(wake_gpio);
        rtc_gpio_set_direction(wake_gpio, RTC_GPIO_MODE_INPUT_ONLY);
        if (enable_internal_pullup) {
            rtc_gpio_pullup_en(wake_gpio);
        } else {
            rtc_gpio_pullup_dis(wake_gpio);
        }
        rtc_gpio_pulldown_dis(wake_gpio);
        esp_sleep_enable_ext1_wakeup(1ULL << wake_gpio, ESP_EXT1_WAKEUP_ALL_LOW);
    } else {
        ESP_LOGW(TAG, "No touch-wake GPIO configured -- deep sleep until reset");
    }

    esp_deep_sleep_start();
}
#endif /* CONFIG_HARDWARE_CORE2 */

/* ── On-screen clock ─────────────────────────────────────────────────────
 * "clock" / "clock digital" / "clock analog" replace the screen with a
 * live-updating clock, refreshed once per second from the main loop.
 * Any other command (handled at the top of pf_event_handler) or on-screen
 * menu open stops the clock view. */
#if CONFIG_CORE2_HW

static void pf_clock_stop(void)
{
    if (s_clock_mode == PF_CLOCK_OFF) return;
    s_clock_mode = PF_CLOCK_OFF;
    if (s_mp3_saved_font_scale >= 0) {
        screen_set_font_scale_silent(s_mp3_saved_font_scale);
        s_mp3_saved_font_scale = -1;
    }
}

static void pf_clock_start(const char *params)
{
    s_clock_mode = (params && strcasecmp(params, "analog") == 0)
                       ? PF_CLOCK_ANALOG
                       : PF_CLOCK_DIGITAL;
    s_clock_last_sec = -1; /* force an immediate redraw */
}

/* Set TZ from a friendly alias or raw POSIX string, then call tzset(). */
static void apply_timezone(const char *tz_str)
{
    const char *posix;
    if      (strcasecmp(tz_str, "eastern")       == 0 ||
             strcasecmp(tz_str, "est")            == 0 ||
             strcasecmp(tz_str, "edt")            == 0)
        posix = "EST5EDT,M3.2.0,M11.1.0";
    else if (strcasecmp(tz_str, "central")       == 0 ||
             strcasecmp(tz_str, "cst")            == 0 ||
             strcasecmp(tz_str, "cdt")            == 0)
        posix = "CST6CDT,M3.2.0,M11.1.0";
    else if (strcasecmp(tz_str, "mountain")      == 0 ||
             strcasecmp(tz_str, "mst")            == 0 ||
             strcasecmp(tz_str, "mdt")            == 0)
        posix = "MST7MDT,M3.2.0,M11.1.0";
    else if (strcasecmp(tz_str, "pacific")       == 0 ||
             strcasecmp(tz_str, "pst")            == 0 ||
             strcasecmp(tz_str, "pdt")            == 0)
        posix = "PST8PDT,M3.2.0,M11.1.0";
    else if (strcasecmp(tz_str, "alaska")        == 0 ||
             strcasecmp(tz_str, "akst")           == 0 ||
             strcasecmp(tz_str, "akdt")           == 0)
        posix = "AKST9AKDT,M3.2.0,M11.1.0";
    else if (strcasecmp(tz_str, "hawaii")        == 0 ||
             strcasecmp(tz_str, "hst")            == 0)
        posix = "HST10";
    else if (strcasecmp(tz_str, "utc")           == 0 ||
             strcasecmp(tz_str, "gmt")            == 0)
        posix = "UTC0";
    else if (strcasecmp(tz_str, "london")        == 0 ||
             strcasecmp(tz_str, "uk")             == 0 ||
             strcasecmp(tz_str, "bst")            == 0)
        posix = "GMT0BST,M3.5.0/1,M10.5.0";
    else if (strcasecmp(tz_str, "central_europe") == 0 ||
             strcasecmp(tz_str, "cet")            == 0 ||
             strcasecmp(tz_str, "europe")         == 0)
        posix = "CET-1CEST,M3.5.0,M10.5.0/3";
    else
        posix = tz_str; /* treat as raw POSIX TZ string */
    strncpy(s_timezone, posix, sizeof(s_timezone) - 1);
    s_timezone[sizeof(s_timezone) - 1] = '\0';
    setenv("TZ", s_timezone, 1);
    tzset();
    s_clock_last_sec = -1; /* force clock redraw with new zone */
}

/* True if some other on-screen view currently owns the display and the
 * clock should not draw over it this tick. */
static bool pf_clock_blocked(void)
{
    return s_pending_jpeg || s_pending_jpeg_redraw || s_pending_jpeg_file ||
           s_pending_text_draw || s_menu_active || s_battery_display_active ||
           s_folder_list_display_active || s_file_list_display_active ||
           s_menu_result_active || (s_mp3.active && s_mp3_ui_override_allowed);
}

static inline void clk_set_px(uint16_t *buf, int dim, int x, int y, uint16_t color)
{
    if (x < 0 || x >= dim || y < 0 || y >= dim) return;
    buf[y * dim + x] = color;
}

/* Bresenham line with a square "pen" of the given thickness (odd values
 * center the pen on the line). */
static void clk_draw_line(uint16_t *buf, int dim, int x0, int y0, int x1, int y1,
                           uint16_t color, int thickness)
{
    int half = thickness / 2;
    int dx = abs(x1 - x0), sx = (x1 > x0) ? 1 : -1;
    int dy = -abs(y1 - y0), sy = (y1 > y0) ? 1 : -1;
    int err = dx + dy;
    while (1) {
        for (int ty = -half; ty <= half; ty++) {
            for (int tx = -half; tx <= half; tx++) {
                clk_set_px(buf, dim, x0 + tx, y0 + ty, color);
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Midpoint circle outline, drawn `thickness` pixels deep. */
static void clk_draw_circle(uint16_t *buf, int dim, int cx, int cy, int r,
                             uint16_t color, int thickness)
{
    for (int t = 0; t < thickness; t++) {
        int rr = r - t;
        int x = rr, y = 0;
        int err = 1 - rr;
        while (x >= y) {
            clk_set_px(buf, dim, cx + x, cy + y, color);
            clk_set_px(buf, dim, cx + y, cy + x, color);
            clk_set_px(buf, dim, cx - y, cy + x, color);
            clk_set_px(buf, dim, cx - x, cy + y, color);
            clk_set_px(buf, dim, cx - x, cy - y, color);
            clk_set_px(buf, dim, cx - y, cy - x, color);
            clk_set_px(buf, dim, cx + y, cy - x, color);
            clk_set_px(buf, dim, cx + x, cy - y, color);
            y++;
            if (err < 0) {
                err += 2 * y + 1;
            } else {
                x--;
                err += 2 * (y - x) + 1;
            }
        }
    }
}

static void clk_fill_circle(uint16_t *buf, int dim, int cx, int cy, int r, uint16_t color)
{
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) clk_set_px(buf, dim, cx + x, cy + y, color);
        }
    }
}

/* Renders a 240x240 analog clock face into an RGB565 buffer and blits it.
 * screen_draw_rgb565() letterboxes the square face onto the panel using
 * the current background colour in both orientations. */
static void pf_clock_render_analog(const struct tm *tm_now)
{
    const int dim = 240;
    uint16_t *buf = heap_caps_malloc((size_t)dim * dim * sizeof(uint16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = heap_caps_malloc((size_t)dim * dim * sizeof(uint16_t), MALLOC_CAP_8BIT);
    if (!buf) return;

    uint8_t bg_r, bg_g, bg_b, fg_r, fg_g, fg_b;
    screen_get_color(&bg_r, &bg_g, &bg_b);
    screen_get_text_color(&fg_r, &fg_g, &fg_b);
    uint16_t bg = (uint16_t)(((bg_r & 0xF8) << 8) | ((bg_g & 0xFC) << 3) | (bg_b >> 3));
    uint16_t fg = (uint16_t)(((fg_r & 0xF8) << 8) | ((fg_g & 0xFC) << 3) | (fg_b >> 3));

    for (int i = 0; i < dim * dim; i++) buf[i] = bg;

    const int cx = dim / 2, cy = dim / 2;
    const int r = dim / 2 - 6;

    clk_draw_circle(buf, dim, cx, cy, r, fg, 2);

    /* Hour ticks */
    for (int i = 0; i < 12; i++) {
        float a = (float)i * ((float)M_PI / 6.0f);
        int x0 = cx + (int)(sinf(a) * (r - 10));
        int y0 = cy - (int)(cosf(a) * (r - 10));
        int x1 = cx + (int)(sinf(a) * r);
        int y1 = cy - (int)(cosf(a) * r);
        clk_draw_line(buf, dim, x0, y0, x1, y1, fg, 2);
    }

    int hour = tm_now->tm_hour % 12;
    int min  = tm_now->tm_min;
    int sec  = tm_now->tm_sec;

    float hour_angle = ((float)hour + (float)min / 60.0f) * ((float)M_PI / 6.0f);
    float min_angle  = ((float)min + (float)sec / 60.0f) * ((float)M_PI / 30.0f);
    float sec_angle  = (float)sec * ((float)M_PI / 30.0f);

    int hx = cx + (int)(sinf(hour_angle) * (r * 0.5f));
    int hy = cy - (int)(cosf(hour_angle) * (r * 0.5f));
    clk_draw_line(buf, dim, cx, cy, hx, hy, fg, 5);

    int mx = cx + (int)(sinf(min_angle) * (r * 0.75f));
    int my = cy - (int)(cosf(min_angle) * (r * 0.75f));
    clk_draw_line(buf, dim, cx, cy, mx, my, fg, 3);

    int sx = cx + (int)(sinf(sec_angle) * (r * 0.85f));
    int sy = cy - (int)(cosf(sec_angle) * (r * 0.85f));
    clk_draw_line(buf, dim, cx, cy, sx, sy, fg, 1);

    clk_fill_circle(buf, dim, cx, cy, 3, fg);

    screen_draw_rgb565((const uint8_t *)buf, dim, dim);
    free(buf);
}

static void pf_clock_render_digital(const struct tm *tm_now)
{
    char time_buf[16];
    char date_buf[24];
    strftime(time_buf, sizeof(time_buf), "%I:%M:%S %p", tm_now);
    strftime(date_buf, sizeof(date_buf), "%a %Y-%m-%d", tm_now);

    char msg[64];
    snprintf(msg, sizeof(msg), "\n\n%s\n\n%s", time_buf, date_buf);

    if (s_mp3_saved_font_scale < 0) {
        int cur = 2;
        screen_get_font_scale(&cur);
        s_mp3_saved_font_scale = cur;
    }
    screen_set_font_scale_silent(2);
    screen_draw_text(msg);
}

/* Called once per main-loop tick while the clock is active; redraws only
 * when the wall-clock second has changed (or on the first render). */
static void pf_clock_render(void)
{
    if (s_clock_mode == PF_CLOCK_OFF || pf_clock_blocked()) return;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_sec == s_clock_last_sec) return;
    s_clock_last_sec = tm_now.tm_sec;

    if (s_clock_mode == PF_CLOCK_ANALOG) {
        pf_clock_render_analog(&tm_now);
    } else {
        pf_clock_render_digital(&tm_now);
    }
}

#endif /* CONFIG_CORE2_HW */

/* ── Socket.IO event handler ────────────────────────────────────────────── */

static void pf_event_handler(const char *event_name,
                              const char *payload_json,
                              void       *ctx)
{
    (void)ctx;

    ESP_LOGI(TAG, "pf_event: name='%s' payload=%.200s", event_name, payload_json);

    if (strcmp(event_name, "message") != 0) return;

#if CONFIG_HARDWARE_CORE2
    s_last_activity_tick = xTaskGetTickCount();
#endif

    static char s_trigger[64];
    static char s_id[32];
    static char s_params[256];

    extract_message_fields(payload_json,
                           s_trigger,
                           sizeof(s_trigger),
                           s_id,
                           sizeof(s_id),
                           s_params,
                           sizeof(s_params));

    ESP_LOGI(TAG, "message dispatch: trigger='%s' id='%s' params='%s'",
             s_trigger, s_id, s_params);

#if CONFIG_CORE2_HW
    /* Leaving any scale-2 UI screen (battery/folder/file list): restore the
     * pre-display font scale. The folders -> files drill-down is the one
     * exception where we stay in list mode and keep the saved scale. */
    if ((s_battery_display_active ||
         s_folder_list_display_active || s_file_list_display_active) &&
        strcmp(s_trigger, "files") != 0) {
        pf_list_restore_scale();
    }
    s_battery_display_active = false;
    s_folder_list_display_active = false;
    s_file_list_display_active = false;
    s_menu_result_active = false;
    s_save_result_saved_font_scale = -1;
    /* Any command other than "clock" or "save" takes the screen back.
     * "save" is excluded so it captures the live clock state rather than
     * stopping it before save_display_state_to_nvs() runs. */
    if (strcmp(s_trigger, "clock") != 0 && strcmp(s_trigger, "save") != 0 &&
        strcmp(s_trigger, "timezone") != 0) {
        if (s_clock_mode != PF_CLOCK_OFF)
            nvs_write_u8(NVS_KEY_CLOCK_MODE, 0);
        pf_clock_stop();
    }
#endif
    s_jpeg_folder_display_active = false;

    if (strcmp(s_trigger, "text") == 0) {
        /* Discard any cached JPEG so orientation changes redraw text, not image */
        if (s_jpeg_cache) { free(s_jpeg_cache); s_jpeg_cache = NULL; s_jpeg_cache_len = 0; }
#if CONFIG_CORE2_HW
        pf_free_jpeg_rgb_cache();
#endif
        s_mp3_ui_override_allowed = false;
        s_pending_jpeg = false;
        s_pending_jpeg_redraw = false;
        strncpy(s_last_text, s_params[0] ? s_params : " ", sizeof(s_last_text) - 1);
        s_last_text[sizeof(s_last_text) - 1] = '\0';
        strncpy(s_pending_text, s_last_text, sizeof(s_pending_text) - 1);
        s_pending_text[sizeof(s_pending_text) - 1] = '\0';
        s_pending_text_draw = true;
#if CONFIG_CORE2_HW
        s_pending_text_redraw_retries = 5;
#endif
        s_current_jpeg_url[0] = '\0';

    } else if (strcmp(s_trigger, "speak") == 0) {
        if (s_jpeg_cache) { free(s_jpeg_cache); s_jpeg_cache = NULL; s_jpeg_cache_len = 0; }
#if CONFIG_CORE2_HW
        pf_free_jpeg_rgb_cache();
#endif
        s_mp3_ui_override_allowed = false;
        s_pending_jpeg = false;
        s_pending_jpeg_redraw = false;
        strncpy(s_last_text, s_params[0] ? s_params : " ", sizeof(s_last_text) - 1);
        s_last_text[sizeof(s_last_text) - 1] = '\0';
        strncpy(s_pending_text, s_last_text, sizeof(s_pending_text) - 1);
        s_pending_text[sizeof(s_pending_text) - 1] = '\0';
        s_pending_text_draw = true;
#if CONFIG_CORE2_HW
        s_pending_text_redraw_retries = 5;
#endif
        s_current_jpeg_url[0] = '\0';
#if CONFIG_CORE2_HW
        strncpy(s_pending_speak_text, s_params, sizeof(s_pending_speak_text) - 1);
        s_pending_speak_text[sizeof(s_pending_speak_text) - 1] = '\0';
        s_pending_speak = true;
#endif

    } else if (strcmp(s_trigger, "color") == 0) {
        uint8_t r = 0, g = 0, b = 0;
        if (parse_color(s_params, &r, &g, &b)) {
            s_pending_bg_r = r;
            s_pending_bg_g = g;
            s_pending_bg_b = b;
            s_pending_bg_color = true;
        } else {
            ESP_LOGW(TAG, "color: unrecognised '%s'", s_params);
        }

    } else if (strcmp(s_trigger, "textcolor") == 0) {
        uint8_t r = 255, g = 255, b = 255;
        if (parse_color(s_params, &r, &g, &b)) {
            s_pending_fg_r = r;
            s_pending_fg_g = g;
            s_pending_fg_b = b;
            s_pending_fg_color = true;
        } else {
            ESP_LOGW(TAG, "textcolor: unrecognised '%s'", s_params);
        }

    } else if (strcmp(s_trigger, "fontsize") == 0) {
        int scale = atoi(s_params);
        if (scale < 1) scale = 1;
        if (scale > 4) scale = 4;
        s_pending_font_scale = scale;

    } else if (strcmp(s_trigger, "landscape") == 0) {
        s_pending_orientation = 1;

    } else if (strcmp(s_trigger, "portrait") == 0) {
        s_pending_orientation = 0;

    } else if (strcmp(s_trigger, "jpeg") == 0) {
        if (s_params[0]) {
            s_mp3_ui_override_allowed = false;
            /* Trim leading whitespace from URL */
            const char *url = s_params;
            while (*url == ' ') url++;
            s_pending_jpeg_redraw = false;
            strncpy(s_pending_jpeg_url, url, sizeof(s_pending_jpeg_url) - 1);
            s_pending_jpeg_url[sizeof(s_pending_jpeg_url) - 1] = '\0';
            s_pending_jpeg = true;
        } else {
            ESP_LOGW(TAG, "jpeg: no URL in params");
            return;  /* don't report run/save for empty command */
        }

    } else if (strcmp(s_trigger, "save") == 0) {
        /* Defer to main loop: save_display_state_to_nvs() calls NVS flash ops
         * and save_display_state_to_sd() allocates ~1 KB on the stack for
         * text encoding and then calls FatFS (f_open/fprintf/fclose, ~2-3 KB).
         * Combined with the WS task's baseline stack usage this overflows
         * the 6 KB websocket_task stack. */
        s_pending_save = true;

    } else if (strcmp(s_trigger, "savepic") == 0) {
        /* Preserve JPEG folder navigation state across savepic so swipe still works. */
        bool prev_folder_active = s_jpeg_folder_display_active;
        int  prev_folder_idx    = s_jpeg_folder_display_idx;
        int  prev_image_idx     = s_jpeg_folder_image_idx;
        if (!s_jpeg_cache || s_jpeg_cache_len <= 0) {
            screen_draw_text("No image\nto save");
        } else if (!mount_sd_card_if_needed()) {
            screen_draw_text("No SD card");
        } else {
            const char *pics_dir = MP3_ROOT_PATH "/saved-jpegs";
            mkdir(pics_dir, 0755);
            char fpath[128];
            time_t now = time(NULL);
            snprintf(fpath, sizeof(fpath), "%s/pic_%ld.jpg", pics_dir, (long)now);
            FILE *f = fopen(fpath, "wb");
            if (!f) {
                ESP_LOGE(TAG, "savepic: cannot create %s", fpath);
                screen_draw_text("Save failed");
            } else {
                size_t written = fwrite(s_jpeg_cache, 1, (size_t)s_jpeg_cache_len, f);
                fclose(f);
                if (written == (size_t)s_jpeg_cache_len) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Saved!\npic_%ld.jpg", (long)now);
                    ESP_LOGI(TAG, "savepic: %s (%d bytes)", fpath, s_jpeg_cache_len);
                    screen_draw_text(msg);
                } else {
                    ESP_LOGE(TAG, "savepic: wrote %zu/%d bytes to %s",
                             written, s_jpeg_cache_len, fpath);
                    screen_draw_text("Save failed");
                }
            }
        }
        s_jpeg_folder_display_active = prev_folder_active;
        s_jpeg_folder_display_idx    = prev_folder_idx;
        s_jpeg_folder_image_idx      = prev_image_idx;

    } else if (strcmp(s_trigger, "folders") == 0) {
        if (!mount_sd_card_if_needed()) {
            screen_draw_text("No SD card");
        } else {
            DIR *d = opendir(MP3_ROOT_PATH);
            if (!d) {
                screen_draw_text("No SD card");
            } else {
#if CONFIG_CORE2_HW
                s_folder_list_count = 0;
#endif
                char msg[256] = "Folders:";
                int msg_len = (int)strlen(msg);
                /* JSON array with pre-escaped quotes for embedding in the run/save JSON string.
                 * Each name is stored as \"name\" so snprintf %s produces valid JSON. */
                char json_arr[512];
                int  ja = 0;
                bool jfirst = true;
                json_arr[ja++] = '[';
                int count = 0;
                struct dirent *e;
                while ((e = readdir(d)) != NULL) {
                    if (e->d_name[0] == '.') continue;
                    char fpath[MP3_MAX_PATH_LEN];
                    int max_name = (int)sizeof(fpath) - (int)strlen(MP3_ROOT_PATH) - 2;
                    if (max_name <= 0) continue;
                    snprintf(fpath, sizeof(fpath), "%s/%.*s", MP3_ROOT_PATH, max_name, e->d_name);
                    struct stat st;
                    if (stat(fpath, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                    int nlen = (int)strlen(e->d_name);
                    int disp_len = nlen;
#if CONFIG_CORE2_HW
                    /* Truncate long names so this line never wraps, keeping
                     * the list display's one-line-per-entry tap math valid. */
                    int max_cols = pf_list_max_cols();
                    if (disp_len > max_cols) disp_len = max_cols;
#endif
                    if (msg_len + 1 + disp_len < (int)sizeof(msg) - 1) {
                        msg[msg_len++] = '\n';
                        memcpy(msg + msg_len, e->d_name, (size_t)disp_len);
                        msg_len += disp_len;
                        msg[msg_len] = '\0';
#if CONFIG_CORE2_HW
                        if (s_folder_list_count < PF_FOLDER_LIST_MAX) {
                            strncpy(s_folder_list_names[s_folder_list_count], e->d_name,
                                    sizeof(s_folder_list_names[0]) - 1);
                            s_folder_list_names[s_folder_list_count]
                                [sizeof(s_folder_list_names[0]) - 1] = '\0';
                            s_folder_list_count++;
                        }
#endif
                    }
                    int ja_need = (jfirst ? 0 : 1) + 4 + nlen; /* [,]\"name\" */
                    if (ja + ja_need < (int)sizeof(json_arr) - 2) {
                        if (!jfirst) json_arr[ja++] = ',';
                        json_arr[ja++] = '\\'; json_arr[ja++] = '"';
                        memcpy(json_arr + ja, e->d_name, (size_t)nlen);
                        ja += nlen;
                        json_arr[ja++] = '\\'; json_arr[ja++] = '"';
                        jfirst = false;
                    }
                    count++;
                }
                closedir(d);
                json_arr[ja++] = ']';
                json_arr[ja]   = '\0';
                strncpy(s_pending_result, json_arr, sizeof(s_pending_result) - 1);
                s_pending_result[sizeof(s_pending_result) - 1] = '\0';
                s_pending_has_result = true;
                ESP_LOGI(TAG, "folders: %d folder(s) on SD, result: %s", count, s_pending_result);
#if CONFIG_CORE2_HW
                if (count > 0) pf_list_enter_scale2();
#endif
                screen_draw_text(count > 0 ? msg : "No folders\non SD card");
#if CONFIG_CORE2_HW
                s_folder_list_display_active = (count > 0);
#endif
            }
        }

    } else if (strcmp(s_trigger, "files") == 0) {
        if (!s_params[0]) {
            screen_draw_text("Usage: files\n<folder>");
        } else if (!mount_sd_card_if_needed()) {
            screen_draw_text("No SD card");
        } else {
            char dir_path[MP3_MAX_PATH_LEN];
            int max_name = (int)sizeof(dir_path) - (int)strlen(MP3_ROOT_PATH) - 2;
            snprintf(dir_path, sizeof(dir_path), "%s/%.*s",
                     MP3_ROOT_PATH, max_name, s_params);
            DIR *d = opendir(dir_path);
            if (!d) {
                screen_draw_text("Folder not\nfound");
            } else {
#if CONFIG_CORE2_HW
                s_file_list_count = 0;
                int mp3_idx = 0, jpeg_idx = 0;
                strncpy(s_file_list_folder_trigger, s_params, sizeof(s_file_list_folder_trigger) - 1);
                s_file_list_folder_trigger[sizeof(s_file_list_folder_trigger) - 1] = '\0';
                strncpy(s_file_list_folder_path, dir_path, sizeof(s_file_list_folder_path) - 1);
                s_file_list_folder_path[sizeof(s_file_list_folder_path) - 1] = '\0';
#endif
                char msg[256];
                int msg_len;
#if CONFIG_CORE2_HW
                /* Truncate the header so it never wraps (reserve 1 char for
                 * the trailing ':'), keeping the tap math valid. */
                int max_cols = pf_list_max_cols();
                {
                    int hdr_len = (int)strlen(s_params);
                    if (hdr_len > max_cols - 1) hdr_len = max_cols - 1;
                    if (hdr_len < 0) hdr_len = 0;
                    msg_len = snprintf(msg, sizeof(msg), "%.*s:", hdr_len, s_params);
                }
#else
                msg_len = snprintf(msg, sizeof(msg), "%s:", s_params);
#endif
                char json_arr[512];
                int  ja = 0;
                bool jfirst = true;
                json_arr[ja++] = '[';
                int count = 0;
                struct dirent *e;
                while ((e = readdir(d)) != NULL) {
                    if (e->d_name[0] == '.') continue;
                    int nlen = (int)strlen(e->d_name);
                    int disp_len = nlen;
#if CONFIG_CORE2_HW
                    /* Truncate long names so this line never wraps, keeping
                     * the list display's one-line-per-entry tap math valid. */
                    if (disp_len > max_cols) disp_len = max_cols;
#endif
                    if (msg_len + 1 + disp_len < (int)sizeof(msg) - 1) {
                        msg[msg_len++] = '\n';
                        memcpy(msg + msg_len, e->d_name, (size_t)disp_len);
                        msg_len += disp_len;
                        msg[msg_len] = '\0';
#if CONFIG_CORE2_HW
                        if (s_file_list_count < PF_FILE_LIST_MAX) {
                            strncpy(s_file_list_names[s_file_list_count], e->d_name,
                                    sizeof(s_file_list_names[0]) - 1);
                            s_file_list_names[s_file_list_count]
                                [sizeof(s_file_list_names[0]) - 1] = '\0';
                            if (is_mp3_file_name(e->d_name)) {
                                s_file_list_types[s_file_list_count]  = PF_FILE_MP3;
                                s_file_list_subidx[s_file_list_count] = mp3_idx++;
                            } else if (is_jpeg_file_name(e->d_name)) {
                                s_file_list_types[s_file_list_count]  = PF_FILE_JPEG;
                                s_file_list_subidx[s_file_list_count] = jpeg_idx++;
                            } else {
                                s_file_list_types[s_file_list_count]  = PF_FILE_OTHER;
                                s_file_list_subidx[s_file_list_count] = -1;
                            }
                            s_file_list_count++;
                        }
#endif
                    }
                    int ja_need = (jfirst ? 0 : 1) + 4 + nlen;
                    if (ja + ja_need < (int)sizeof(json_arr) - 2) {
                        if (!jfirst) json_arr[ja++] = ',';
                        json_arr[ja++] = '\\'; json_arr[ja++] = '"';
                        memcpy(json_arr + ja, e->d_name, (size_t)nlen);
                        ja += nlen;
                        json_arr[ja++] = '\\'; json_arr[ja++] = '"';
                        jfirst = false;
                    }
                    count++;
                }
                closedir(d);
                json_arr[ja++] = ']';
                json_arr[ja]   = '\0';
                strncpy(s_pending_result, json_arr, sizeof(s_pending_result) - 1);
                s_pending_result[sizeof(s_pending_result) - 1] = '\0';
                s_pending_has_result = true;
                ESP_LOGI(TAG, "files: %d file(s) in %s, result: %s", count, dir_path, s_pending_result);
#if CONFIG_CORE2_HW
                if (count > 0) pf_list_enter_scale2();
#endif
                screen_draw_text(count > 0 ? msg : "Empty folder");
#if CONFIG_CORE2_HW
                s_file_list_display_active = (count > 0);
#endif
            }
        }

    } else if (strcmp(s_trigger, "reboot") == 0) {
        ESP_LOGW(TAG, "reboot: restarting device by command");
        screen_draw_text("Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();

    } else if (strcmp(s_trigger, "play") == 0) {
        s_mp3_ui_override_allowed = true;
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

    } else if (strcmp(s_trigger, "pause") == 0) {
        s_mp3_ui_override_allowed = true;
        if (s_mp3.active && !s_mp3.paused) {
            s_mp3.paused = true;
            s_mp3_resume_on_bt_reconnect = false;
#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
            s_bt_media_prime_pending = false;
            s_bt_media_prime_deadline = 0;
            s_bt_media_prime_target_bytes = BT_PCM_START_PRIME_BYTES;
            bt_media_stop_if_needed();
#endif
            mp3_request_ui_refresh();
        } else if (s_mp3.active && s_mp3.paused) {
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
        s_mp3_ui_override_allowed = true;
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
        s_mp3_ui_override_allowed = true;
        (void)mp3_advance_track(1, "next command");

    } else if (strcmp(s_trigger, "previous") == 0) {
        s_mp3_ui_override_allowed = true;
        (void)mp3_advance_track(-1, "previous command");

    } else if (strcmp(s_trigger, "forward") == 0) {
        s_mp3_ui_override_allowed = true;
        (void)mp3_queue_seek_relative((int32_t)MP3_SEEK_STEP_MS, "forward command");

    } else if (strcmp(s_trigger, "reverse") == 0) {
        s_mp3_ui_override_allowed = true;
        (void)mp3_queue_seek_relative(-(int32_t)MP3_SEEK_STEP_MS, "reverse command");

    } else if (strcmp(s_trigger, "volumeup") == 0) {
        s_mp3_ui_override_allowed = true;
        s_mp3.muted = false;
        s_mp3.volume += 5;
        if (s_mp3.volume > 100) s_mp3.volume = 100;
        nvs_write_u8(NVS_KEY_VOLUME, (uint8_t)s_mp3.volume);
        if (s_mp3.active) mp3_request_ui_refresh();

    } else if (strcmp(s_trigger, "volumedown") == 0) {
        s_mp3_ui_override_allowed = true;
        s_mp3.muted = false;
        s_mp3.volume -= 5;
        if (s_mp3.volume < 0) s_mp3.volume = 0;
        nvs_write_u8(NVS_KEY_VOLUME, (uint8_t)s_mp3.volume);
        if (s_mp3.active) mp3_request_ui_refresh();

    } else if (strcmp(s_trigger, "volumelevel") == 0) {
        s_mp3_ui_override_allowed = true;
        s_mp3.muted = false;
        int lvl = atoi(s_params);
        if (lvl < 0) lvl = 0;
        if (lvl > 100) lvl = 100;
        s_mp3.volume = lvl;
        nvs_write_u8(NVS_KEY_VOLUME, (uint8_t)s_mp3.volume);
        if (s_mp3.active) mp3_request_ui_refresh();

    } else if (strcmp(s_trigger, "mute") == 0) {
        s_mp3_ui_override_allowed = true;
        s_mp3.muted = !s_mp3.muted;
        if (s_mp3.active) mp3_request_ui_refresh();

    } else if (strcmp(s_trigger, "shuffle") == 0) {
        s_mp3_ui_override_allowed = true;
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
        s_mp3_ui_override_allowed = true;
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
        s_mp3_ui_override_allowed = true;
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

    } else if (strcmp(s_trigger, "visualizer") == 0) {
#if CONFIG_CORE2_HW
        s_mp3_ui_override_allowed = true;
        char mode[16] = {0};
        strncpy(mode, s_params, sizeof(mode) - 1);
        for (size_t i = 0; mode[i]; i++) mode[i] = (char)tolower((unsigned char)mode[i]);
        char *endptr = NULL;
        long style_num = (mode[0] != '\0') ? strtol(mode, &endptr, 10) : 0;
        if (strcmp(mode, "on") == 0) {
            s_mp3.visualizer = true;
        } else if (strcmp(mode, "off") == 0) {
            s_mp3.visualizer = false;
        } else if (endptr != NULL && endptr != mode && *endptr == '\0'
                   && style_num >= VISUALIZER_STYLE_MIN && style_num <= VISUALIZER_STYLE_MAX) {
            s_mp3.visualizer_style = (uint8_t)style_num;
            s_mp3.visualizer = true;
            nvs_write_u8(NVS_KEY_VISUALIZER_STYLE, s_mp3.visualizer_style);
        } else {
            s_mp3.visualizer = !s_mp3.visualizer;
        }
        nvs_write_u8(NVS_KEY_VISUALIZER, s_mp3.visualizer ? 1 : 0);
        if (!s_mp3.visualizer) core2_leds_off();
        mp3_log_mode_status("visualizer command");
        if (s_mp3.active) mp3_request_ui_refresh();
#endif

    } else if (strcmp(s_trigger, "visualizernext") == 0 || strcmp(s_trigger, "visualizerprevious") == 0) {
#if CONFIG_CORE2_HW
        s_mp3_ui_override_allowed = true;
        int style = (int)s_mp3.visualizer_style;
        if (style < VISUALIZER_STYLE_MIN || style > VISUALIZER_STYLE_MAX) style = VISUALIZER_STYLE_MIN;
        if (strcmp(s_trigger, "visualizernext") == 0) {
            style = (style >= VISUALIZER_STYLE_MAX) ? VISUALIZER_STYLE_MIN : style + 1;
        } else {
            style = (style <= VISUALIZER_STYLE_MIN) ? VISUALIZER_STYLE_MAX : style - 1;
        }
        s_mp3.visualizer_style = (uint8_t)style;
        s_mp3.visualizer = true;
        nvs_write_u8(NVS_KEY_VISUALIZER_STYLE, s_mp3.visualizer_style);
        nvs_write_u8(NVS_KEY_VISUALIZER, s_mp3.visualizer ? 1 : 0);
        mp3_log_mode_status(s_trigger);
        if (s_mp3.active) mp3_request_ui_refresh();
#endif

    } else if (strcmp(s_trigger, "ledcolor") == 0) {
#if CONFIG_CORE2_HW
        s_mp3.visualizer = false;
        if (strcasecmp(s_params, "off") == 0 || strcmp(s_params, "0") == 0) {
            core2_leds_off();
        } else {
            uint8_t r = 0, g = 0, b = 0;
            if (parse_color(s_params, &r, &g, &b)) {
                core2_leds_set_solid(r, g, b);
            } else {
                ESP_LOGW(TAG, "ledcolor: unrecognised '%s'", s_params);
            }
        }
#endif

    } else if (strcmp(s_trigger, "pair") == 0) {
        bt_cmd_pair_start();

    } else if (strcmp(s_trigger, "btstatus") == 0) {
        bt_cmd_status();

    } else if (strcmp(s_trigger, "btdisconnect") == 0) {
        bt_cmd_disconnect();

    } else if (strcmp(s_trigger, "btforget") == 0) {
        bt_cmd_forget();

#if CONFIG_HARDWARE_CORE2
    } else if (strcmp(s_trigger, "sleeptimer") == 0) {
        int mins = atoi(s_params);
        if (mins < 0)   mins = 0;
        if (mins > 120) mins = 120;
        s_sleep_timeout_s = (uint32_t)mins * 60u;
        nvs_write_u8(NVS_KEY_SLEEP_MIN, (uint8_t)mins);
        ESP_LOGI(TAG, "sleep timeout set to %d minutes", mins);

#if CONFIG_CORE2_HW
    } else if (strcmp(s_trigger, "sleeponpower") == 0) {
        char mode[16] = {0};
        strncpy(mode, s_params, sizeof(mode) - 1);
        for (size_t i = 0; mode[i]; i++) mode[i] = (char)tolower((unsigned char)mode[i]);
        if (strcmp(mode, "on") == 0) {
            s_sleep_while_powered = true;
        } else if (strcmp(mode, "off") == 0) {
            s_sleep_while_powered = false;
        } else {
            s_sleep_while_powered = !s_sleep_while_powered;
        }
        nvs_write_u8(NVS_KEY_SLEEP_ON_PWR, s_sleep_while_powered ? 1 : 0);
        ESP_LOGI(TAG, "sleep-while-powered: %s", s_sleep_while_powered ? "on" : "off");
#endif

    } else if (strcmp(s_trigger, "sleep") == 0) {
        ESP_LOGI(TAG, "sleep command — entering deep sleep");
        pf_enter_deep_sleep_with_touch_wake();
#endif

#if CONFIG_CORE2_HW
    } else if (strcmp(s_trigger, "battery") == 0) {
        s_battery_display_active = false;
        uint8_t vbat_h = 0, vbat_l = 0;
        bool read_ok = (core2_axp_read_reg(0x78, &vbat_h) == ESP_OK) &&
                       (core2_axp_read_reg(0x79, &vbat_l) == ESP_OK);
        if (!read_ok) {
            screen_draw_text("Battery read\nfailed");
        } else {
            int adc    = ((int)vbat_h << 4) | ((int)vbat_l & 0x0F);
            int vbat   = (int)((float)adc * 1.1f);   /* millivolts */
            int  level = (vbat - 3300) * 100 / 900;
            if (level < 0)   level = 0;
            if (level > 100) level = 100;
            bool charging = core2_axp_on_external_power();
            char scr[80];
            snprintf(scr, sizeof(scr), "Battery: %d%%\n%d mV%s", level, vbat,
                     charging ? "\nCharging" : "");
            pf_list_enter_scale2();
            screen_draw_text(scr);
            s_battery_display_active = true;
            snprintf(s_pending_result, sizeof(s_pending_result),
                     "{\\\"level\\\":%d,\\\"voltage_mv\\\":%d,\\\"charging\\\":%s}",
                     level, vbat, charging ? "true" : "false");
            s_pending_has_result = true;
            ESP_LOGI(TAG, "battery: %d%% %d mV charging=%d", level, vbat, charging);
        }
#endif

    } else if (strcmp(s_trigger, "listen") == 0) {
#if CONFIG_CORE2_HW
        s_pending_voice_query = true;
#endif

#if !CONFIG_HARDWARE_CYD
    } else if (strcmp(s_trigger, "askpic") == 0) {
        strncpy(s_pending_ask_text, s_params[0] ? s_params : "What is in this picture?",
                sizeof(s_pending_ask_text) - 1);
        s_pending_ask_text[sizeof(s_pending_ask_text) - 1] = '\0';
        strncpy(s_pending_ask_run_id, s_id, sizeof(s_pending_ask_run_id) - 1);
        s_pending_ask_run_id[sizeof(s_pending_ask_run_id) - 1] = '\0';
        s_pending_ask_pic = true;
        s_id[0] = '\0';   /* suppress the generic run/save below — pf_ask_picture()
                           * sends run/save + command/result together once done */

    } else if (strcmp(s_trigger, "askgpt") == 0) {
        strncpy(s_pending_askgpt_text, s_params[0] ? s_params : "What would you like to know?",
                sizeof(s_pending_askgpt_text) - 1);
        s_pending_askgpt_text[sizeof(s_pending_askgpt_text) - 1] = '\0';
        strncpy(s_pending_askgpt_run_id, s_id, sizeof(s_pending_askgpt_run_id) - 1);
        s_pending_askgpt_run_id[sizeof(s_pending_askgpt_run_id) - 1] = '\0';
        s_pending_askgpt = true;
        s_id[0] = '\0';   /* suppress the generic run/save below — pf_ask_gpt()
                           * sends run/save + command/result together once done */
#endif

    } else if (strcmp(s_trigger, "backup") == 0) {
        strncpy(s_pending_backup_run_id, s_id, sizeof(s_pending_backup_run_id) - 1);
        s_pending_backup_run_id[sizeof(s_pending_backup_run_id) - 1] = '\0';
        s_pending_backup = true;
        s_id[0] = '\0';   /* suppress the generic run/save below — pf_backup_sd()
                           * reports run/save + command/result once the upload finishes */

#if CONFIG_CORE2_HW
    } else if (strcmp(s_trigger, "aitts") == 0) {
        char mode[16] = {0};
        strncpy(mode, s_params, sizeof(mode) - 1);
        for (size_t i = 0; mode[i]; i++) mode[i] = (char)tolower((unsigned char)mode[i]);
        if (strcmp(mode, "on") == 0) {
            s_ai_tts_enabled = true;
        } else if (strcmp(mode, "off") == 0) {
            s_ai_tts_enabled = false;
        } else {
            s_ai_tts_enabled = !s_ai_tts_enabled;
        }
        nvs_write_u8(NVS_KEY_AI_TTS, s_ai_tts_enabled ? 1 : 0);
        ESP_LOGI(TAG, "AI TTS: %s", s_ai_tts_enabled ? "on" : "off");

    } else if (strcmp(s_trigger, "micsrc") == 0) {
        char mode[16] = {0};
        strncpy(mode, s_params, sizeof(mode) - 1);
        for (size_t i = 0; mode[i]; i++) mode[i] = (char)tolower((unsigned char)mode[i]);
        if (strcmp(mode, "grove") == 0) {
            s_mic_src_grove = true;
        } else if (strcmp(mode, "pdm") == 0) {
            s_mic_src_grove = false;
        } else {
            s_mic_src_grove = !s_mic_src_grove;
        }
        nvs_write_u8(NVS_KEY_MIC_SRC, s_mic_src_grove ? 1 : 0);
        ESP_LOGI(TAG, "Mic source: %s", s_mic_src_grove ? "grove (ADC)" : "pdm (built-in)");
#endif

#if CONFIG_CORE2_HW
    } else if (strcmp(s_trigger, "clock") == 0) {
        pf_clock_start(s_params);
        nvs_write_u8(NVS_KEY_CLOCK_MODE, (uint8_t)s_clock_mode);
    } else if (strcmp(s_trigger, "timezone") == 0) {
        if (s_params[0]) {
            apply_timezone(s_params);
            nvs_write_str(NVS_KEY_TIMEZONE, s_timezone);
            ESP_LOGI(TAG, "Timezone set to: %s", s_timezone);
        }
#endif

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
            int jpeg_folder_idx = jpeg_find_folder_trigger(s_trigger);
            if (jpeg_folder_idx >= 0) {
                int requested_idx = 0;
                const char *p = s_params;
                while (*p == ' ') p++;
                if (*p >= '1' && *p <= '9') {
                    int n = atoi(p);
                    if (n >= 1 && n <= 100) requested_idx = n - 1;
                }
                char file_name[MP3_MAX_FILE_LEN] = {0};
                int total = 0;
                if (!jpeg_get_nth_file(s_jpeg_folders[jpeg_folder_idx].folder_path,
                                       requested_idx, file_name, sizeof(file_name), &total)
                        || total <= 0) {
                    ESP_LOGW(TAG, "jpeg folder: no jpeg files in '%s'", s_trigger);
                    return;
                }
                char file_path[MP3_MAX_PATH_LEN + MP3_MAX_FILE_LEN + 4];
                snprintf(file_path, sizeof(file_path), "%s/%s",
                         s_jpeg_folders[jpeg_folder_idx].folder_path, file_name);
                s_mp3_ui_override_allowed = false;
                s_pending_jpeg = false;
                s_jpeg_folder_display_active = true;
                s_jpeg_folder_display_idx    = jpeg_folder_idx;
                s_jpeg_folder_image_idx      = requested_idx;
                strncpy(s_pending_jpeg_file_path, file_path, sizeof(s_pending_jpeg_file_path) - 1);
                s_pending_jpeg_file_path[sizeof(s_pending_jpeg_file_path) - 1] = '\0';
                s_pending_jpeg_file = true;
            } else {
                ESP_LOGW(TAG, "message: unknown trigger '%s'", s_trigger);
                return;
            }
        }
    }

    /* Signal the main loop to pulse the vibration motor (Core2 only). */
    s_pending_vibrate = true;

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

/* Captures the Location response header so the redirect loop can follow it.
 * esp_http_client_get_header() only reads request headers, not response ones. */
static esp_err_t jpeg_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data &&
        strcasecmp(evt->header_key, "Location") == 0) {
        char *loc = (char *)evt->user_data;
        strncpy(loc, evt->header_value, 1023);
        loc[1023] = '\0';
    }
    return ESP_OK;
}

#if CONFIG_CORE2_HW
/* Frees the cached decoded RGB565 image (if any) and resets pinch-to-zoom
 * view state to fit-to-screen. Called whenever s_jpeg_cache is freed or
 * about to be replaced by a new image. */
static void pf_free_jpeg_rgb_cache(void)
{
    if (s_jpeg_rgb565) { free(s_jpeg_rgb565); s_jpeg_rgb565 = NULL; }
    s_jpeg_rgb_w  = 0;
    s_jpeg_rgb_h  = 0;
    s_jpeg_zoom   = 1.0f;
    s_jpeg_pan_cx = 0.5f;
    s_jpeg_pan_cy = 0.5f;
}

/* Blits the current zoom/pan view of s_jpeg_rgb565 to the screen. No-op if
 * no image is currently decoded. */
static void pf_redraw_jpeg_view(void)
{
    if (!s_jpeg_rgb565 || s_jpeg_rgb_w <= 0 || s_jpeg_rgb_h <= 0) return;

    int crop_w = (int)((float)s_jpeg_rgb_w / s_jpeg_zoom);
    int crop_h = (int)((float)s_jpeg_rgb_h / s_jpeg_zoom);
    if (crop_w < 1) crop_w = 1;
    if (crop_h < 1) crop_h = 1;

    int crop_x = (int)(s_jpeg_pan_cx * (float)s_jpeg_rgb_w) - crop_w / 2;
    int crop_y = (int)(s_jpeg_pan_cy * (float)s_jpeg_rgb_h) - crop_h / 2;
    if (crop_x < 0) crop_x = 0;
    if (crop_y < 0) crop_y = 0;
    if (crop_x + crop_w > s_jpeg_rgb_w) crop_x = s_jpeg_rgb_w - crop_w;
    if (crop_y + crop_h > s_jpeg_rgb_h) crop_y = s_jpeg_rgb_h - crop_h;

    screen_draw_rgb565_region(s_jpeg_rgb565, s_jpeg_rgb_w, s_jpeg_rgb_h,
                               crop_x, crop_y, crop_w, crop_h);
}

/* Two-finger pinch-to-zoom/pan handler for the JPEG viewer. Runs on the
 * touch-poll task; redraws directly via screen_draw_rgb565_region(), which
 * takes the screen driver's own mutex. */
static void pf_pinch_handler(screen_pinch_phase_t phase, int x1, int y1, int x2, int y2)
{
    if (!s_jpeg_rgb565 || s_jpeg_rgb_w <= 0 || s_jpeg_rgb_h <= 0) return;
    if (phase == SCREEN_PINCH_END) return;

    float dx   = (float)(x2 - x1);
    float dy   = (float)(y2 - y1);
    float dist = sqrtf(dx * dx + dy * dy);
    int   mx   = (x1 + x2) / 2;
    int   my   = (y1 + y2) / 2;

    if (phase == SCREEN_PINCH_BEGIN) {
        s_pinch_base_dist = dist;
        s_pinch_base_zoom = s_jpeg_zoom;
        s_pinch_base_cx   = s_jpeg_pan_cx;
        s_pinch_base_cy   = s_jpeg_pan_cy;
        s_pinch_base_mx   = mx;
        s_pinch_base_my   = my;
        return;
    }

    if (s_pinch_base_dist < 1.0f) return;

    float new_zoom = s_pinch_base_zoom * (dist / s_pinch_base_dist);
    if (new_zoom < 1.0f) new_zoom = 1.0f;
    if (new_zoom > JPEG_ZOOM_MAX) new_zoom = JPEG_ZOOM_MAX;

    /* Two-finger drag pans the view: shift the crop centre opposite to the
     * midpoint's on-screen movement, scaled by the fraction of the image
     * visible at the baseline zoom, so content tracks the fingers. */
    bool landscape;
    screen_get_landscape(&landscape);
    int screen_w = landscape ? 320 : 240;
    int screen_h = landscape ? 240 : 320;

    float frac_dx = (float)(mx - s_pinch_base_mx) / (float)screen_w / s_pinch_base_zoom;
    float frac_dy = (float)(my - s_pinch_base_my) / (float)screen_h / s_pinch_base_zoom;

    float new_cx = s_pinch_base_cx - frac_dx;
    float new_cy = s_pinch_base_cy - frac_dy;

    float half_w = 0.5f / new_zoom;
    float half_h = 0.5f / new_zoom;
    if (new_cx < half_w)        new_cx = half_w;
    if (new_cx > 1.0f - half_w) new_cx = 1.0f - half_w;
    if (new_cy < half_h)        new_cy = half_h;
    if (new_cy > 1.0f - half_h) new_cy = 1.0f - half_h;

    s_jpeg_zoom   = new_zoom;
    s_jpeg_pan_cx = new_cx;
    s_jpeg_pan_cy = new_cy;

    pf_redraw_jpeg_view();
}
#endif /* CONFIG_CORE2_HW */

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
#if CONFIG_CORE2_HW
    /* Keep the decoded image so pinch-to-zoom can crop/rescale it without
     * re-decoding on every touch sample. */
    if (s_jpeg_rgb565) free(s_jpeg_rgb565);
    s_jpeg_rgb565 = rgb565_buf;
    s_jpeg_rgb_w  = (int)out.width;
    s_jpeg_rgb_h  = (int)out.height;
    pf_redraw_jpeg_view();
#else
    screen_draw_rgb565(rgb565_buf, (int)out.width, (int)out.height);
    free(rgb565_buf);
#endif
    return true;
}

static bool download_and_show_jpeg(const char *url)
{
#if CONFIG_HARDWARE_CYD
    /* No-PSRAM CYD: fetching a web image would require a SECOND concurrent TLS
     * context alongside the persistent Socket.IO websocket. This board's
     * internal RAM cannot hold two TLS sessions at once (see
     * [[project_cyd_single_tls_context]]) — the handshake attempt drives the
     * heap to near-OOM and takes the command socket down with it. (Full image
     * decode also needs a ~150KB RGB565 buffer that doesn't fit without PSRAM.)
     * Refuse cleanly so the websocket stays alive. */
    (void)url;
    ESP_LOGW(TAG, "jpeg: web image fetch unsupported on no-PSRAM CYD — keeping WS alive");
    screen_draw_text("Web images\nneed PSRAM");
    return false;
#else
    screen_draw_text("Loading image...");

    char effective_url[1024];
    strncpy(effective_url, url, sizeof(effective_url) - 1);
    effective_url[sizeof(effective_url) - 1] = '\0';

    esp_http_client_handle_t client = NULL;
    int64_t cl = 0;

    for (int redir = 0; redir < 10; redir++) {
        char loc_buf[1024] = {0};
        esp_http_client_config_t cfg = {
            .url           = effective_url,
            .method        = HTTP_METHOD_GET,
            .timeout_ms    = 15000,
            .event_handler = jpeg_http_event,
            .user_data     = loc_buf,
        };
        /* Certificate verification intentionally skipped for JPEG URLs */

        client = esp_http_client_init(&cfg);
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

        cl = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status >= 300 && status < 400) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = NULL;
            if (!loc_buf[0]) break;
            if (strncmp(loc_buf, "http://", 7) == 0 || strncmp(loc_buf, "https://", 8) == 0) {
                strncpy(effective_url, loc_buf, sizeof(effective_url) - 1);
                effective_url[sizeof(effective_url) - 1] = '\0';
            } else {
                /* Relative redirect — prepend scheme+host from current URL */
                char origin[512] = {0};
                const char *p = strstr(effective_url, "://");
                if (p) {
                    p += 3;
                    const char *slash = strchr(p, '/');
                    size_t origin_len = slash ? (size_t)(slash - effective_url) : strlen(effective_url);
                    if (origin_len < sizeof(origin))
                        memcpy(origin, effective_url, origin_len);
                }
                int path_max = (int)(sizeof(effective_url) - strlen(origin) - 2);
                if (path_max < 0) path_max = 0;
                snprintf(effective_url, sizeof(effective_url), "%s%s%.*s",
                         origin,
                         (loc_buf[0] == '/') ? "" : "/",
                         path_max, loc_buf);
            }
            ESP_LOGI(TAG, "jpeg: redirect %d -> %s", status, effective_url);
            continue;
        }

        if (status < 200 || status >= 300) {
            ESP_LOGE(TAG, "jpeg: HTTP %d for %s", status, effective_url);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            screen_draw_text("Image load\nfailed");
            return false;
        }

        break; /* 2xx — proceed with download */
    }

    if (!client) {
        ESP_LOGE(TAG, "jpeg: too many redirects");
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
#if CONFIG_CORE2_HW
    pf_free_jpeg_rgb_cache();
#endif
    s_jpeg_cache     = jpeg_buf;   /* take ownership — do NOT free */
    s_jpeg_cache_len = total;

    if (decode_and_show_jpeg(s_jpeg_cache, s_jpeg_cache_len)) {
        return true;
    }

    screen_draw_text("Image decode\nfailed");
    return false;
#endif /* CONFIG_HARDWARE_CYD */
}

static bool load_and_show_jpeg_file(const char *path)
{
    screen_draw_text("Loading image...");

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "jpeg file: cannot open %s", path);
        screen_draw_text("Image load\nfailed");
        return false;
    }
    fseek(f, 0, SEEK_END);
    long file_sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_sz <= 0 || file_sz > 512 * 1024) {
        ESP_LOGE(TAG, "jpeg file: bad size %ld for %s", file_sz, path);
        fclose(f);
        screen_draw_text("Image load\nfailed");
        return false;
    }
    uint8_t *buf = heap_caps_malloc((size_t)file_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc((size_t)file_sz);
    if (!buf) {
        ESP_LOGE(TAG, "jpeg file: cannot alloc %ld bytes", file_sz);
        fclose(f);
        screen_draw_text("Image load\nfailed");
        return false;
    }
    size_t nr = fread(buf, 1, (size_t)file_sz, f);
    fclose(f);
    if ((long)nr != file_sz) {
        ESP_LOGE(TAG, "jpeg file: read %zu of %ld bytes", nr, file_sz);
        free(buf);
        screen_draw_text("Image load\nfailed");
        return false;
    }

    if (s_jpeg_cache) { free(s_jpeg_cache); s_jpeg_cache = NULL; }
#if CONFIG_CORE2_HW
    pf_free_jpeg_rgb_cache();
#endif
    s_jpeg_cache     = buf;
    s_jpeg_cache_len = (int)nr;

    if (decode_and_show_jpeg(s_jpeg_cache, s_jpeg_cache_len)) {
        return true;
    }
    screen_draw_text("Image decode\nfailed");
    return false;
}

static void sync_time_before_tls(void)
{
    const int max_retries = 20;
    const time_t min_valid_epoch = 1000000000; /* ~2001-09-09 */

    ESP_LOGI(TAG, "SNTP: initializing time sync");
    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    for (int i = 0; i < max_retries; i++) {
        time_t now = time(NULL);
        if (now >= min_valid_epoch) {
            ESP_LOGI(TAG, "SNTP: time synced (%lld)", (long long)now);
            return;
        }
        ESP_LOGI(TAG, "SNTP: waiting for time sync... (%d/%d)", i + 1, max_retries);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGW(TAG, "SNTP: time not synced before timeout; current=%lld", (long long)time(NULL));
}

/* ── Connection step: Socket.IO + subscribeToFunRoom ────────────────────── */

#if CONFIG_HARDWARE_CYD
/* Create the TRIGGERcmd "computer" over the already-open websocket (Sails
 * virtual POST), so the no-PSRAM CYD never opens a second TLS context. On
 * Core2 (PSRAM) this is done over HTTPS before connecting. Returns ESP_OK if
 * s_computer_id is known afterwards. */
static esp_err_t cyd_create_computer_over_ws(void)
{
    if (s_computer_id[0]) return ESP_OK;   /* already provisioned */

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char computer_name[COMPUTER_NAME_LEN];
    snprintf(computer_name, sizeof(computer_name),
             "TCMDCYD-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "Creating computer over WS: %s", computer_name);
    pf_status_draw("Creating computer...");

    char data[96];
    snprintf(data, sizeof(data),
             "{\"name\":\"%s\",\"voice\":\"yellow display\"}", computer_name);

    char resp[640] = {0};
    esp_err_t r = socketio_vpost_sync(
        "/api/computer/save?__sails_io_sdk_version=0.11.0",
        s_hw_token, data, resp, sizeof(resp), 8000);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "computer/save(ws) failed: %s", esp_err_to_name(r));
        return r;
    }

    char cid[COMPUTER_ID_MAX_LEN] = {0};
    if (!json_extract_nested(resp, "data", "id", cid, sizeof(cid)) || !cid[0]) {
        ESP_LOGE(TAG, "computer/save(ws): could not parse data.id from: %.200s", resp);
        return ESP_FAIL;
    }

    strncpy(s_computer_id, cid, sizeof(s_computer_id) - 1);
    s_computer_id[sizeof(s_computer_id) - 1] = '\0';
    esp_err_t we = nvs_write_str(NVS_KEY_COMPID, s_computer_id);
    ESP_LOGI(TAG, "computer_id stored (ws): %s (nvs write=%s)",
             s_computer_id, esp_err_to_name(we));
    return ESP_OK;
}
#endif /* CONFIG_HARDWARE_CYD */

/* initial_connect: true for the very first connection attempt at boot.
 * In that case the "Connected!" status is skipped here — the boot-time
 * !restored_display_state block (after this returns) will draw either the
 * restored display state or fall back to the same "Connected!" text, so
 * drawing it here too would just be an extra redundant draw. */
static esp_err_t connect_and_subscribe(bool initial_connect)
{
    if (!s_mp3.active) pf_status_draw("Connecting to server...");

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

#if CONFIG_HARDWARE_CYD
    /* On the no-PSRAM CYD the computer is created over THIS websocket (rather
     * than a separate HTTPS call before connecting) so the WS stays the only
     * TLS context. Must happen before subscribing — the room name is the
     * computer id. */
    if (cyd_create_computer_over_ws() != ESP_OK || !s_computer_id[0]) {
        ESP_LOGE(TAG, "computer provisioning over WS failed — will retry");
        socketio_disconnect();
        return ESP_FAIL;
    }
#endif

    /* Subscribe via Sails.io virtual GET over the active socket.
     * Append __sails_io_sdk_version to the path (Python client style). */
    char sub_path[192];
    snprintf(sub_path, sizeof(sub_path),
             "/api/computer/subscribeToFunRoom?roomName=%s&__sails_io_sdk_version=0.11.0",
             s_computer_id);
    socketio_send_vget(sub_path, s_hw_token);

    if (s_mp3.active) {
        mp3_request_ui_refresh();
    } else if (!initial_connect) {
        pf_status_draw("Connected!\nWaiting for\ncommands...");
    }
    return ESP_OK;
}

/* ── SD card boot-time config ───────────────────────────────────────────── */

/* Returns true for key names whose values are secrets (WiFi creds, API keys). */
static bool sd_is_secret_key(const char *key)
{
    return (strcmp(key, "ssid")       == 0 || strcmp(key, "password")  == 0 ||
            strcmp(key, "ssid2")      == 0 || strcmp(key, "password2") == 0 ||
            strcmp(key, "ssid3")      == 0 || strcmp(key, "password3") == 0 ||
            strcmp(key, "openai_key") == 0);
}

/* Read /sdcard/secrets_config.txt (key=value lines) and process secrets.
 *
 * Default (secrets_in_sd absent or =0):
 *   Write secrets to NVS and, if all writes succeed, remove the secret lines
 *   from secrets_config.txt so the file no longer contains plaintext credentials.
 *
 * secrets_in_sd=1:
 *   Keep secrets on the SD card only — store in module-level vars, do NOT
 *   write to NVS.  Credentials are unavailable if the card is removed.
 *
 * Silently skips if the SD card is absent or the file does not exist. */
static void sd_apply_config_if_present(void)
{
    if (!mount_sd_card_if_needed()) return;

    FILE *f = fopen("/sdcard/secrets_config.txt", "r");
    if (!f) {
        ESP_LOGI(TAG, "sd config: no secrets_config.txt on SD card");
        return;
    }

    char ssid[64]       = {0};
    char password[128]  = {0};
    char ssid2[64]      = {0};
    char password2[128] = {0};
    char ssid3[64]      = {0};
    char password3[128] = {0};
    bool secrets_in_sd  = false;
#if CONFIG_CORE2_HW
    char openai_key[256] = {0};
#endif

    char line[384];
    while (fgets(line, sizeof(line), f)) {
        /* strip trailing newline / CR */
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln - 1] == '\r' || line[ln - 1] == '\n'))
            line[--ln] = '\0';

        /* skip blank lines and # comments */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        /* trim trailing whitespace from key */
        char *ke = key + strlen(key) - 1;
        while (ke >= key && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';

        /* trim leading whitespace from value */
        while (*val == ' ' || *val == '\t') val++;

        if (strcmp(key, "secrets_in_sd") == 0)
            secrets_in_sd = (val[0] == '1');
        else if (strcmp(key, "ssid") == 0)
            snprintf(ssid, sizeof(ssid), "%s", val);
        else if (strcmp(key, "password") == 0)
            snprintf(password, sizeof(password), "%s", val);
        else if (strcmp(key, "ssid2") == 0)
            snprintf(ssid2, sizeof(ssid2), "%s", val);
        else if (strcmp(key, "password2") == 0)
            snprintf(password2, sizeof(password2), "%s", val);
        else if (strcmp(key, "ssid3") == 0)
            snprintf(ssid3, sizeof(ssid3), "%s", val);
        else if (strcmp(key, "password3") == 0)
            snprintf(password3, sizeof(password3), "%s", val);
#if CONFIG_CORE2_HW
        else if (strcmp(key, "openai_key") == 0)
            snprintf(openai_key, sizeof(openai_key), "%s", val);
#endif
    }
    fclose(f);

    if (secrets_in_sd) {
        /* Keep secrets in SD card only — store in module vars, skip NVS writes. */
        ESP_LOGI(TAG, "sd config: secrets_in_sd=1, credentials kept on SD card only");
        s_sd_secrets_only = true;
        s_sd_wifi_count = 0;
        if (ssid[0]) {
            snprintf(s_sd_wifi_ssid[s_sd_wifi_count],
                     sizeof(s_sd_wifi_ssid[0]), "%s", ssid);
            snprintf(s_sd_wifi_pass[s_sd_wifi_count],
                     sizeof(s_sd_wifi_pass[0]), "%s", password);
            s_sd_wifi_count++;
        }
        if (ssid2[0]) {
            snprintf(s_sd_wifi_ssid[s_sd_wifi_count],
                     sizeof(s_sd_wifi_ssid[0]), "%s", ssid2);
            snprintf(s_sd_wifi_pass[s_sd_wifi_count],
                     sizeof(s_sd_wifi_pass[0]), "%s", password2);
            s_sd_wifi_count++;
        }
        if (ssid3[0]) {
            snprintf(s_sd_wifi_ssid[s_sd_wifi_count],
                     sizeof(s_sd_wifi_ssid[0]), "%s", ssid3);
            snprintf(s_sd_wifi_pass[s_sd_wifi_count],
                     sizeof(s_sd_wifi_pass[0]), "%s", password3);
            s_sd_wifi_count++;
        }
#if CONFIG_CORE2_HW
        if (openai_key[0]) {
            snprintf(s_sd_openai_key, sizeof(s_sd_openai_key), "%s", openai_key);
            ESP_LOGI(TAG, "sd config: OpenAI key loaded from SD (not written to NVS)");
        }
#endif
        return;
    }

    /* Default: write secrets to NVS, then remove them from secrets_config.txt. */
    bool any_secret = ssid[0] || ssid2[0] || ssid3[0];
#if CONFIG_CORE2_HW
    any_secret = any_secret || openai_key[0];
#endif
    bool all_ok = true;

    if (ssid[0]) {
        esp_err_t err = wifi_save_credentials(ssid, password);
        if (err == ESP_OK)
            ESP_LOGI(TAG, "sd config: saved WiFi SSID '%s'", ssid);
        else {
            ESP_LOGE(TAG, "sd config: WiFi save failed: %s", esp_err_to_name(err));
            all_ok = false;
        }
    }

    if (ssid2[0]) {
        esp_err_t err = wifi_save_credentials2(ssid2, password2);
        if (err == ESP_OK)
            ESP_LOGI(TAG, "sd config: saved WiFi SSID2 '%s'", ssid2);
        else {
            ESP_LOGE(TAG, "sd config: WiFi2 save failed: %s", esp_err_to_name(err));
            all_ok = false;
        }
    }

    if (ssid3[0]) {
        esp_err_t err = wifi_save_credentials3(ssid3, password3);
        if (err == ESP_OK)
            ESP_LOGI(TAG, "sd config: saved WiFi SSID3 '%s'", ssid3);
        else {
            ESP_LOGE(TAG, "sd config: WiFi3 save failed: %s", esp_err_to_name(err));
            all_ok = false;
        }
    }

#if CONFIG_CORE2_HW
    if (openai_key[0]) {
        esp_err_t err = nvs_write_str(NVS_KEY_STT, openai_key);
        if (err == ESP_OK)
            ESP_LOGI(TAG, "sd config: saved OpenAI key");
        else {
            ESP_LOGE(TAG, "sd config: OpenAI key save failed: %s", esp_err_to_name(err));
            all_ok = false;
        }
    }
#endif

    /* If all secrets were written to NVS, strip them from secrets_config.txt so the
     * file no longer contains plaintext credentials. */
    if (any_secret && all_ok) {
        FILE *rf = fopen("/sdcard/secrets_config.txt", "r");
        if (rf) {
            char *keep = (char *)malloc(4096);
            size_t keep_len = 0;
            if (keep) {
                char rline[384];
                while (fgets(rline, sizeof(rline), rf)) {
                    /* Identify the key on this line without modifying rline. */
                    char tmp[384];
                    strncpy(tmp, rline, sizeof(tmp) - 1);
                    tmp[sizeof(tmp) - 1] = '\0';
                    char *rp = tmp;
                    while (*rp == ' ' || *rp == '\t') rp++;
                    bool is_secret_line = false;
                    if (*rp != '\0' && *rp != '#') {
                        char *req = strchr(rp, '=');
                        if (req) {
                            *req = '\0';
                            char *rke = rp + strlen(rp) - 1;
                            while (rke >= rp && (*rke == ' ' || *rke == '\t'))
                                *rke-- = '\0';
                            is_secret_line = sd_is_secret_key(rp);
                        }
                    }
                    if (!is_secret_line) {
                        size_t rlen = strlen(rline);
                        if (keep_len + rlen < 4096) {
                            memcpy(keep + keep_len, rline, rlen);
                            keep_len += rlen;
                        }
                    }
                }
                fclose(rf);
                FILE *wf = fopen("/sdcard/secrets_config.txt", "w");
                if (wf) {
                    fwrite(keep, 1, keep_len, wf);
                    fclose(wf);
                    ESP_LOGI(TAG, "sd config: secrets moved to NVS and removed from secrets_config.txt");
                }
                free(keep);
            } else {
                fclose(rf);
                ESP_LOGW(TAG, "sd config: malloc failed, secrets left in secrets_config.txt");
            }
        }
    }
}

/* ── Main entry point ───────────────────────────────────────────────────── */

void picture_frame_run(void)
{
    /* Initialise display first — screen_init() creates s_draw_mutex which all
     * screen_draw_*() helpers require.  Must happen before any screen call. */
    screen_init();
    screen_set_touch_handler(pf_touch_handler);
#if CONFIG_CORE2_HW
    screen_set_pinch_handler(pf_pinch_handler);
#endif
    ESP_LOGI(TAG, "firmware version %s", g_firmware_version);
    mp3_ensure_task();
    sd_apply_config_if_present();

#if CONFIG_HARDWARE_CORE2
    {
        bool touch_wakeup = (esp_sleep_get_wakeup_causes() & ESP_SLEEP_WAKEUP_EXT1) != 0;
#if CONFIG_CORE2_HW
        mpu6886_init();
#endif
        if (touch_wakeup) {
            ESP_LOGI(TAG, "Woke from deep sleep via touch");
        }
        s_last_activity_tick = xTaskGetTickCount();
    }
#endif

    /* ── WiFi ────────────────────────────────────────────────────────────── */
    pf_status_draw("Waiting for WiFi...");

    esp_err_t wifi_ret;
    if (s_sd_secrets_only && s_sd_wifi_count > 0) {
        /* SD-only mode: connect directly with secrets_config.txt credentials; skip NVS. */
        screen_set_touch_handler(pf_wifi_skip_touch_handler);
        wifi_ret = ESP_FAIL;
        for (int i = 0; i < s_sd_wifi_count && wifi_ret != ESP_OK; i++) {
            ESP_LOGI(TAG, "WiFi: trying SD-only SSID '%s'", s_sd_wifi_ssid[i]);
            wifi_ret = wifi_connect_with_credentials(s_sd_wifi_ssid[i],
                                                     s_sd_wifi_pass[i]);
        }
        screen_set_touch_handler(pf_touch_handler);
    } else {
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
        screen_set_touch_handler(pf_wifi_skip_touch_handler);
        wifi_ret = wifi_connect();
        screen_set_touch_handler(pf_touch_handler);
    }

    if (wifi_ret != ESP_OK) {
        if (wifi_connect_was_aborted()) {
            ESP_LOGW(TAG, "WiFi skipped by user");
            pf_status_draw("No WiFi - tap to reboot");
        } else {
            screen_set_color(64, 0, 0);
            ESP_LOGE(TAG, "WiFi connect failed");
            pf_status_draw("WiFi failed - tap to reboot");
        }
        screen_set_touch_handler(pf_reboot_touch_handler);
        vTaskSuspend(NULL);
    }

    sync_time_before_tls();

    screen_off();

    /* ── NVS: read hw_token and computer_id ─────────────────────────────── */
    bool have_token   = nvs_read_str(NVS_KEY_TOKEN,  s_hw_token,   sizeof(s_hw_token));
    bool have_comp_id = nvs_read_str(NVS_KEY_COMPID, s_computer_id, sizeof(s_computer_id));
#if CONFIG_CORE2_HW
    nvs_read_str(NVS_KEY_VOICE_CONV, s_voice_conv_id, sizeof(s_voice_conv_id));
#endif
#if CONFIG_CORE2_HW
    /* Start the config HTTP server so the STT API key (and WiFi networks) can
     * be configured at http://<device-ip>/ even when the device is already
     * paired.  The "pair code" section shows "-----" when not actively pairing.
     *
     * CYD-only exception: this no-PSRAM board is too RAM-tight to run the httpd
     * (task + listening socket + lwip structs) concurrently with the pairing
     * lookup polls and the persistent Socket.IO TLS handshake — doing so left
     * the largest contiguous internal block below the handshake peak and
     * esp_tls_init() failed with ESP_ERR_NO_MEM. On CYD the config server is
     * deferred until AFTER the first websocket connects (see below). */
    http_pf_config_start(NULL);
#endif

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
        uint8_t volume = 0;
        if (nvs_read_u8(NVS_KEY_VOLUME, &volume)) {
            s_mp3.volume = (int)volume;
        }
#if CONFIG_CORE2_HW
        uint8_t visualizer = 0;
        if (nvs_read_u8(NVS_KEY_VISUALIZER, &visualizer)) {
            s_mp3.visualizer = (visualizer != 0);
        }
        uint8_t visualizer_style = 0;
        if (nvs_read_u8(NVS_KEY_VISUALIZER_STYLE, &visualizer_style)
            && (visualizer_style >= VISUALIZER_STYLE_MIN && visualizer_style <= VISUALIZER_STYLE_MAX)) {
            s_mp3.visualizer_style = visualizer_style;
        }
#endif
#if CONFIG_HARDWARE_CORE2
        uint8_t sleep_min = 0;
        if (nvs_read_u8(NVS_KEY_SLEEP_MIN, &sleep_min)) {
            s_sleep_timeout_s = (uint32_t)sleep_min * 60u;
        }
#endif
#if CONFIG_CORE2_HW
        uint8_t sleep_on_pwr = 0;
        if (nvs_read_u8(NVS_KEY_SLEEP_ON_PWR, &sleep_on_pwr)) {
            s_sleep_while_powered = (sleep_on_pwr != 0);
        }
        uint8_t ai_tts = 1;
        nvs_read_u8(NVS_KEY_AI_TTS, &ai_tts);
        s_ai_tts_enabled = (ai_tts != 0);
        uint8_t mic_src = 0;
        nvs_read_u8(NVS_KEY_MIC_SRC, &mic_src);
        s_mic_src_grove = (mic_src != 0);
        {
            char tz[sizeof(s_timezone)] = {0};
            if (nvs_read_str(NVS_KEY_TIMEZONE, tz, sizeof(tz)) && tz[0])
                strncpy(s_timezone, tz, sizeof(s_timezone) - 1);
        }
        setenv("TZ", s_timezone, 1);
        tzset();
#endif
    }
    /* Defer SD mount/index work until the main loop so boot UI is responsive. */
    s_mp3_next_mount_retry = xTaskGetTickCount();

    /* ── Pair code flow — runs until a user JWT is obtained ─────────────── */
    if (!have_token) {
        while (true) {  /* pair_loop — retries on network failure or 10-min timeout */

            char *pair_body = NULL;
            char pair_url[192];
            snprintf(pair_url, sizeof(pair_url),
                     "%s/pair?model=TCMDCORE2", TCMD_BASE_URL);
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
    /* CYD does this over the websocket (see cyd_create_computer_over_ws) to
     * avoid opening a second TLS context, so skip the HTTPS path here. */
#if !CONFIG_HARDWARE_CYD
    if (!have_comp_id) {
        /* Build a unique computer name from the WiFi base MAC */
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char computer_name[COMPUTER_NAME_LEN];
        snprintf(computer_name, sizeof(computer_name),
                 "TCMDCORE2-%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        ESP_LOGI(TAG, "Creating computer: %s", computer_name);
        pf_status_draw("Creating computer...");

        char save_url[192];
        snprintf(save_url, sizeof(save_url), "%s/api/computer/save", TCMD_BASE_URL);

        /* Body: name=TCMDCORE2-AABBCCDDEEFF&voice=core+2 */
        char form[64];
        snprintf(form, sizeof(form), "name=%s&voice=core+2", computer_name);

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
    }
#endif /* !CONFIG_HARDWARE_CYD */

#if CONFIG_CORE2_HW
    /* Log and configure AXP192 power rails for SK6812 LED bar.
     * M5Core2 Axp.begin() enables EXTEN (bit 6, 5V boost) and LDO3.
     * Our screen init leaves EXTEN and LDO3 in their boot defaults; enable
     * both here so at least one of the possible LED power sources is on. */
    {
        uint8_t reg12 = 0, reg28 = 0, reg94 = 0;
        core2_axp_read_reg(0x12, &reg12);
        core2_axp_read_reg(0x28, &reg28);
        core2_axp_read_reg(0x94, &reg94);
        ESP_LOGI("pf", "AXP192 pre-LED: reg12=0x%02X reg28=0x%02X reg94=0x%02X",
                 reg12, reg28, reg94);

        /* Enable EXTEN (5V boost, bit 6) — powers LED bar on some Core2 revisions */
        core2_axp_write_reg(0x12, reg12 | 0x40);

        /* Also try GPIO4 / reg 0x94 approach */
        core2_axp_write_reg(0x94, reg94 & (uint8_t)~0x02);

        uint8_t reg12_after = 0;
        core2_axp_read_reg(0x12, &reg12_after);
        ESP_LOGI("pf", "AXP192 post-LED: reg12=0x%02X", reg12_after);
    }
    core2_leds_init();
#endif

    /* NOTE: on CYD (no PSRAM) the folder-index rebuilds and the ~30 HTTPS
     * command-sync POSTs are deferred until AFTER the first websocket connects
     * (see the !restored_display_state block below). They fragment the 8-bit
     * heap enough that the persistent Socket.IO TLS handshake's alloc(5473)
     * cert-record buffer could no longer find a contiguous block. Connecting
     * the websocket first lets it use the fresh post-reboot heap (the same
     * state in which /pair succeeds), then sync runs with the WS holding only
     * ~6KB. Core2 has PSRAM and is unaffected either way, but the shared order
     * is harmless there. */

    /* ── Connect + subscribe loop ────────────────────────────────────────── */
    bool restored_display_state = false;

    while (true) {
        esp_err_t ret = connect_and_subscribe(!restored_display_state);
        if (ret != ESP_OK) {
            if (!s_mp3.active) pf_status_draw("Server connect\nfailed\nRetrying in 10s");
            vTaskDelay(pdMS_TO_TICKS(10000));
            socketio_disconnect();
            continue;
        }

        TickType_t last_ping_tick = xTaskGetTickCount();

        if (!restored_display_state) {
            restored_display_state = true;

            /* WS is up on the fresh heap — now do the memory-heavy boot work
             * that we deferred so it wouldn't fragment the heap before the
             * handshake: folder-index rebuilds + the command sync. */
            rebuild_mp3_folder_index();
            rebuild_jpeg_folder_index();
#if CONFIG_HARDWARE_CYD || CONFIG_CORE2_HW
            /* Register commands over the open websocket instead of opening a
             * second HTTPS connection at boot. cmd/save CREATES a new record
             * each call (it is not an upsert — every call returns "Created
             * successfully" with a new id), and the WS path can't fetch+diff
             * the existing command list, so sync ONLY right after the computer
             * is freshly created (a new computer has no commands yet).
             * have_comp_id is the value read from NVS at boot, so
             * !have_comp_id == "created this boot". On a normal reboot the
             * commands are already registered. */
            if (!have_comp_id) {
                pf_status_draw("Syncing commands...");
                sync_all_commands_ws();
            }
#else
            pf_status_draw("Syncing commands...");
            sync_all_commands(true);
#endif

#if CONFIG_HARDWARE_CYD
            /* CYD only: now that the Socket.IO TLS session is up, it's safe to
             * start the config HTTP server (deferred from boot so its httpd
             * task + socket didn't starve the TLS handshake on this no-PSRAM
             * board). On Core2 the server was already started at boot. */
            http_pf_config_start(NULL);
#endif

            /* Defer Classic BT reconnect until after the first Socket.IO/TLS
             * session is established; BT startup can otherwise starve mbedTLS
             * allocation during the reboot path. */
            bt_try_reconnect_on_boot();

            /* The "Syncing commands..." status above was the last thing drawn;
             * now that the one-time boot work is done, let any saved display
             * state (color/text/jpeg) draw over it. restore_display_state_from_nvs()
             * falls back to "Connected!\nWaiting for\ncommands..." itself when
             * there is no saved state, e.g. right after pairing. */
            if (s_mp3.active) {
                mp3_request_ui_refresh();
            }
            restore_display_state_from_nvs();
#if CONFIG_CORE2_HW
            {
                /* Read after restore so the SD-card restore path (which writes
                 * NVS) has already set the authoritative values. */
                uint8_t bs = 0, cm = 0;
                nvs_read_u8(NVS_KEY_BOOT_SHOW,  &bs);
                nvs_read_u8(NVS_KEY_CLOCK_MODE, &cm);
                /* bs 1=digital 2=analog; bs=0 legacy: infer from clock_mode */
                bool start_clock = (bs == 1 || bs == 2) ||
                                   (bs == 0 && cm != 0);
                if (start_clock) {
                    s_pending_jpeg = false;
                    s_pending_jpeg_file = false;
                    s_pending_jpeg_url[0] = '\0';
                    s_pending_jpeg_file_path[0] = '\0';
                    bool analog = (bs == 2) ||
                                  (bs == 0 && cm == (uint8_t)PF_CLOCK_ANALOG);
                    pf_clock_start(analog ? "analog" : "digital");
                }
            }
#endif
        }

        while (true) {
#if CONFIG_CORE2_HW
            if (s_menu_pending_item >= 0) {
                int item = s_menu_pending_item;
                s_menu_pending_item = -1;
                if (pf_menu_execute_item(item)) {
                    pf_menu_close();
                } else {
                    pf_menu_render();
                }
            }
            if (s_pending_folder_tap_idx >= 0) {
                int idx = s_pending_folder_tap_idx;
                s_pending_folder_tap_idx = -1;
                if (idx < s_folder_list_count) {
                    pf_menu_dispatch("files", s_folder_list_names[idx]);
                }
            }
#endif
            if (s_pending_bg_color) {
                s_pending_bg_color = false;
                screen_set_color(s_pending_bg_r, s_pending_bg_g, s_pending_bg_b);
                if (s_jpeg_cache) s_pending_jpeg_redraw = true;
            }

            if (s_pending_fg_color) {
                s_pending_fg_color = false;
                screen_set_text_color(s_pending_fg_r, s_pending_fg_g, s_pending_fg_b);
            }

            if (s_pending_font_scale > 0) {
                int scale = s_pending_font_scale;
                s_pending_font_scale = 0;
#if CONFIG_CORE2_HW
                if (s_menu_active) {
                    /* Track the new size without redrawing the on-screen
                     * text at it; the menu stays at its own font scale 2
                     * until it closes. */
                    screen_set_font_scale_silent(scale);
                } else
#endif
                screen_set_font_scale(scale);
            }

            if (s_pending_orientation >= 0) {
                bool landscape = (s_pending_orientation != 0);
                s_pending_orientation = -1;
                screen_set_landscape(landscape);
                if (s_jpeg_cache) s_pending_jpeg_redraw = true;
            }

            if (s_pending_text_draw) {
                s_pending_text_draw = false;
                if (s_mp3_saved_font_scale >= 0) {
                    screen_set_font_scale_silent(s_mp3_saved_font_scale);
                    s_mp3_saved_font_scale = -1;
                }
                ESP_LOGI(TAG, "apply text draw: '%.80s'", s_pending_text);
                screen_draw_text(s_pending_text[0] ? s_pending_text : " ");
            }

            if (s_pending_text_redraw_retries > 0 && !s_pending_jpeg && !s_pending_jpeg_redraw) {
                s_pending_text_redraw_retries--;
                screen_draw_text(s_last_text[0] ? s_last_text : " ");
            }

            /* Download + display JPEG from main task — blocking HTTP + decode */
            if (s_pending_jpeg_redraw) {
                s_pending_jpeg_redraw = false;
                if (s_jpeg_cache) {
                    decode_and_show_jpeg(s_jpeg_cache, s_jpeg_cache_len);
                }
            }

            if (s_pending_jpeg) {
                s_pending_jpeg = false;
                if (s_mp3_saved_font_scale >= 0) {
                    screen_set_font_scale_silent(s_mp3_saved_font_scale);
                    s_mp3_saved_font_scale = -1;
                }
                char jpeg_url[512];
                strncpy(jpeg_url, s_pending_jpeg_url, sizeof(jpeg_url) - 1);
                jpeg_url[sizeof(jpeg_url) - 1] = '\0';
                if (download_and_show_jpeg(jpeg_url)) {
                    strncpy(s_current_jpeg_url, jpeg_url, sizeof(s_current_jpeg_url) - 1);
                    s_current_jpeg_url[sizeof(s_current_jpeg_url) - 1] = '\0';
                }
            }

            if (s_pending_jpeg_file) {
                s_pending_jpeg_file = false;
                if (s_mp3_saved_font_scale >= 0) {
                    screen_set_font_scale_silent(s_mp3_saved_font_scale);
                    s_mp3_saved_font_scale = -1;
                }
                char file_path[512];
                strncpy(file_path, s_pending_jpeg_file_path, sizeof(file_path) - 1);
                file_path[sizeof(file_path) - 1] = '\0';
                if (load_and_show_jpeg_file(file_path)) {
                    strncpy(s_current_jpeg_url, file_path, sizeof(s_current_jpeg_url) - 1);
                    s_current_jpeg_url[sizeof(s_current_jpeg_url) - 1] = '\0';
                }
            }

            if (s_mp3_ui_pending) {
                if (!s_mp3.active) {
                    s_mp3_ui_pending = false;
                    if (s_mp3_saved_font_scale >= 0) {
                        screen_set_font_scale_silent(s_mp3_saved_font_scale);
                        s_mp3_saved_font_scale = -1;
                    }
                } else if (s_mp3_ui_override_allowed && !s_pending_jpeg && !s_pending_jpeg_redraw
#if CONFIG_CORE2_HW
                           && !s_menu_active
                           && !s_battery_display_active
                           && !s_folder_list_display_active
                           && !s_file_list_display_active
                           && !s_menu_result_active
#endif
                          ) {
                    s_mp3_ui_pending = false;
                    mp3_render_now_playing();
                }
                /* Keep pending=true when temporarily blocked by JPEG, UI override,
                 * the on-screen menu, or a menu-action result screen, so the
                 * next eligible main-loop tick will render now-playing once
                 * that screen is dismissed. */
            }

            /* Post command/result — dedicated result payload for commands that
             * return data (e.g. "folders").  Sent before run/save so the server
             * has the result ready when the MCP tool reply is triggered. */
            if (s_pending_has_result) {
                char result_json[640];
                snprintf(result_json, sizeof(result_json),
                         "{\"computer_id\":\"%s\",\"command_id\":\"%s\",\"result\":\"%s\"}",
                         s_computer_id, s_pending_run_id, s_pending_result);
                esp_err_t result_err = socketio_send_vpost("/api/command/result",
                                                           s_hw_token, result_json);
                ESP_LOGI(TAG, "command/result vpost → %s", esp_err_to_name(result_err));
                s_pending_has_result = false;
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

            if (s_pending_save) {
                s_pending_save = false;
                esp_err_t save_err = save_display_state_to_nvs();
                if (save_err != ESP_OK) {
                    ESP_LOGE(TAG, "save: NVS failed: %s", esp_err_to_name(save_err));
                    screen_draw_text("Save failed");
                } else {
                    ESP_LOGI(TAG, "save: display state persisted");
                    bool landscape = false;
                    int font_scale = 2;
                    uint8_t bg_r = 0, bg_g = 0, bg_b = 0;
                    uint8_t fg_r = 255, fg_g = 255, fg_b = 255;
                    screen_get_landscape(&landscape);
                    screen_get_font_scale(&font_scale);
                    screen_get_color(&bg_r, &bg_g, &bg_b);
                    screen_get_text_color(&fg_r, &fg_g, &fg_b);
                    bool music_active = s_mp3.active && s_mp3_ui_override_allowed;
                    char msg[640];
                    int mlen = 0;
                    mlen += snprintf(msg + mlen, sizeof(msg) - mlen,
                                     "Saved settings\n"
                                     "BG: %u,%u,%u\n"
                                     "FG: %u,%u,%u\n"
                                     "%s  Font %d\n"
                                     "Music: %s\n",
                                     bg_r, bg_g, bg_b,
                                     fg_r, fg_g, fg_b,
                                     landscape ? "Landscape" : "Portrait", font_scale,
                                     music_active ? "On" : "Off");
#if CONFIG_CORE2_HW
                    mlen += snprintf(msg + mlen, sizeof(msg) - mlen,
                                     "Clock: %s\n",
                                     s_clock_mode == PF_CLOCK_ANALOG  ? "Analog" :
                                     s_clock_mode == PF_CLOCK_DIGITAL ? "Digital" : "Off");
                    {
                        const char *bs =
                            (s_clock_mode == PF_CLOCK_ANALOG)  ? "Analog clock" :
                            (s_clock_mode == PF_CLOCK_DIGITAL)  ? "Digital clock" :
                            music_active                         ? "Music" :
                            (s_jpeg_cache && s_jpeg_cache_len > 0 && s_current_jpeg_url[0]) ? "JPEG" : "Text";
                        mlen += snprintf(msg + mlen, sizeof(msg) - mlen, "Boot: %s\n", bs);
                    }
                    {
                        const char *tz_label;
                        if      (strcmp(s_timezone, "EST5EDT,M3.2.0,M11.1.0")      == 0) tz_label = "Eastern";
                        else if (strcmp(s_timezone, "CST6CDT,M3.2.0,M11.1.0")      == 0) tz_label = "Central";
                        else if (strcmp(s_timezone, "MST7MDT,M3.2.0,M11.1.0")      == 0) tz_label = "Mountain";
                        else if (strcmp(s_timezone, "PST8PDT,M3.2.0,M11.1.0")      == 0) tz_label = "Pacific";
                        else if (strcmp(s_timezone, "AKST9AKDT,M3.2.0,M11.1.0")    == 0) tz_label = "Alaska";
                        else if (strcmp(s_timezone, "HST10")                        == 0) tz_label = "Hawaii";
                        else if (strcmp(s_timezone, "UTC0")                         == 0) tz_label = "UTC";
                        else if (strcmp(s_timezone, "GMT0BST,M3.5.0/1,M10.5.0")    == 0) tz_label = "London";
                        else if (strcmp(s_timezone, "CET-1CEST,M3.5.0,M10.5.0/3")  == 0) tz_label = "C.Europe";
                        else tz_label = s_timezone;
                        mlen += snprintf(msg + mlen, sizeof(msg) - mlen, "TZ: %s\n", tz_label);
                    }
#endif
                    if (!music_active && s_jpeg_cache && s_jpeg_cache_len > 0 && s_current_jpeg_url[0]) {
                        mlen += snprintf(msg + mlen, sizeof(msg) - mlen,
                                         "JPEG: %s\n", s_current_jpeg_url);
                    }
                    if (s_last_text[0]) {
                        char tline[65];
                        strncpy(tline, s_last_text, sizeof(tline) - 1);
                        tline[sizeof(tline) - 1] = '\0';
                        char *nl = strchr(tline, '\n');
                        if (nl) *nl = '\0';
                        mlen += snprintf(msg + mlen, sizeof(msg) - mlen, "Text: %s\n", tline);
                    }
                    snprintf(msg + mlen, sizeof(msg) - mlen, "%s",
                             s_sd_mounted ? "SD + NVS" : "NVS only");
                    s_save_result_saved_font_scale = font_scale;
                    screen_set_font_scale_silent(1);
                    screen_draw_text(msg);
                }
            }

#if CONFIG_HARDWARE_CORE2
#if CONFIG_CORE2_HW
            if (s_pending_vibrate) {
                s_pending_vibrate = false;
                uint8_t vib_reg = 0;
                if (core2_axp_read_reg(0x12, &vib_reg) == ESP_OK) {
                    (void)core2_axp_write_reg(0x12, vib_reg | 0x08);  /* LDO3 on */
                    vTaskDelay(pdMS_TO_TICKS(80));
                    (void)core2_axp_read_reg(0x12, &vib_reg);
                    (void)core2_axp_write_reg(0x12, vib_reg & (uint8_t)~0x08); /* LDO3 off */
                }
            }
            pf_clock_render();
            core2_poll_pwr_key();   /* voice query on PWR short press */
            if (s_pending_voice_query) {
                s_pending_voice_query = false;
                do_core2_voice_query();
            }
            if (s_pending_speak) {
                s_pending_speak = false;
                core2_tts_speak(s_pending_speak_text);
            }
#endif

#if CONFIG_BT_ENABLED && CONFIG_BT_A2DP_ENABLE
            if (s_avrc_pending_play_pause) {
                s_avrc_pending_play_pause = false;
                bool was_paused = s_mp3.paused;
                (void)pf_touch_handler(0, 0, SCREEN_GESTURE_TAP);
                ESP_LOGI(TAG, "avrc: single tap → %s", was_paused ? "play" : "pause");
            }
            {
                int step = s_avrc_pending_track_step;
                if (step != 0) {
                    s_avrc_pending_track_step = 0;
                    if (step > 0) {
                        ESP_LOGI(TAG, "avrc: double tap → next track");
                        if (mp3_advance_track(1, "avrc next")) mp3_request_ui_refresh();
                    } else {
                        ESP_LOGI(TAG, "avrc: triple tap → previous track");
                        if (mp3_advance_track(-1, "avrc prev")) mp3_request_ui_refresh();
                    }
                }
            }
#if CONFIG_CORE2_HW
            if (s_avrc_pending_voice) {
                s_avrc_pending_voice = false;
                ESP_LOGI(TAG, "avrc: long press → voice query (Core2 mic)");
                do_core2_voice_query();
            }
#endif
#endif

#if !CONFIG_HARDWARE_CYD
            if (s_pending_ask_pic) {
                s_pending_ask_pic = false;
                pf_ask_picture(s_pending_ask_text, s_pending_ask_run_id);
            }
            if (s_pending_askgpt) {
                s_pending_askgpt = false;
                pf_ask_gpt(s_pending_askgpt_text, s_pending_askgpt_run_id);
            }
#endif
            if (s_pending_backup) {
                s_pending_backup = false;
                if (s_backup_running) {
                    ESP_LOGW(TAG, "backup: already running, ignoring re-dispatch");
                } else {
                    s_backup_running = true;
                    if (xTaskCreate(pf_backup_task, "pf_backup", 8192, NULL,
                                    4, NULL) != pdPASS) {
                        s_backup_running = false;
                        ESP_LOGE(TAG, "backup: failed to create task");
                        screen_draw_text("Backup failed\n(no memory)");
                    }
                }
            }

#if CONFIG_CORE2_HW
            {
                /* Poll IMU every 2 s; reset idle timer on motion so the device
                 * stays awake while being held or moved. */
                static TickType_t s_imu_last_poll = 0;
                TickType_t now = xTaskGetTickCount();
                if ((TickType_t)(now - s_imu_last_poll) >= pdMS_TO_TICKS(2000)) {
                    s_imu_last_poll = now;
                    if (mpu6886_motion_detected(500)) {
                        s_last_activity_tick = now;
                    }
                }
            }
#endif

            if (s_sleep_timeout_s > 0 &&
                !(s_mp3.active && !s_mp3.paused) &&
                (s_sleep_while_powered || !core2_axp_treat_as_powered()) &&
                (TickType_t)(xTaskGetTickCount() - s_last_activity_tick) >=
                    pdMS_TO_TICKS(s_sleep_timeout_s * 1000u)) {
                ESP_LOGI(TAG, "sleeponpower=%d on_external_power=%d treat_as_powered=%d",
                         s_sleep_while_powered, core2_axp_on_external_power(),
                         core2_axp_treat_as_powered());
                ESP_LOGI(TAG, "Idle timeout (%d s) — entering deep sleep",
                         CONFIG_CORE2_SLEEP_TIMEOUT_S);
                /* MPU6886 INT is not wired to the ESP32 on Core2. */
                pf_enter_deep_sleep_with_touch_wake();
            }
#endif

            vTaskDelay(pdMS_TO_TICKS(200));   /* poll every 200 ms */

            if (!s_sd_mounted && s_mp3_next_mount_retry != 0 &&
                xTaskGetTickCount() >= s_mp3_next_mount_retry) {
                ESP_LOGI(TAG, "sd: retrying deferred mount");
                bool was_mounted = s_sd_mounted;
                rebuild_mp3_folder_index();
                rebuild_jpeg_folder_index();
                if (!was_mounted && s_sd_mounted) {
                    ESP_LOGI(TAG, "sd: mount restored; reconciling MP3 folder commands");
                    sync_all_commands(true);
                    if (s_mp3_autostart && s_mp3_folder_count > 0) {
                        s_mp3_autostart = false;
                        mp3_start_track(0, -1, false);
                    }
                }
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
                if (!s_mp3.active) pf_status_draw("Reconnecting...");
                vTaskDelay(pdMS_TO_TICKS(5000));
                socketio_disconnect();
                break;
            }
        }
    }
}
