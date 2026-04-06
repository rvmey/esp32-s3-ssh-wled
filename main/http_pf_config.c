/*
 * http_pf_config.c
 *
 * Tiny HTTP server (port 80) that shows the TRIGGERcmd pair code while the
 * firmware awaits pairing.  Modelled on http_config.c — same dark-card
 * styling (#0f1117 background, #4f46e5 accent, system-ui font).
 *
 * Routes:
 *   GET  /             — Pairing info page with pair code + numbered instructions
 *   POST /reprovision  — erase computer_id from NVS; reboot
 */

#include "http_pf_config.h"

#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define TAG         "http_pf_cfg"
#define NVS_NS      "pf_cfg"
#define NVS_KEY_CID "computer_id"

/* Pair code set by http_pf_config_start(); read by the GET / handler */
static char            s_pair_code[8] = "-----";
static httpd_handle_t  s_server       = NULL;

/* ── Static HTML ─────────────────────────────────────────────────────────── */

static const char s_html_head[] =
    "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>TRIGGERcmd Setup</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;background:#0f1117;color:#e2e8f0;"
         "min-height:100vh;display:flex;align-items:center;justify-content:center;"
         "padding:1.5rem}"
    ".card{background:#1e2130;border:1px solid #2d3148;border-radius:1rem;"
          "padding:2.5rem 2rem;max-width:420px;width:100%;"
          "box-shadow:0 8px 32px rgba(0,0,0,.4)}"
    "h1{font-size:1.4rem;color:#a5b4fc;margin:0 0 .5rem}"
    "p{color:#94a3b8;font-size:.9rem;margin:0 0 1rem}"
    "ol{color:#94a3b8;font-size:.9rem;margin:0 0 1rem;padding-left:1.4rem}"
    "ol li{margin-bottom:.35rem}"
    "strong{color:#e2e8f0}"
    ".code{font-family:monospace;font-size:2.5rem;font-weight:700;"
          "letter-spacing:.3em;color:#a5b4fc;text-align:center;"
          "background:#0f1117;border:2px solid #4f46e5;border-radius:.6rem;"
          "padding:.75rem 1.5rem;margin:1.25rem 0}"
    ".note{color:#64748b;font-size:.8rem;text-align:center;margin-bottom:1.5rem}"
    "hr{border:none;border-top:1px solid #2d3148;margin:1.5rem 0}"
    "button{width:100%;padding:.75rem;background:#4f46e5;color:#fff;border:none;"
           "border-radius:.6rem;font-size:1rem;font-weight:600;cursor:pointer}"
    "button:hover{background:#4338ca}"
    ".ok{color:#86efac;margin-top:1rem;text-align:center}"
    "</style></head><body><div class='card'>"
    "<h1>TRIGGERcmd Setup</h1>"
    "<p>Your picture frame is ready to pair. "
       "To connect it to your TRIGGERcmd account:</p>"
    "<ol>"
    "<li>Go to <strong>www.triggercmd.com</strong> and sign in.</li>"
    "<li>Click your name in the upper right corner.</li>"
    "<li>Click <strong>Pair</strong>.</li>"
    "<li>Enter the pair code shown below.</li>"
    "</ol>";

static const char s_tail[] = "</div></body></html>";

/* ── Handlers ────────────────────────────────────────────────────────────── */

static esp_err_t get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, s_html_head);

    char code_block[64];
    snprintf(code_block, sizeof(code_block),
             "<div class='code'>%s</div>", s_pair_code);
    httpd_resp_sendstr_chunk(req, code_block);

    httpd_resp_sendstr_chunk(req,
        "<p class='note'>This code expires in 10 minutes. "
        "The screen and this page will show a new code automatically "
        "if it expires.</p>"
        "<hr>"
        "<form method='POST' action='/reprovision'>"
        "<button type='submit'>"
            "Re-provision (reassign to a different account)"
        "</button>"
        "</form>");
    httpd_resp_sendstr_chunk(req, s_tail);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t post_reprovision_handler(httpd_req_t *req)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_CID);
        nvs_commit(h);
        nvs_close(h);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, s_html_head);
    httpd_resp_sendstr_chunk(req,
        "<p class='ok'>&#10003; Re-provisioning. Rebooting&hellip;</p>");
    httpd_resp_sendstr_chunk(req, s_tail);
    httpd_resp_sendstr_chunk(req, NULL);

    ESP_LOGI(TAG, "Re-provision requested; rebooting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;   /* unreachable */
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t http_pf_config_start(const char *pair_code)
{
    strncpy(s_pair_code, pair_code ? pair_code : "-----",
            sizeof(s_pair_code) - 1);
    s_pair_code[sizeof(s_pair_code) - 1] = '\0';

    if (s_server) {
        /* Server already running — updated code is served on next GET / */
        ESP_LOGI(TAG, "Pair code updated to %s", s_pair_code);
        return ESP_OK;
    }

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 3;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t u_get  = { "/",            HTTP_GET,  get_handler,              NULL };
    static const httpd_uri_t u_repr = { "/reprovision", HTTP_POST, post_reprovision_handler, NULL };

    httpd_register_uri_handler(s_server, &u_get);
    httpd_register_uri_handler(s_server, &u_repr);

    ESP_LOGI(TAG, "Pairing info server started on port 80 (code=%s)", s_pair_code);
    return ESP_OK;
}

void http_pf_config_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Pairing info server stopped");
    }
}
