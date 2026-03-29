/*
 * http_config.c
 *
 * Tiny HTTP server (port 80) that serves a one-page setup form so the user
 * can change the SSH password from their browser right after provisioning.
 * The new password is saved to NVS under namespace "ssh_cfg", key "password".
 * ssh_server.c checks NVS before falling back to the Kconfig default.
 */

#include "http_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"

#define TAG          "http_cfg"
#define NVS_NS       "ssh_cfg"
#define NVS_KEY_PASS "password"
#define MAX_PASS_LEN 64

/* ── Helpers ─────────────────────────────────────────────────────────────── */

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

/* Extract value of key= from an application/x-www-form-urlencoded body. */
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
        /* skip to next key */
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return 0;
}

/* ── HTML page ───────────────────────────────────────────────────────────── */

static const char s_html_head[] =
    "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-S3 SSH Setup</title>"
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
    "<h1>SSH Setup</h1>"
    "<p>Set the password used to log in via SSH.<br>"
    "Username: <code>admin</code></p>";

static const char s_html_form[] =
    "<form method='POST' action='/'>"
    "<label for='p1'>New SSH password</label>"
    "<div class='show-row'>"
    "<input type='password' id='p1' name='pass1' required minlength='4' maxlength='63'>"
    "<label><input type='checkbox' onchange=\""
        "var t=document.getElementById('p1');"
        "t.type=this.checked?'text':'password'\"> show</label>"
    "</div>"
    "<label for='p2'>Confirm password</label>"
    "<div class='show-row'>"
    "<input type='password' id='p2' name='pass2' required minlength='4' maxlength='63'>"
    "<label><input type='checkbox' onchange=\""
        "var t=document.getElementById('p2');"
        "t.type=this.checked?'text':'password'\"> show</label>"
    "</div>"
    "<button type='submit'>Save password</button>"
    "</form>"
    "</div></body></html>";

static const char s_html_ok[] =
    "<form method='POST' action='/'>"
    "<label for='p1'>New SSH password</label>"
    "<div class='show-row'>"
    "<input type='password' id='p1' name='pass1' required minlength='4' maxlength='63'>"
    "<label><input type='checkbox' onchange=\""
        "var t=document.getElementById('p1');"
        "t.type=this.checked?'text':'password'\"> show</label>"
    "</div>"
    "<label for='p2'>Confirm password</label>"
    "<div class='show-row'>"
    "<input type='password' id='p2' name='pass2' required minlength='4' maxlength='63'>"
    "<label><input type='checkbox' onchange=\""
        "var t=document.getElementById('p2');"
        "t.type=this.checked?'text':'password'\"> show</label>"
    "</div>"
    "<button type='submit'>Save password</button>"
    "</form>"
    "<p class='ok'>&#10003; Password saved. Reconnect SSH to use the new password.</p>"
    "</div></body></html>";

static const char s_html_mismatch[] =
    "<form method='POST' action='/'>"
    "<label for='p1'>New SSH password</label>"
    "<div class='show-row'>"
    "<input type='password' id='p1' name='pass1' required minlength='4' maxlength='63'>"
    "<label><input type='checkbox' onchange=\""
        "var t=document.getElementById('p1');"
        "t.type=this.checked?'text':'password'\"> show</label>"
    "</div>"
    "<label for='p2'>Confirm password</label>"
    "<div class='show-row'>"
    "<input type='password' id='p2' name='pass2' required minlength='4' maxlength='63'>"
    "<label><input type='checkbox' onchange=\""
        "var t=document.getElementById('p2');"
        "t.type=this.checked?'text':'password'\"> show</label>"
    "</div>"
    "<button type='submit'>Save password</button>"
    "</form>"
    "<p class='err'>&#10007; Passwords do not match &ndash; try again.</p>"
    "</div></body></html>";

/* ── Handlers ────────────────────────────────────────────────────────────── */

static esp_err_t get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, s_html_head);
    httpd_resp_sendstr_chunk(req, s_html_form);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int  received  = 0;
    int  remaining = req->content_len;

    while (remaining > 0) {
        int n = httpd_req_recv(req, body + received,
                               sizeof(body) - received - 1);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return ESP_FAIL;
        }
        received  += n;
        remaining -= n;
    }
    body[received] = '\0';

    char pass1[MAX_PASS_LEN] = {0};
    char pass2[MAX_PASS_LEN] = {0};
    form_get_value(body, "pass1", pass1, sizeof(pass1));
    form_get_value(body, "pass2", pass2, sizeof(pass2));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, s_html_head);

    if (strlen(pass1) < 4 || strcmp(pass1, pass2) != 0) {
        httpd_resp_sendstr_chunk(req, s_html_mismatch);
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    /* Save to NVS */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_str(h, NVS_KEY_PASS, pass1);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "SSH password updated via web UI");
    } else {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
    }

    httpd_resp_sendstr_chunk(req, s_html_ok);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t http_config_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 3; /* stay within LWIP_MAX_SOCKETS budget */

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t get_uri = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = get_handler,
    };
    static const httpd_uri_t post_uri = {
        .uri     = "/",
        .method  = HTTP_POST,
        .handler = post_handler,
    };

    httpd_register_uri_handler(server, &get_uri);
    httpd_register_uri_handler(server, &post_uri);

    ESP_LOGI(TAG, "HTTP config server started on port 80");
    return ESP_OK;
}
