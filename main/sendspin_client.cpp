/*
 * sendspin_client.cpp
 *
 * Minimal C++ wrapper around sendspin-cpp.  Exposes a plain-C API so
 * picture_frame.c (a C translation unit) can start/stop the client.
 *
 * The player role is intentionally not added: the Core2's internal SRAM
 * is nearly fully committed between wolfSSL, BT, and the existing tasks,
 * and the player/sync task stack + decoder init would exhaust the
 * DMA-capable heap that the SPI display driver needs.  The device still
 * appears as a Sendspin node on the network (HTTP/WebSocket server runs
 * on port 8928) but does not advertise audio capability.
 *
 * client.loop() runs in a dedicated FreeRTOS task (not the main task) to
 * isolate its JSON/WebSocket stack usage from picture_frame.c's deep
 * call stack.
 */

#include "sendspin_client.h"

extern "C" {
#include "esp_mac.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

#include "sendspin/client.h"

using namespace sendspin;

static const char *TAG = "sendspin";

/* ── Provider: ready when WiFi has an IP ────────────────────────────────── */
struct WifiProvider : SendspinNetworkProvider {
    bool is_network_ready() override {
        char ip[16];
        return wifi_get_ip(ip, sizeof(ip)) == ESP_OK && ip[0] != '\0';
    }
};

/* ── Module state ────────────────────────────────────────────────────────── */
static SendspinClient *s_client       = nullptr;
static WifiProvider   *s_net_provider = nullptr;
static volatile bool   s_running      = false;
static volatile bool   s_loop_running = false;
static TaskHandle_t    s_loop_task    = nullptr;

/* ── Loop task: drives client.loop() on its own stack ───────────────────── */
static void sendspin_loop_task(void *arg)
{
    (void)arg;
    while (s_loop_running) {
        if (s_client) s_client->loop();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_loop_task = nullptr;
    vTaskDelete(nullptr);
}

/* ── Public C API ────────────────────────────────────────────────────────── */

extern "C" void sendspin_client_start(void)
{
    if (s_running) return;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char client_id[18];
    snprintf(client_id, sizeof(client_id), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    SendspinClientConfig cfg;
    cfg.client_id        = client_id;
    cfg.name             = "Picture Frame";
    cfg.product_name     = "ESP32 Picture Frame";
    cfg.manufacturer     = "TriggerCMD";
    cfg.software_version = "2.0.283";

    s_net_provider = new WifiProvider();
    s_client       = new SendspinClient(std::move(cfg));
    s_client->set_network_provider(s_net_provider);

    /* No player role: adding it would start a sync/decode task whose stack
     * and decoder init exhaust the DMA-capable internal SRAM on the Core2. */

    if (!s_client->start_server()) {
        ESP_LOGE(TAG, "start_server() failed");
        delete s_client;       s_client = nullptr;
        delete s_net_provider; s_net_provider = nullptr;
        return;
    }

    s_loop_running = true;
    xTaskCreate(sendspin_loop_task, "sendspin_loop", 6144, nullptr, 3, &s_loop_task);

    s_running = true;
    ESP_LOGI(TAG, "started (id=%s)", client_id);
}

extern "C" void sendspin_client_stop(void)
{
    if (!s_running || !s_client) return;

    s_loop_running = false;
    for (int i = 0; i < 20 && s_loop_task != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    s_client->disconnect(SendspinGoodbyeReason::SHUTDOWN);

    delete s_client;       s_client = nullptr;
    delete s_net_provider; s_net_provider = nullptr;

    s_running = false;
    ESP_LOGI(TAG, "stopped");
}

extern "C" bool sendspin_client_is_running(void)
{
    return s_running;
}

/* No-op: loop is driven by the dedicated sendspin_loop task. */
extern "C" void sendspin_client_loop(void) {}
