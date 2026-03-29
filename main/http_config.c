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
#define NVS_KEY_PKEY "pubkey"
#define MAX_PASS_LEN 64
#define BODY_CAP     1024

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Read full request body into a heap buffer (caller must free). */
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

static int valid_pubkey_line(const char *s)
{
    if (strncmp(s, "ssh-", 4) != 0 && strncmp(s, "ecdsa-", 6) != 0)
        return 0;
    const char *sp = strchr(s, ' ');
    if (!sp || sp == s || !*(sp + 1)) return 0;
    char c = *(sp + 1);
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/* ── Static HTML ─────────────────────────────────────────────────────────── */

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

/* Password change form – posts to /password */
static const char s_pass_form[] =
    "<h2>SSH Password</h2>"
    "<form method='POST' action='/password'>"
    "<label for='p1'>New password</label>"
    "<div class='row'>"
    "<input type='password' id='p1' name='pass1' required minlength='4' maxlength='63'>"
    "<label><input type='checkbox' "
        "onchange=\"document.getElementById('p1').type=this.checked?'text':'password'\"> show</label>"
    "</div>"
    "<label for='p2'>Confirm password</label>"
    "<div class='row'>"
    "<input type='password' id='p2' name='pass2' required minlength='4' maxlength='63'>"
    "<label><input type='checkbox' "
        "onchange=\"document.getElementById('p2').type=this.checked?'text':'password'\"> show</label>"
    "</div>"
    "<button type='submit'>Save Password</button>"
    "</form>";

/* Public-key form – posts to /pubkey */
static const char s_key_form[] =
    "<h2>SSH Public Key <span style='font-weight:400;color:#64748b'>(optional)</span></h2>"
    "<form method='POST' action='/pubkey'>"
    "<label for='pk'>Authorized key</label>"
    "<textarea id='pk' name='pubkey' spellcheck='false'"
        " placeholder='ssh-ed25519 AAAAC3... user@host'></textarea>"
    "<p class='hint'>Paste one line in authorized_keys format (ssh-ed25519, "
    "ecdsa-sha2-nistp256, ssh-rsa). Leave empty to clear the stored key.</p>"
    "<button type='submit'>Save Public Key</button>"
    "</form>";

static const char s_tail[]         = "</div></body></html>";
static const char s_ok_pass[]      = "<p class='ok'>&#10003; Password saved.</p>";
static const char s_ok_key[]       = "<p class='ok'>&#10003; Public key saved &#8211; pubkey auth is now active.</p>";
static const char s_ok_cleared[]   = "<p class='ok'>&#10003; Public key cleared.</p>";
static const char s_err_mismatch[] = "<p class='err'>&#10007; Passwords do not match.</p>";
static const char s_err_short[]    = "<p class='err'>&#10007; Password too short (minimum 4 characters).</p>";
static const char s_err_key[]      = "<p class='err'>&#10007; Invalid key &#8211; expected authorized_keys format.</p>";
static const char s_err_nvs[]      = "<p class='err'>&#10007; Save failed (storage error).</p>";

/* ── Page helper ─────────────────────────────────────────────────────────── */

static void send_page(httpd_req_t *req,
                      const char  *pass_msg,
                      const char  *key_msg)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, s_html_head);
    if (pass_msg) httpd_resp_sendstr_chunk(req, pass_msg);
    httpd_resp_sendstr_chunk(req, s_pass_form);
    if (key_msg)  httpd_resp_sendstr_chunk(req, key_msg);
    httpd_resp_sendstr_chunk(req, s_key_form);
    httpd_resp_sendstr_chunk(req, s_tail);
    httpd_resp_sendstr_chunk(req, NULL);
}

/* ── Handlers ────────────────────────────────────────────────────────────── */

static esp_err_t get_handler(httpd_req_t *req)
{
    send_page(req, NULL, NULL);
    return ESP_OK;
}

static esp_err_t post_password_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_OK;
    }

    char pass1[MAX_PASS_LEN] = {0};
    char pass2[MAX_PASS_LEN] = {0};
    form_get_value(body, "pass1", pass1, sizeof(pass1));
    form_get_value(body, "pass2", pass2, sizeof(pass2));
    free(body);

    const char *msg;
    if (strlen(pass1) < 4) {
        msg = s_err_short;
    } else if (strcmp(pass1, pass2) != 0) {
        msg = s_err_mismatch;
    } else {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, NVS_KEY_PASS, pass1);
            nvs_commit(h);
            nvs_close(h);
            ESP_LOGI(TAG, "SSH password updated");
            msg = s_ok_pass;
        } else {
            msg = s_err_nvs;
        }
    }
    send_page(req, msg, NULL);
    return ESP_OK;
}

static esp_err_t post_pubkey_handler(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_OK;
    }

    /* pubkey value may be up to ~800 chars after URL-decode */
    char *pubkey = calloc(BODY_CAP, 1);
    if (!pubkey) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_OK;
    }
    form_get_value(body, "pubkey", pubkey, BODY_CAP);
    free(body);

    /* Trim trailing whitespace / newlines */
    int len = (int)strlen(pubkey);
    while (len > 0 && (pubkey[len-1] == '\r' || pubkey[len-1] == '\n' ||
                       pubkey[len-1] == ' '  || pubkey[len-1] == '\t'))
        pubkey[--len] = '\0';

    const char *msg;
    if (len == 0) {
        /* Empty \u2192 clear stored key */
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, NVS_KEY_PKEY);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "SSH public key cleared");
        msg = s_ok_cleared;
    } else if (!valid_pubkey_line(pubkey)) {
        msg = s_err_key;
    } else {
        /* Strip trailing comment (third space-delimited field) to save NVS space */
        int spaces = 0;
        for (int i = 0; pubkey[i]; i++) {
            if (pubkey[i] == ' ' && ++spaces == 2) {
                pubkey[i] = '\0';
                break;
            }
        }
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, NVS_KEY_PKEY, pubkey);
            nvs_commit(h);
            nvs_close(h);
            ESP_LOGI(TAG, "SSH public key updated");
            msg = s_ok_key;
        } else {
            msg = s_err_nvs;
        }
    }
    free(pubkey);
    send_page(req, NULL, msg);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t http_config_start(void)
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 3; /* stay within LWIP_MAX_SOCKETS budget */

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t u_get  = { "/",         HTTP_GET,  get_handler,           NULL };
    static const httpd_uri_t u_pass = { "/password", HTTP_POST, post_password_handler, NULL };
    static const httpd_uri_t u_pkey = { "/pubkey",   HTTP_POST, post_pubkey_handler,   NULL };

    httpd_register_uri_handler(server, &u_get);
    httpd_register_uri_handler(server, &u_pass);
    httpd_register_uri_handler(server, &u_pkey);

    ESP_LOGI(TAG, "HTTP config server started on port 80");
    return ESP_OK;
}
