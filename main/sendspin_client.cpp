/*
 * sendspin_client.cpp
 *
 * Minimal C++ wrapper around sendspin-cpp.  Exposes a plain-C API so
 * picture_frame.c (a C translation unit) can start/stop/loop the client.
 *
 * Audio is discarded by the player listener; the primary purpose is to make
 * the device discoverable as a Sendspin receiver.  Route audio here later by
 * replacing DiscardPlayer::on_audio_write with real I2S / BT output.
 */

#include "sendspin_client.h"

extern "C" {
#include "esp_mac.h"
#include "esp_log.h"
#include "wifi_manager.h"
}

#include "sendspin/client.h"
#include "sendspin/player_role.h"

using namespace sendspin;

static const char *TAG = "sendspin";

/* ── Listener: discard incoming audio frames ─────────────────────────── */
struct DiscardPlayer : PlayerRoleListener {
    size_t on_audio_write(uint8_t *data, size_t length, uint32_t /*timeout_ms*/) override {
        (void)data;
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
static bool             s_running         = false;

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
    cfg.client_id       = client_id;
    cfg.name            = "Picture Frame";
    cfg.product_name    = "ESP32 Picture Frame";
    cfg.manufacturer    = "TriggerCMD";
    cfg.software_version = "2.0.278";

    s_player_listener = new DiscardPlayer();
    s_net_provider    = new WifiProvider();
    s_client          = new SendspinClient(std::move(cfg));

    PlayerRoleConfig pcfg;
    pcfg.audio_formats = {
        {SendspinCodecFormat::PCM, 2, 44100, 16},
        {SendspinCodecFormat::PCM, 2, 48000, 16},
    };
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

    s_running = true;
    ESP_LOGI(TAG, "started (id=%s)", client_id);
}

extern "C" void sendspin_client_stop(void)
{
    if (!s_running || !s_client) return;

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

extern "C" void sendspin_client_loop(void)
{
    if (s_running && s_client) {
        s_client->loop();
    }
}
