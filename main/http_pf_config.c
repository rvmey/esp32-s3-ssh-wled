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
#include "wifi_manager.h"

#define TAG         "http_pf_cfg"
#define TAG_PF_WIFI "http_pf_cfg"
#define NVS_NS      "pf_cfg"
#define NVS_KEY_CID "computer_id"
#define NVS_KEY_STT "stt_key"

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

static const char s_wifi_form_head[] =
    "<hr>"
    "<h2 style='font-size:1.1rem;color:#a5b4fc;margin:0 0 .75rem'>Secondary Wi-Fi Networks "
    "<span style='font-weight:400;color:#64748b;font-size:.9rem'>(optional)</span></h2>"
    "<form method='POST' action='/wifi'>"
    "<p style='color:#94a3b8;font-size:.85rem;margin:0 0 .75rem'>"
    "Alternate networks tried when the primary is unavailable.</p>"
    "<label style='display:block;color:#94a3b8;font-size:.85rem;margin:.5rem 0 .2rem'>SSID 2</label>"
    "<input style='width:100%;box-sizing:border-box;padding:.5rem;background:#0f1117;"
           "border:1px solid #4f46e5;border-radius:.4rem;color:#e2e8f0;font-size:.95rem;"
           "margin-bottom:.5rem' type='text' name='ssid2' value='";
static const char s_wifi_form_mid1[] =
    "'>"
    "<label style='display:block;color:#94a3b8;font-size:.85rem;margin:.25rem 0 .2rem'>Password 2</label>"
    "<input style='width:100%;box-sizing:border-box;padding:.5rem;background:#0f1117;"
           "border:1px solid #4f46e5;border-radius:.4rem;color:#e2e8f0;font-size:.95rem;"
           "margin-bottom:.5rem' type='password' name='pass2'>"
    "<label style='display:block;color:#94a3b8;font-size:.85rem;margin:.25rem 0 .2rem'>SSID 3</label>"
    "<input style='width:100%;box-sizing:border-box;padding:.5rem;background:#0f1117;"
           "border:1px solid #4f46e5;border-radius:.4rem;color:#e2e8f0;font-size:.95rem;"
           "margin-bottom:.5rem' type='text' name='ssid3' value='";
static const char s_wifi_form_mid2[] =
    "'>"
    "<label style='display:block;color:#94a3b8;font-size:.85rem;margin:.25rem 0 .2rem'>Password 3</label>"
    "<input style='width:100%;box-sizing:border-box;padding:.5rem;background:#0f1117;"
           "border:1px solid #4f46e5;border-radius:.4rem;color:#e2e8f0;font-size:.95rem;"
           "margin-bottom:.75rem' type='password' name='pass3'>"
    "<p style='color:#64748b;font-size:.8rem;margin:0 0 .75rem'>"
    "Leave a password blank to keep the current one. Clear an SSID to remove that network.</p>"
    "<button style='width:100%;padding:.65rem;background:#4f46e5;color:#fff;border:none;"
            "border-radius:.6rem;font-size:.95rem;font-weight:600;cursor:pointer'>"
    "Save Networks</button>"
    "</form>";

static const char s_ok_wifi[] =
    "<p style='color:#86efac;margin-top:.75rem;text-align:center'>&#10003; Networks saved.</p>";

static const char s_stt_form[] =
    "<hr>"
    "<h2 style='font-size:1.1rem;color:#a5b4fc;margin:0 0 .5rem'>AI Features (OpenAI)</h2>"
    "<p style='color:#94a3b8;font-size:.85rem;margin:0 0 .75rem'>"
    "Press the side button to record a voice command, or use the "
    "'askpic' command to ask a question about the displayed picture. "
    "An OpenAI API key is required for these features.</p>"
    "<form method='POST' action='/stt_key'>"
    "<label style='display:block;color:#94a3b8;font-size:.85rem;margin:.5rem 0 .2rem'>"
    "OpenAI API Key</label>"
    "<input style='width:100%;box-sizing:border-box;padding:.5rem;background:#0f1117;"
           "border:1px solid #4f46e5;border-radius:.4rem;color:#e2e8f0;font-size:.95rem;"
           "margin-bottom:.75rem' type='password' name='stt_key' placeholder='sk-...'>"
    "<button style='width:100%;padding:.65rem;background:#4f46e5;color:#fff;border:none;"
            "border-radius:.6rem;font-size:.95rem;font-weight:600;cursor:pointer'>"
    "Save API Key</button>"
    "</form>";

static const char s_ok_stt[] =
    "<p style='color:#86efac;margin-top:.75rem;text-align:center'>&#10003; API key saved.</p>";

/* ── SIP speakerphone config form ────────────────────────────────────────── */
#define SIP_INPUT_STYLE \
    "style='width:100%;box-sizing:border-box;padding:.5rem;background:#0f1117;" \
    "border:1px solid #4f46e5;border-radius:.4rem;color:#e2e8f0;font-size:.95rem;" \
    "margin-bottom:.5rem'"
#define SIP_LABEL_STYLE \
    "style='display:block;color:#94a3b8;font-size:.85rem;margin:.25rem 0 .2rem'"

static const char s_sip_form_head[] =
    "<hr>"
    "<h2 style='font-size:1.1rem;color:#a5b4fc;margin:0 0 .5rem'>SIP Speakerphone "
    "<span style='font-weight:400;color:#64748b;font-size:.9rem'>(Core2)</span></h2>"
    "<p style='color:#94a3b8;font-size:.85rem;margin:0 0 .75rem'>"
    "Register to an Asterisk/FreePBX server to make and receive calls. "
    "Half-duplex: tap the screen to switch between talk and listen during a call.</p>"
    "<form method='POST' action='/sip'>"
    "<label " SIP_LABEL_STYLE ">Server (host)</label>"
    "<input " SIP_INPUT_STYLE " type='text' name='sip_srv' placeholder='pbx.example.com' value='";
static const char s_sip_form_1[] =
    "'>"
    "<label " SIP_LABEL_STYLE ">Port (default 5061, TLS)</label>"
    "<input " SIP_INPUT_STYLE " type='text' name='sip_port' placeholder='5061' value='";
static const char s_sip_form_2[] =
    "'>"
    "<label " SIP_LABEL_STYLE ">Username / Extension</label>"
    "<input " SIP_INPUT_STYLE " type='text' name='sip_user' placeholder='1001' value='";
static const char s_sip_form_3[] =
    "'>"
    "<label " SIP_LABEL_STYLE ">Domain (optional, defaults to server)</label>"
    "<input " SIP_INPUT_STYLE " type='text' name='sip_dom' value='";
static const char s_sip_form_4[] =
    "'>"
    "<label " SIP_LABEL_STYLE ">Password</label>"
    "<input " SIP_INPUT_STYLE " type='password' name='sip_pass' placeholder='(unchanged)'>"
    "<p style='color:#64748b;font-size:.8rem;margin:0 0 .75rem'>"
    "Leave the password blank to keep the current one. Clear the server to disable SIP. "
    "Reboots after saving.</p>"
    "<button style='width:100%;padding:.65rem;background:#4f46e5;color:#fff;border:none;"
            "border-radius:.6rem;font-size:.95rem;font-weight:600;cursor:pointer'>"
    "Save SIP Settings</button>"
    "</form>";

static const char s_tail[] = "</div></body></html>";

static void sip_nvs_get(const char *key, char *out, size_t max)
{
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t l = max;
        nvs_get_str(h, key, out, &l);
        nvs_close(h);
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void url_decode_pf(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' '; r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void form_get_pf(const char *body, const char *key, char *out, size_t max)
{
    size_t klen = strlen(key);
    const char *p = body;
    out[0] = '\0';
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i < max - 1) out[i++] = *p++;
            out[i] = '\0';
            url_decode_pf(out);
            return;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
}

/* HTML-escape a string into chunked response output. */
static void send_escaped_pf(httpd_req_t *req, const char *s)
{
    char buf[64]; int pos = 0;
    while (*s) {
        const char *ent = NULL;
        if      (*s == '<') ent = "&lt;";
        else if (*s == '>') ent = "&gt;";
        else if (*s == '&') ent = "&amp;";
        else if (*s == '"') ent = "&quot;";
        if (ent) {
            if (pos) { buf[pos] = '\0'; httpd_resp_sendstr_chunk(req, buf); pos = 0; }
            httpd_resp_sendstr_chunk(req, ent);
        } else {
            buf[pos++] = *s;
            if (pos >= (int)sizeof(buf) - 1) { buf[pos] = '\0'; httpd_resp_sendstr_chunk(req, buf); pos = 0; }
        }
        s++;
    }
    if (pos) { buf[pos] = '\0'; httpd_resp_sendstr_chunk(req, buf); }
}

static void send_wifi_section_pf(httpd_req_t *req, const char *wifi_msg)
{
    char ssid2[33] = {0};
    char ssid3[33] = {0};
    wifi_get_ssid2(ssid2, sizeof(ssid2));
    wifi_get_ssid3(ssid3, sizeof(ssid3));
    if (wifi_msg) httpd_resp_sendstr_chunk(req, wifi_msg);
    httpd_resp_sendstr_chunk(req, s_wifi_form_head);
    send_escaped_pf(req, ssid2);
    httpd_resp_sendstr_chunk(req, s_wifi_form_mid1);
    send_escaped_pf(req, ssid3);
    httpd_resp_sendstr_chunk(req, s_wifi_form_mid2);
}

static void send_sip_section_pf(httpd_req_t *req, const char *sip_msg)
{
    char srv[64] = {0}, port[8] = {0}, user[48] = {0}, dom[64] = {0};
    sip_nvs_get("sip_srv", srv, sizeof(srv));
    sip_nvs_get("sip_port", port, sizeof(port));
    sip_nvs_get("sip_user", user, sizeof(user));
    sip_nvs_get("sip_dom", dom, sizeof(dom));
    if (sip_msg) httpd_resp_sendstr_chunk(req, sip_msg);
    httpd_resp_sendstr_chunk(req, s_sip_form_head);
    send_escaped_pf(req, srv);
    httpd_resp_sendstr_chunk(req, s_sip_form_1);
    send_escaped_pf(req, port);
    httpd_resp_sendstr_chunk(req, s_sip_form_2);
    send_escaped_pf(req, user);
    httpd_resp_sendstr_chunk(req, s_sip_form_3);
    send_escaped_pf(req, dom);
    httpd_resp_sendstr_chunk(req, s_sip_form_4);
}

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
    httpd_resp_sendstr_chunk(req, s_stt_form);
    send_sip_section_pf(req, NULL);
    send_wifi_section_pf(req, NULL);
    httpd_resp_sendstr_chunk(req, s_tail);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t post_sip_handler(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len >= 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_OK;
    }
    char *body = calloc(len + 1, 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL); return ESP_OK; }
    int got = 0, rem = len;
    while (rem > 0) {
        int n = httpd_req_recv(req, body + got, rem);
        if (n <= 0) { free(body); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL); return ESP_OK; }
        got += n; rem -= n;
    }

    char srv[64] = {0}, port[8] = {0}, user[48] = {0}, dom[64] = {0}, pass[64] = {0};
    form_get_pf(body, "sip_srv", srv, sizeof(srv));
    form_get_pf(body, "sip_port", port, sizeof(port));
    form_get_pf(body, "sip_user", user, sizeof(user));
    form_get_pf(body, "sip_dom", dom, sizeof(dom));
    form_get_pf(body, "sip_pass", pass, sizeof(pass));
    free(body);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "sip_srv", srv);
        nvs_set_str(h, "sip_port", port);
        nvs_set_str(h, "sip_user", user);
        nvs_set_str(h, "sip_dom", dom);
        if (pass[0]) nvs_set_str(h, "sip_pass", pass);   /* blank = keep existing */
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "SIP settings saved (srv='%s' user='%s')", srv, user);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, s_html_head);
    httpd_resp_sendstr_chunk(req,
        "<p class='ok'>&#10003; SIP settings saved. Rebooting&hellip;</p>");
    httpd_resp_sendstr_chunk(req, s_tail);
    httpd_resp_sendstr_chunk(req, NULL);

    ESP_LOGI(TAG, "SIP settings changed; rebooting to apply");
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

static esp_err_t post_wifi_handler(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len >= 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_OK;
    }
    char *body = calloc(len + 1, 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL); return ESP_OK; }
    int got = 0, rem = len;
    while (rem > 0) { int n = httpd_req_recv(req, body + got, rem); if (n <= 0) { free(body); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL); return ESP_OK; } got += n; rem -= n; }

    char ssid2[33] = {0}, pass2[65] = {0};
    char ssid3[33] = {0}, pass3[65] = {0};
    form_get_pf(body, "ssid2", ssid2, sizeof(ssid2));
    form_get_pf(body, "pass2", pass2, sizeof(pass2));
    form_get_pf(body, "ssid3", ssid3, sizeof(ssid3));
    form_get_pf(body, "pass3", pass3, sizeof(pass3));
    free(body);

    if (ssid2[0] && !pass2[0]) {
        nvs_handle_t h;
        if (nvs_open("wifi_cfg", NVS_READONLY, &h) == ESP_OK) { size_t l = sizeof(pass2); nvs_get_str(h, "password2", pass2, &l); nvs_close(h); }
    }
    if (ssid3[0] && !pass3[0]) {
        nvs_handle_t h;
        if (nvs_open("wifi_cfg", NVS_READONLY, &h) == ESP_OK) { size_t l = sizeof(pass3); nvs_get_str(h, "password3", pass3, &l); nvs_close(h); }
    }

    wifi_save_credentials2(ssid2, pass2);
    wifi_save_credentials3(ssid3, pass3);
    ESP_LOGI(TAG_PF_WIFI, "Secondary WiFi networks updated (ssid2='%s' ssid3='%s')", ssid2, ssid3);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, s_html_head);
    char code_block[64];
    snprintf(code_block, sizeof(code_block), "<div class='code'>%s</div>", s_pair_code);
    httpd_resp_sendstr_chunk(req, code_block);
    httpd_resp_sendstr_chunk(req,
        "<p class='note'>This code expires in 10 minutes.</p>"
        "<hr>"
        "<form method='POST' action='/reprovision'>"
        "<button type='submit'>Re-provision (reassign to a different account)</button>"
        "</form>");
    send_wifi_section_pf(req, s_ok_wifi);
    httpd_resp_sendstr_chunk(req, s_tail);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t post_stt_key_handler(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len >= 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_OK;
    }
    char *body = calloc(len + 1, 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL); return ESP_OK; }
    int got = 0, rem = len;
    while (rem > 0) {
        int n = httpd_req_recv(req, body + got, rem);
        if (n <= 0) { free(body); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL); return ESP_OK; }
        got += n; rem -= n;
    }

    char key[256] = {0};
    form_get_pf(body, "stt_key", key, sizeof(key));
    free(body);

    if (key[0]) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, NVS_KEY_STT, key);
            nvs_commit(h);
            nvs_close(h);
            ESP_LOGI(TAG, "STT API key saved (%d chars)", (int)strlen(key));
        }
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, s_html_head);
    char code_block[64];
    snprintf(code_block, sizeof(code_block), "<div class='code'>%s</div>", s_pair_code);
    httpd_resp_sendstr_chunk(req, code_block);
    httpd_resp_sendstr_chunk(req,
        "<p class='note'>This code expires in 10 minutes.</p>"
        "<hr>"
        "<form method='POST' action='/reprovision'>"
        "<button type='submit'>Re-provision (reassign to a different account)</button>"
        "</form>");
    httpd_resp_sendstr_chunk(req, key[0] ? s_ok_stt : s_stt_form);
    if (key[0]) httpd_resp_sendstr_chunk(req, s_stt_form);
    send_wifi_section_pf(req, NULL);
    httpd_resp_sendstr_chunk(req, s_tail);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
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

    static const httpd_uri_t u_get  = { .uri = "/",            .method = HTTP_GET,  .handler = get_handler,              .user_ctx = NULL };
    static const httpd_uri_t u_repr = { .uri = "/reprovision", .method = HTTP_POST, .handler = post_reprovision_handler, .user_ctx = NULL };
    static const httpd_uri_t u_wifi = { .uri = "/wifi",        .method = HTTP_POST, .handler = post_wifi_handler,        .user_ctx = NULL };
    static const httpd_uri_t u_stt  = { .uri = "/stt_key",     .method = HTTP_POST, .handler = post_stt_key_handler,     .user_ctx = NULL };
    static const httpd_uri_t u_sip  = { .uri = "/sip",         .method = HTTP_POST, .handler = post_sip_handler,         .user_ctx = NULL };

    httpd_register_uri_handler(s_server, &u_get);
    httpd_register_uri_handler(s_server, &u_repr);
    httpd_register_uri_handler(s_server, &u_wifi);
    httpd_register_uri_handler(s_server, &u_stt);
    httpd_register_uri_handler(s_server, &u_sip);

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
