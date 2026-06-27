#include "wifi_manager.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1
#define WIFI_ABORT_BIT       BIT2
#define WIFI_MAX_RETRIES     2   /* retries per attempt; callers alternate SSIDs across rounds */

#define NVS_NAMESPACE        "wifi_cfg"
#define NVS_KEY_SSID         "ssid"
#define NVS_KEY_PASS         "password"
#define NVS_KEY_SSID2        "ssid2"
#define NVS_KEY_PASS2        "password2"
#define NVS_KEY_SSID3        "ssid3"
#define NVS_KEY_PASS3        "password3"

static const char *TAG = "wifi";

/* Hostname reported to DHCP/routers (replaces the default "espressif").
 * Picked per hardware variant at build time. */
#if CONFIG_HARDWARE_CORE2
#define WIFI_STA_HOSTNAME    "tcmd-core2"
#elif CONFIG_HARDWARE_CYD
#define WIFI_STA_HOSTNAME    "tcmd-cyd"
#elif CONFIG_HARDWARE_PICTURE_FRAME
#define WIFI_STA_HOSTNAME    "tcmd-picture-frame"
#elif CONFIG_HARDWARE_JC3248W535
#define WIFI_STA_HOSTNAME    "tcmd-screen"
#elif CONFIG_HARDWARE_DEVKITC
#define WIFI_STA_HOSTNAME    "tcmd-devkitc"
#elif CONFIG_HARDWARE_ESP32S3_CAM
#define WIFI_STA_HOSTNAME    "tcmd-cam"
#elif CONFIG_HARDWARE_ATOMS3_LITE
#define WIFI_STA_HOSTNAME    "tcmd-atoms3-lite"
#elif CONFIG_HARDWARE_TCMD_ATOM_ECHO
#define WIFI_STA_HOSTNAME    "tcmd-atom-echo"
#elif CONFIG_HARDWARE_BIKE_TRACKER
#define WIFI_STA_HOSTNAME    "tcmd-bike-tracker"
#endif

/* Persistent across connection attempts ------------------------------------ */
static bool            s_stack_initialized = false;
static bool            s_wifi_started      = false;
static esp_netif_t    *s_netif             = NULL;

/* Per-attempt state -------------------------------------------------------- */
static EventGroupHandle_t s_wifi_event_group = NULL;
static int                s_retry_count      = 0;
static volatile bool      s_abort_requested  = false;

/* -------------------------------------------------------------------------- */

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_abort_requested) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_ABORT_BIT);
        } else if (s_retry_count < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "Retry %d/%d ...", s_retry_count, WIFI_MAX_RETRIES);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Initialise the WiFi/netif stack once ------------------------------------- */

static void wifi_stack_init(void)
{
    if (s_stack_initialized) return;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

#ifdef WIFI_STA_HOSTNAME
    /* Show a friendly name on the router/network instead of "espressif" */
    esp_err_t herr = esp_netif_set_hostname(s_netif, WIFI_STA_HOSTNAME);
    if (herr != ESP_OK) {
        ESP_LOGW(TAG, "set hostname failed: %s", esp_err_to_name(herr));
    }
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_stack_initialized = true;
}

void wifi_stack_init_public(void) { wifi_stack_init(); }

/* -------------------------------------------------------------------------- */

esp_err_t wifi_connect_with_credentials(const char *ssid, const char *password)
{
    wifi_stack_init();

    /* Stop a previous attempt cleanly before reconfiguring */
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
        vTaskDelay(pdMS_TO_TICKS(100)); /* let the stack settle */
    }

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    s_wifi_event_group = xEventGroupCreate();
    s_retry_count = 0;

    esp_event_handler_instance_t h_any, h_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &h_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &h_got_ip));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid,     ssid,     sizeof(wifi_cfg.sta.ssid)     - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = (strlen(password) == 0)
                                      ? WIFI_AUTH_OPEN
                                      : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* A2DP + TLS handshake is sensitive to Wi-Fi modem sleep latency. */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable WiFi power save: %s", esp_err_to_name(ps_err));
    }
    s_wifi_started = true;

    ESP_LOGI(TAG, "Connecting to \"%s\" ...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_ABORT_BIT,
                                           pdFALSE, pdFALSE,
                                           portMAX_DELAY);

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, h_any);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, h_got_ip);
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;

    if (bits & WIFI_CONNECTED_BIT) { s_abort_requested = false; return ESP_OK; }
    if (bits & WIFI_ABORT_BIT) {
        ESP_LOGW(TAG, "WiFi connect aborted by user");
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "Failed to connect to \"%s\"", ssid);
    return ESP_FAIL;
}

/* -------------------------------------------------------------------------- */

void wifi_connect_abort(void)
{
    s_abort_requested = true;
    if (s_wifi_event_group) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_ABORT_BIT);
    }
}

bool wifi_connect_was_aborted(void) { return s_abort_requested; }

esp_err_t wifi_connect(void)
{
    char ssid[33]      = {0};
    char password[65]  = {0};
    char ssid2[33]     = {0};
    char password2[65] = {0};
    char ssid3[33]     = {0};
    char password3[65] = {0};

    s_abort_requested = false;

    /* Prefer credentials stored in NVS over Kconfig defaults */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len;
        len = sizeof(ssid);      nvs_get_str(nvs, NVS_KEY_SSID,  ssid,      &len);
        len = sizeof(password);  nvs_get_str(nvs, NVS_KEY_PASS,  password,  &len);
        len = sizeof(ssid2);     nvs_get_str(nvs, NVS_KEY_SSID2, ssid2,     &len);
        len = sizeof(password2); nvs_get_str(nvs, NVS_KEY_PASS2, password2, &len);
        len = sizeof(ssid3);     nvs_get_str(nvs, NVS_KEY_SSID3, ssid3,     &len);
        len = sizeof(password3); nvs_get_str(nvs, NVS_KEY_PASS3, password3, &len);
        nvs_close(nvs);
    }

    /* No hardcoded fallback SSID. A compiled-in default network name would be a
     * security risk: a factory-fresh or unprovisioned device could silently
     * auto-join an attacker's open AP advertising that name. If no network is
     * stored in any NVS slot, connect nothing and let the caller provision. */

    /* Build a list of the configured SSIDs so the round loop is uniform. Empty
     * slots become NULL and are skipped. */
    const char *ssids[3]     = { ssid[0]  ? ssid  : NULL, ssid2[0] ? ssid2 : NULL, ssid3[0] ? ssid3 : NULL };
    const char *passwords[3] = { ssid[0]  ? password : NULL, ssid2[0] ? password2 : NULL, ssid3[0] ? password3 : NULL };

    /* Try each configured SSID in turn, cycling through up to 3 rounds. */
    for (int round = 0; round < 3 && !s_abort_requested; round++) {
        for (int i = 0; i < 3 && !s_abort_requested; i++) {
            if (!ssids[i]) continue;
            ESP_LOGI(TAG, "Round %d/3: trying \"%s\"", round + 1, ssids[i]);
            if (wifi_connect_with_credentials(ssids[i], passwords[i]) == ESP_OK) return ESP_OK;
        }
    }
    return ESP_FAIL;
}

/* -------------------------------------------------------------------------- */

bool wifi_has_stored_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    char ssid[33] = {0};
    size_t len = sizeof(ssid);
    esp_err_t ret = nvs_get_str(nvs, NVS_KEY_SSID, ssid, &len);
    nvs_close(nvs);

    /* len includes the null terminator; len > 1 means non-empty string */
    return (ret == ESP_OK) && (len > 1);
}

esp_err_t wifi_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    if (ret == ESP_OK) ret = nvs_set_str(nvs, NVS_KEY_PASS, password);
    if (ret == ESP_OK) ret = nvs_commit(nvs);
    nvs_close(nvs);
    return ret;
}

esp_err_t wifi_save_credentials2(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    if (ssid && ssid[0]) {
        ret = nvs_set_str(nvs, NVS_KEY_SSID2, ssid);
        if (ret == ESP_OK) ret = nvs_set_str(nvs, NVS_KEY_PASS2, password ? password : "");
    } else {
        nvs_erase_key(nvs, NVS_KEY_SSID2);
        nvs_erase_key(nvs, NVS_KEY_PASS2);
    }
    if (ret == ESP_OK) ret = nvs_commit(nvs);
    nvs_close(nvs);
    return ret;
}

esp_err_t wifi_save_credentials3(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    if (ssid && ssid[0]) {
        ret = nvs_set_str(nvs, NVS_KEY_SSID3, ssid);
        if (ret == ESP_OK) ret = nvs_set_str(nvs, NVS_KEY_PASS3, password ? password : "");
    } else {
        nvs_erase_key(nvs, NVS_KEY_SSID3);
        nvs_erase_key(nvs, NVS_KEY_PASS3);
    }
    if (ret == ESP_OK) ret = nvs_commit(nvs);
    nvs_close(nvs);
    return ret;
}

void wifi_get_ssid2(char *buf, size_t len)
{
    nvs_handle_t nvs;
    buf[0] = '\0';
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    nvs_get_str(nvs, NVS_KEY_SSID2, buf, &len);
    nvs_close(nvs);
}

void wifi_get_ssid3(char *buf, size_t len)
{
    nvs_handle_t nvs;
    buf[0] = '\0';
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
    nvs_get_str(nvs, NVS_KEY_SSID3, buf, &len);
    nvs_close(nvs);
}

esp_err_t wifi_get_ip(char *buf, size_t buf_len)
{
    if (!s_netif) return ESP_FAIL;
    esp_netif_ip_info_t info;
    esp_err_t ret = esp_netif_get_ip_info(s_netif, &info);
    if (ret == ESP_OK) {
        snprintf(buf, buf_len, IPSTR, IP2STR(&info.ip));
    }
    return ret;
}
