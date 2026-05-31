/*
 * sendspin_client.cpp
 *
 * Minimal C++ wrapper around sendspin-cpp.  Exposes a plain-C API so
 * picture_frame.c (a C translation unit) can start/stop the client.
 *
 * client.loop() runs in a dedicated FreeRTOS task (not the main task)
 * to avoid overflowing the main task's stack with sendspin's JSON/WebSocket
 * processing.  Audio is discarded by the player listener; the primary purpose
 * is to make the device discoverable as a Sendspin receiver.
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
#include "sendspin/player_role.h"

using namespace sendspin;

static const char *TAG = "sendspin";

/* ── Listener: discard incoming audio frames ─────────────────────────── */
struct DiscardPlayer : PlayerRoleListener {
    size_t on_audio_write(uint8_t *data, size_t length, uint32_t timeout_ms) override {
        (void)data;
        /* Sleep for timeout_ms so the player task doesn't busy-loop and starve
         * other tasks.  Cap at 100 ms so stop() is responsive. */
        uint32_t delay_ms = timeout_ms > 0 ? timeout_ms : 10;
        if (delay_ms > 100) delay_ms = 100;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        return length;
    }
};

/* ── Provider: ready when WiFi has an IP ────────────────────────────────── */
struct WifiProvider : SendspinNetworkProvider {
    bool is_network_ready() override {
        char ip[16];
        return wifi_get_ip(ip, sizeof(ip)) == ESP_OK && ip[0] != '\0';
    }
};

/* ── Module state ────────────────────────────────────────────────────────── */
static SendspinClient  *s_client          = nullptr;
static DiscardPlayer   *s_player_listener = nullptr;
static WifiProvider    *s_net_provider    = nullptr;
static volatile bool    s_running         = false;
static volatile bool    s_loop_running    = false;
static TaskHandle_t     s_loop_task       = nullptr;

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
    cfg.software_version = "2.0.281";

    s_player_listener = new DiscardPlayer();
    s_net_provider    = new WifiProvider();
    s_client          = new SendspinClient(std::move(cfg));

    PlayerRoleConfig pcfg;
    pcfg.audio_formats = {
        {SendspinCodecFormat::PCM, 2, 44100, 16},
        {SendspinCodecFormat::PCM, 2, 48000, 16},
    };
    /* Default 1 MB ring buffer exhausts the Core2's PSRAM, starving the SPI
     * DMA allocator used by the display driver.  16 KB is plenty for a discard
     * player whose on_audio_write never accumulates a backlog. */
    pcfg.audio_buffer_capacity = 16 * 1024;
    pcfg.priority = 5;  /* low priority: discard player has no real-time deadline */
    auto &player = s_client->add_player(std::move(pcfg));
    player.set_listener(s_player_listener);
    s_client->set_network_provider(s_net_provider);

    if (!s_client->start_server()) {
        ESP_LOGE(TAG, "start_server() failed");
        delete s_client;          s_client = nullptr;
        delete s_player_listener; s_player_listener = nullptr;
        delete s_net_provider;    s_net_provider = nullptr;
        return;
    }

    /* Start the loop task with its own 8 KB stack so client.loop() JSON/WS
     * processing does not overflow the main task's stack. */
    s_loop_running = true;
    xTaskCreate(sendspin_loop_task, "sendspin_loop", 8192, nullptr, 3, &s_loop_task);

    s_running = true;
    ESP_LOGI(TAG, "started (id=%s)", client_id);
}

extern "C" void sendspin_client_stop(void)
{
    if (!s_running || !s_client) return;

    s_loop_running = false;
    /* Wait for the loop task to exit (it checks s_loop_running every 20 ms). */
    for (int i = 0; i < 20 && s_loop_task != nullptr; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    s_client->disconnect(SendspinGoodbyeReason::SHUTDOWN);

    delete s_client;          s_client = nullptr;
    delete s_player_listener; s_player_listener = nullptr;
    delete s_net_provider;    s_net_provider = nullptr;

    s_running = false;
    ESP_LOGI(TAG, "stopped");
}

extern "C" bool sendspin_client_is_running(void)
{
    return s_running;
}

/* sendspin_client_loop() is kept for API compatibility but is now a no-op:
 * the loop task calls client.loop() internally. */
extern "C" void sendspin_client_loop(void) {}
