/*
 * http_pf_config.c
 *
 * Tiny HTTP server (port 80) that lets the user enter their TRIGGERcmd
 * hardware JWT via a browser form.  Modelled on http_config.c — same
 * dark-card styling (#0f1117 background, #4f46e5 button, system-ui font).
 *
 * Routes:
 *   GET  /             — TRIGGERcmd Setup form
 *   POST /provision    — save hw_token (+ optional computer_id) to NVS; reboot
 *   POST /reprovision  — erase computer_id from NVS; reboot
 */

#include "http_pf_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define TAG          "http_pf_cfg"
#define NVS_NS       "pf_cfg"
#define NVS_KEY_TOK  "hw_token"
#define NVS_KEY_CID  "computer_id"
#define MAX_TOKEN    513    /* 512 + NUL */
#define MAX_CID      33     /* 32 + NUL  */
#define BODY_CAP     600    /* form body is small */

/* ── Request body helper ─────────────────────────────────────────────────── */

static char *read_body(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len >= BODY_CAP) return NULL;
    char *buf = calloc(len + 1, 1);
    if (!buf) return NULL;
    int got = 0, rem = len;
    while (rem > 0) {
        int n = httpd_req_recv(req, buf + got, rem);
        if (n <= 0) { free(buf); return NULL; }
        got += n; rem -= n;
    }
    return buf;
}

/* Minimal percent-decode: decodes %XX and turns + into space, in-place. */
static void url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static int form_get_value(const char *body, const char *key,
                          char *out, size_t out_max)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i < out_max - 1)
                out[i++] = *p++;
            out[i] = '\0';
            url_decode(out);
            return 1;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return 0;
}

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
    "h1{font-size:1.4rem;color:#a5b4fc;margin:0 0 .3rem}"
    "p{color:#94a3b8;font-size:.9rem;margin:0 0 1.5rem}"
    "label{display:block;font-size:.85rem;color:#94a3b8;margin-bottom:.3rem}"
    "input[type=password],input[type=text]{"
        "width:100%;padding:.6rem .8rem;background:#0f1117;color:#e2e8f0;"
        "border:1px solid #2d3148;border-radius:.5rem;font-size:.95rem;"
        "box-sizing:border-box;margin-bottom:1rem}"
    "button{width:100%;padding:.75rem;background:#4f46e5;color:#fff;border:none;"
           "border-radius:.6rem;font-size:1rem;font-weight:600;cursor:pointer}"
    "button:hover{background:#4338ca}"
    ".ok{color:#86efac;margin-top:1rem;text-align:center}"
    ".err{color:#fca5a5;margin-top:1rem;text-align:center}"
    ".show-row{display:flex;align-items:center;gap:.5rem;margin-bottom:1rem}"
    ".show-row input[type=password],.show-row input[type=text]"
        "{flex:1;margin-bottom:0}"
    ".show-row label{margin:0;cursor:pointer;font-size:.8rem;white-space:nowrap}"
    "</style></head><body><div class='card'>"
    "<h1>TRIGGERcmd Setup</h1>"
    "<p>Enter your TRIGGERcmd hardware token to connect this display.</p>";

static const char s_form[] =
    "<form method='POST' action='/provision'>"
    "<label for='tok'>Hardware token</label>"
    "<div class='show-row'>"
    "<input type='password' id='tok' name='token' required maxlength='511'"
           " placeholder='paste token here'>"
    "<label><input type='checkbox' "
        "onchange=\"document.getElementById('tok').type=this.checked?'text':'password'\""
    "> show</label>"
    "</div>"
    "<label for='cid'>Computer ID <span style='color:#64748b'>(optional — leave blank to auto-assign)</span></label>"
    "<input type='text' id='cid' name='computer_id' maxlength='32'"
           " placeholder='leave blank to auto-assign'>"
    "<button type='submit'>Save &amp; Reboot</button>"
    "</form>";

static const char s_tail[] = "</div></body></html>";
static const char s_ok[]   = "<p class='ok'>&#10003; Token saved. Rebooting&hellip;</p>";
static const char s_err_empty[] = "<p class='err'>&#10007; Token must not be empty.</p>";
static const char s_err_nvs[]   = "<p class='err'>&#10007; Storage error.</p>";

/* ── Handlers ────────────────────────────────────────────────────────────── */

static esp_err_t get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, s_html_head);
    httpd_resp_sendstr_chunk(req, s_form);
    httpd_resp_sendstr_chunk(req, s_tail);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t post_provision_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_OK;
    }

    char token[MAX_TOKEN]  = {0};
    char cid[MAX_CID]      = {0};
    form_get_value(body, "token",       token, sizeof(token));
    form_get_value(body, "computer_id", cid,   sizeof(cid));
    free(body);

    if (token[0] == '\0') {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr_chunk(req, s_html_head);
        httpd_resp_sendstr_chunk(req, s_err_empty);
        httpd_resp_sendstr_chunk(req, s_form);
        httpd_resp_sendstr_chunk(req, s_tail);
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr_chunk(req, s_html_head);
        httpd_resp_sendstr_chunk(req, s_err_nvs);
        httpd_resp_sendstr_chunk(req, s_form);
        httpd_resp_sendstr_chunk(req, s_tail);
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    nvs_set_str(h, NVS_KEY_TOK, token);
    if (cid[0] != '\0') {
        nvs_set_str(h, NVS_KEY_CID, cid);
    }
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "HW token saved; rebooting");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, s_html_head);
    httpd_resp_sendstr_chunk(req, s_ok);
    httpd_resp_sendstr_chunk(req, s_tail);
    httpd_resp_sendstr_chunk(req, NULL);

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;   /* unreachable */
}

static esp_err_t post_reprovision_handler(httpd_req_t *req)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_CID);
        nvs_commit(h);
        nvs_close(h);
    }

    static const char *msg =
        "<p class='ok'>&#10003; computer_id cleared. Rebooting&hellip;</p>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, s_html_head);
    httpd_resp_sendstr_chunk(req, msg);
    httpd_resp_sendstr_chunk(req, s_tail);
    httpd_resp_sendstr_chunk(req, NULL);

    ESP_LOGI(TAG, "Re-provision requested; rebooting");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;   /* unreachable */
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t http_pf_config_start(void)
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 3;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t u_get  = { "/",            HTTP_GET,  get_handler,              NULL };
    static const httpd_uri_t u_prov = { "/provision",   HTTP_POST, post_provision_handler,   NULL };
    static const httpd_uri_t u_repr = { "/reprovision", HTTP_POST, post_reprovision_handler, NULL };

    httpd_register_uri_handler(server, &u_get);
    httpd_register_uri_handler(server, &u_prov);
    httpd_register_uri_handler(server, &u_repr);

    ESP_LOGI(TAG, "TRIGGERcmd config server started on port 80");
    return ESP_OK;
}
