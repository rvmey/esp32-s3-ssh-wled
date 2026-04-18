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
#define WIFI_MAX_RETRIES     10

#define NVS_NAMESPACE        "wifi_cfg"
#define NVS_KEY_SSID         "ssid"
#define NVS_KEY_PASS         "password"

static const char *TAG = "wifi";

/* Persistent across connection attempts ------------------------------------ */
static bool            s_stack_initialized = false;
static bool            s_wifi_started      = false;
static esp_netif_t    *s_netif             = NULL;

/* Per-attempt state -------------------------------------------------------- */
static EventGroupHandle_t s_wifi_event_group = NULL;
static int                s_retry_count      = 0;

/* -------------------------------------------------------------------------- */

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRIES) {
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
    s_wifi_started = true;

    ESP_LOGI(TAG, "Connecting to \"%s\" ...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           portMAX_DELAY);

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, h_any);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, h_got_ip);
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;

    if (bits & WIFI_CONNECTED_BIT) return ESP_OK;

    ESP_LOGE(TAG, "Failed to connect to \"%s\"", ssid);
    return ESP_FAIL;
}

/* -------------------------------------------------------------------------- */

esp_err_t wifi_connect(void)
{
    char ssid[33]     = {0};
    char password[65] = {0};

    /* Prefer credentials stored in NVS over Kconfig defaults */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t ssid_len = sizeof(ssid);
        size_t pass_len = sizeof(password);
        nvs_get_str(nvs, NVS_KEY_SSID, ssid, &ssid_len);
        nvs_get_str(nvs, NVS_KEY_PASS, password, &pass_len);
        nvs_close(nvs);
    }

    if (ssid[0] == '\0') {
        strncpy(ssid,     CONFIG_WIFI_SSID,     sizeof(ssid)     - 1);
        strncpy(password, CONFIG_WIFI_PASSWORD, sizeof(password) - 1);
    }

    return wifi_connect_with_credentials(ssid, password);
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
