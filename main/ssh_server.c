/*
 * ssh_server.c
 *
 * wolfSSH-based SSH server for the ESP32-S3.
 * Provides an interactive shell to control the output device (RGB LED or
 * QSPI display) selected at build time via CONFIG_HARDWARE_DEVKITC /
 * CONFIG_HARDWARE_JC3248W535.
 *
 * Shell commands
 * --------------
 *   color <name>      Named colours: red green blue white yellow cyan
 *                                    magenta purple orange pink
 *   color R G B       RGB triplet, each 0-255
 *   color #RRGGBB     Hex colour string
 *   off               Turn the output off
 *   status            Print the current colour
 *   help              Print command reference
 *   exit | quit       Close the SSH session
 *
 * Host-key persistance
 * --------------------
 * On first boot an ECC P-256 key-pair is generated with wolfCrypt and stored
 * in NVS under namespace "ssh", key "hostkey".  All subsequent boots reuse
 * this key so the remote host-key fingerprint stays stable.
 */

#include "ssh_server.h"

#if CONFIG_HARDWARE_DEVKITC
#include "led_control.h"
#elif CONFIG_HARDWARE_JC3248W535
#include "screen_control.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* lwIP sockets */
#include "lwip/sockets.h"
#include "lwip/netdb.h"

/* FreeRTOS */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ESP-IDF */
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

/* wolfSSH */
#include "wolfssh/ssh.h"

/* wolfCrypt (for key generation) */
#include "wolfssl/wolfcrypt/ecc.h"
#include "wolfssl/wolfcrypt/random.h"
#include "wolfssl/wolfcrypt/asn.h"
#include "wolfssl/wolfcrypt/error-crypt.h"

/* sdkconfig-supplied credentials */
#include "sdkconfig.h"

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

static const char *TAG = "ssh_srv";

#define SSH_TASK_STACK   (12 * 1024)
#define SSH_TASK_PRIO    5
#define LINE_BUF_SZ      128
#define KEY_BUF_SZ       512
#define HIST_MAX         20
#define NVS_NS           "ssh"
#define NVS_KEY_HOSTKEY  "hostkey"
#define NVS_CFG_NS        "ssh_cfg"
#define NVS_CFG_KEY_PASS  "password"
#define NVS_CFG_KEY_PKEY  "pubkey"

/* ANSI helpers sent to the terminal */
#define CRLF    "\r\n"
#define PROMPT  "> "

/* ------------------------------------------------------------------ */
/* Banner text                                                         */
/* ------------------------------------------------------------------ */

#if CONFIG_HARDWARE_JC3248W535
static const char s_banner[] =
    "\r\n"
    "  +-----------------------------------------+\r\n"
    "  |  ESP32-S3 Screen Controller via SSH      |\r\n"
    "  |  Type 'help' for available commands.     |\r\n"
    "  +-----------------------------------------+\r\n"
    "\r\n";
#else
static const char s_banner[] =
    "\r\n"
    "  +-----------------------------------------+\r\n"
    "  |  ESP32-S3 RGB LED Controller via SSH     |\r\n"
    "  |  Type 'help' for available commands.     |\r\n"
    "  +-----------------------------------------+\r\n"
    "\r\n";
#endif

/* ------------------------------------------------------------------ */
/* wolfSSH global context                                              */
/* ------------------------------------------------------------------ */

static WOLFSSH_CTX *s_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Base-64 decode (used to parse authorized_keys public key blobs)     */
/* ------------------------------------------------------------------ */

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode a clean base64 string (standard alphabet, may have = padding).
 * Returns decoded byte count, or -1 on buffer overflow / bad input. */
static int b64_decode(const char *in, int n, uint8_t *out, int cap)
{
    int oi = 0, i = 0;
    while (i < n) {
        int a = (i   < n && in[i  ] != '=') ? b64val(in[i  ]) : -1;
        int b = (i+1 < n && in[i+1] != '=') ? b64val(in[i+1]) : -1;
        int c = (i+2 < n && in[i+2] != '=') ? b64val(in[i+2]) : -1;
        int d = (i+3 < n && in[i+3] != '=') ? b64val(in[i+3]) : -1;
        i += 4;
        if (a < 0 || b < 0) break;
        if (oi >= cap) return -1;
        out[oi++] = (uint8_t)((a << 2) | (b >> 4));
        if (c >= 0) { if (oi >= cap) return -1; out[oi++] = (uint8_t)(((b & 0xf) << 4) | (c >> 2)); }
        if (d >= 0) { if (oi >= cap) return -1; out[oi++] = (uint8_t)(((c & 0x3) << 6) |  d); }
    }
    return oi;
}

/* Returns 1 if the presented key matches the stored authorized key, else 0. */
static int check_pubkey_auth(WS_UserAuthData *authData)
{
    nvs_handle_t h;
    if (nvs_open(NVS_CFG_NS, NVS_READONLY, &h) != ESP_OK) return 0;

    char line[800] = {0};   /* "ssh-type base64blob" stored without comment */
    size_t len = sizeof(line);
    esp_err_t err = nvs_get_str(h, NVS_CFG_KEY_PKEY, line, &len);
    nvs_close(h);
    if (err != ESP_OK || line[0] == '\0') return 0;

    /* Skip the key-type field to reach the base64 blob */
    const char *p = line;
    while (*p && *p != ' ') p++;
    if (*p != ' ') return 0;
    p++;  /* skip space */

    /* Find end of base64 blob (stop at space, CR, LF, or NUL) */
    const char *b64_end = p;
    while (*b64_end && *b64_end != ' ' &&
           *b64_end != '\r' && *b64_end != '\n') b64_end++;
    int b64_len = (int)(b64_end - p);
    if (b64_len <= 0) return 0;

    /* Decode and compare against the key blob wolfSSH presented */
    uint8_t decoded[1024];
    int dec_len = b64_decode(p, b64_len, decoded, (int)sizeof(decoded));
    if (dec_len <= 0) return 0;
    if ((word32)dec_len != authData->sf.publicKey.publicKeySz) return 0;
    return memcmp(decoded, authData->sf.publicKey.publicKey, dec_len) == 0;
}

/* ------------------------------------------------------------------ */
/* User-auth callback                                                  */
/* ------------------------------------------------------------------ */

static int user_auth_cb(byte authType,
                        WS_UserAuthData *authData,
                        void *ctx)
{
    (void)ctx;

    const char *want_user = CONFIG_SSH_USERNAME;
    int user_ok = (authData->usernameSz == (word32)strlen(want_user)) &&
                  (memcmp(authData->username, want_user,
                          authData->usernameSz) == 0);
    if (!user_ok) {
        ESP_LOGW(TAG, "Auth FAILED: unknown user '%.*s'",
                 (int)authData->usernameSz, authData->username);
        return WOLFSSH_USERAUTH_FAILURE;
    }

    /* ── Public-key auth ──────────────────────────────────────────── */
    if (authType == WOLFSSH_USERAUTH_PUBLICKEY) {
        if (check_pubkey_auth(authData)) {
            ESP_LOGI(TAG, "Pubkey auth OK for '%s'", want_user);
            return WOLFSSH_USERAUTH_SUCCESS;
        }
        return WOLFSSH_USERAUTH_FAILURE;
    }

    /* ── Password auth ────────────────────────────────────────────── */
    if (authType == WOLFSSH_USERAUTH_PASSWORD) {
        /* Only accept an explicitly-set NVS password (no hardcoded fallback).
         * User must visit the web UI at http://<device-ip>/ to set one. */
        char stored_pass[65] = {0};
        nvs_handle_t nvs_h;
        int have_pass = 0;
        if (nvs_open(NVS_CFG_NS, NVS_READONLY, &nvs_h) == ESP_OK) {
            size_t len = sizeof(stored_pass);
            have_pass = (nvs_get_str(nvs_h, NVS_CFG_KEY_PASS, stored_pass, &len) == ESP_OK
                         && stored_pass[0] != '\0');
            nvs_close(nvs_h);
        }
        if (!have_pass) {
            ESP_LOGW(TAG, "Password auth denied: no password set (use web UI at http://<device-ip>/)");
            return WOLFSSH_USERAUTH_FAILURE;
        }
        int pass_ok =
            (authData->sf.password.passwordSz == (word32)strlen(stored_pass)) &&
            (memcmp(authData->sf.password.password, stored_pass,
                    authData->sf.password.passwordSz) == 0);
        if (pass_ok) {
            ESP_LOGI(TAG, "Password auth OK for '%s'", want_user);
            return WOLFSSH_USERAUTH_SUCCESS;
        }
    }

    ESP_LOGW(TAG, "Auth FAILED for user '%s'", want_user);
    return WOLFSSH_USERAUTH_FAILURE;
}

/* ------------------------------------------------------------------ */
/* Host-key: load from NVS or generate & persist                       */
/* ------------------------------------------------------------------ */

/**
 * Load the ECC host key DER blob from NVS.  Returns the number of bytes
 * loaded, or 0 on failure.
 */
static size_t load_hostkey_from_nvs(byte *buf, size_t buf_sz)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return 0;

    size_t sz = 0;
    err = nvs_get_blob(h, NVS_KEY_HOSTKEY, NULL, &sz);
    if (err != ESP_OK || sz == 0 || sz > buf_sz) {
        nvs_close(h);
        return 0;
    }

    err = nvs_get_blob(h, NVS_KEY_HOSTKEY, buf, &sz);
    nvs_close(h);
    return (err == ESP_OK) ? sz : 0;
}

/**
 * Generate a fresh ECC P-256 private key, export as DER, store in NVS, and
 * copy into buf.  Returns the DER size on success, 0 on failure.
 */
static size_t generate_and_store_hostkey(byte *buf, size_t buf_sz)
{
    WC_RNG    rng;
    ecc_key   key;
    int       ret;
    word32    der_sz = (word32)buf_sz;

    ESP_LOGI(TAG, "Generating ECC P-256 host key (first boot) ...");

    ret = wc_InitRng(&rng);
    if (ret != 0) {
        ESP_LOGE(TAG, "wc_InitRng: %d", ret);
        return 0;
    }

    ret = wc_ecc_init(&key);
    if (ret != 0) {
        wc_FreeRng(&rng);
        ESP_LOGE(TAG, "wc_ecc_init: %d", ret);
        return 0;
    }

    /* 32 bytes → P-256 */
    ret = wc_ecc_make_key(&rng, 32, &key);
    if (ret != 0) {
        wc_ecc_free(&key);
        wc_FreeRng(&rng);
        ESP_LOGE(TAG, "wc_ecc_make_key: %d", ret);
        return 0;
    }

    ret = wc_EccKeyToDer(&key, buf, der_sz);
    wc_ecc_free(&key);
    wc_FreeRng(&rng);

    if (ret <= 0) {
        ESP_LOGE(TAG, "wc_EccKeyToDer: %d", ret);
        return 0;
    }
    der_sz = (word32)ret;

    /* Persist to NVS */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_HOSTKEY, buf, der_sz);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Host key stored in NVS (%lu bytes)", (unsigned long)der_sz);
    }

    return (size_t)der_sz;
}

/* ------------------------------------------------------------------ */
/* Tiny line-editor                                                    */
/* Echoes characters, handles Backspace (0x7f / 0x08).                */
/* ------------------------------------------------------------------ */
/* Command history                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char entries[HIST_MAX][LINE_BUF_SZ];
    int  count;   /* number of valid entries, 0..HIST_MAX     */
    int  write;   /* circular write head                      */
} hist_t;

static void hist_add(hist_t *h, const char *line)
{
    if (!line || line[0] == '\0') return;
    /* Skip duplicate of most-recent entry */
    if (h->count > 0) {
        int last = (h->write - 1 + HIST_MAX) % HIST_MAX;
        if (strcmp(h->entries[last], line) == 0) return;
    }
    strncpy(h->entries[h->write], line, LINE_BUF_SZ - 1);
    h->entries[h->write][LINE_BUF_SZ - 1] = '\0';
    h->write = (h->write + 1) % HIST_MAX;
    if (h->count < HIST_MAX) h->count++;
}

/* browse=1 → newest entry, browse=2 → second newest, … */
static const char *hist_get(const hist_t *h, int browse)
{
    if (browse < 1 || browse > h->count) return NULL;
    int idx = (h->write - browse + HIST_MAX) % HIST_MAX;
    return h->entries[idx];
}

/* ------------------------------------------------------------------ */
/* Read one input line, with history navigation (↑/↓) and backspace.  */
/* Returns the finished line (without CR/LF), or NULL on disconnect.  */
/* ------------------------------------------------------------------ */

static byte read_byte(WOLFSSH *ssh)
{
    byte b;
    int  n;
    do {
        n = wolfSSH_stream_read(ssh, &b, 1);
        if (n == WS_WANT_READ) vTaskDelay(pdMS_TO_TICKS(10));
    } while (n == WS_WANT_READ);
    return (n == 1) ? b : 0xFF;
}

static char *read_line(WOLFSSH *ssh, char *buf, int buf_sz, hist_t *hist)
{
    int  pos         = 0;
    int  last_was_cr = 0;
    int  browse      = 0;          /* 0 = live input; 1..N = history depth */
    char saved[LINE_BUF_SZ];      /* preserves partially-typed line        */
    byte ch;

    buf[0]   = '\0';
    saved[0] = '\0';

    while (1) {
        int n = wolfSSH_stream_read(ssh, &ch, 1);

        if (n == WS_EOF || n == WS_CHANNEL_CLOSED || n == 0) return NULL;
        if (n < 0 && n != WS_WANT_READ) return NULL;
        if (n == WS_WANT_READ) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* ----- Enter (CR / LF / CR+LF) ----- */
        if (ch == '\n') {
            if (last_was_cr) { last_was_cr = 0; continue; }
            wolfSSH_stream_send(ssh, (byte *)CRLF, 2);
            buf[pos] = '\0';
            return buf;
        }
        if (ch == '\r') {
            last_was_cr = 1;
            wolfSSH_stream_send(ssh, (byte *)CRLF, 2);
            buf[pos] = '\0';
            return buf;
        }
        last_was_cr = 0;

        /* ----- ESC sequence (arrow keys etc.) ----- */
        if (ch == 0x1b) {
            byte b1 = read_byte(ssh);
            if (b1 != '[') continue;        /* not a CSI sequence — ignore */
            byte b2 = read_byte(ssh);

            if (b2 == 'A') {
                /* Up arrow — go back in history */
                if (browse == 0) {
                    buf[pos] = '\0';
                    strncpy(saved, buf, buf_sz - 1);
                    saved[buf_sz - 1] = '\0';
                }
                if (browse < hist->count) {
                    browse++;
                    const char *e = hist_get(hist, browse);
                    wolfSSH_stream_send(ssh, (byte *)"\r\x1b[K", 4);
                    wolfSSH_stream_send(ssh, (byte *)PROMPT, strlen(PROMPT));
                    pos = (int)strlen(e);
                    if (pos >= buf_sz) pos = buf_sz - 1;
                    memcpy(buf, e, pos);
                    buf[pos] = '\0';
                    wolfSSH_stream_send(ssh, (byte *)buf, (word32)pos);
                }
            } else if (b2 == 'B') {
                /* Down arrow — go forward in history */
                if (browse > 0) {
                    browse--;
                    const char *e = (browse == 0) ? saved : hist_get(hist, browse);
                    wolfSSH_stream_send(ssh, (byte *)"\r\x1b[K", 4);
                    wolfSSH_stream_send(ssh, (byte *)PROMPT, strlen(PROMPT));
                    pos = (int)strlen(e);
                    if (pos >= buf_sz) pos = buf_sz - 1;
                    memcpy(buf, e, pos);
                    buf[pos] = '\0';
                    wolfSSH_stream_send(ssh, (byte *)buf, (word32)pos);
                }
            }
            /* Ignore right/left/other sequences */
            continue;
        }

        /* ----- Backspace ----- */
        if (ch == 0x7f || ch == 0x08) {
            if (pos > 0) {
                pos--;
                byte erase[] = {0x08, 0x20, 0x08};
                wolfSSH_stream_send(ssh, erase, sizeof(erase));
            }
            continue;
        }

        /* ----- Ignore other non-printable control characters ----- */
        if (ch < 0x20) continue;

        /* ----- Buffer full — ring the bell ----- */
        if (pos >= buf_sz - 1) {
            byte bell = 0x07;
            wolfSSH_stream_send(ssh, &bell, 1);
            continue;
        }

        /* ----- Echo and store; any typing resets history browsing ----- */
        browse = 0;
        wolfSSH_stream_send(ssh, &ch, 1);
        buf[pos++] = (char)ch;
    }
}

/* ------------------------------------------------------------------ */
/* Command helpers                                                     */
/* ------------------------------------------------------------------ */

static void ssh_puts(WOLFSSH *ssh, const char *s)
{
    wolfSSH_stream_send(ssh, (byte *)s, (word32)strlen(s));
}

/* Skip leading whitespace */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Parse "R G B"  → returns 1 on success */
static int parse_rgb(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    int ri, gi, bi;
    if (sscanf(s, "%d %d %d", &ri, &gi, &bi) != 3) return 0;
    if (ri < 0 || ri > 255 || gi < 0 || gi > 255 || bi < 0 || bi > 255)
        return 0;
    *r = (uint8_t)ri;
    *g = (uint8_t)gi;
    *b = (uint8_t)bi;
    return 1;
}

/* Parse "#RRGGBB"  → returns 1 on success */
static int parse_hex(const char *s, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (*s == '#') s++;
    unsigned int hex;
    if (strlen(s) < 6) return 0;
    if (sscanf(s, "%6x", &hex) != 1) return 0;
    *r = (uint8_t)((hex >> 16) & 0xFF);
    *g = (uint8_t)((hex >>  8) & 0xFF);
    *b = (uint8_t)( hex        & 0xFF);
    return 1;
}

/* Map a colour name to RGB. Returns 1 on match. */
static int parse_name(const char *name, uint8_t *r, uint8_t *g, uint8_t *b)
{
    struct { const char *n; uint8_t r, g, b; } table[] = {
        { "red",     255,   0,   0 },
        { "green",     0, 255,   0 },
        { "blue",      0,   0, 255 },
        { "white",   255, 255, 255 },
        { "yellow",  255, 255,   0 },
        { "cyan",      0, 255, 255 },
        { "magenta", 255,   0, 255 },
        { "purple",  128,   0, 128 },
        { "orange",  255, 165,   0 },
        { "pink",    255, 105, 180 },
        { NULL, 0, 0, 0 }
    };
    for (int i = 0; table[i].n; i++) {
        if (strcasecmp(name, table[i].n) == 0) {
            *r = table[i].r;
            *g = table[i].g;
            *b = table[i].b;
            return 1;
        }
    }
    return 0;
}

#if CONFIG_HARDWARE_JC3248W535
static const char s_help[] =
    "Commands:\r\n"
    "  color <name>      Named colour: red green blue white yellow\r\n"
    "                    cyan magenta purple orange pink\r\n"
    "  color R G B       RGB triplet, each value 0-255\r\n"
    "  color #RRGGBB     Hex colour (e.g. #FF8800)\r\n"
    "  off               Fill screen black\r\n"
    "  status            Show current screen colour\r\n"
    "  help              Show this help text\r\n"
    "  exit | quit       Close the connection\r\n";
#else
static const char s_help[] =
    "Commands:\r\n"
    "  color <name>      Named colour: red green blue white yellow\r\n"
    "                    cyan magenta purple orange pink\r\n"
    "  color R G B       RGB triplet, each value 0-255\r\n"
    "  color #RRGGBB     Hex colour (e.g. #FF8800)\r\n"
    "  off               Turn the LED off\r\n"
    "  status            Show current LED colour\r\n"
    "  help              Show this help text\r\n"
    "  exit | quit       Close the connection\r\n";
#endif

/* ------------------------------------------------------------------ */
/* Handle a single parsed command.                                     */
/* Returns 0 to continue, 1 to close the session.                     */
/* ------------------------------------------------------------------ */

static int handle_command(WOLFSSH *ssh, const char *line)
{
    char tmp[LINE_BUF_SZ];
    uint8_t r = 0, g = 0, b = 0;

    line = skip_ws(line);

    if (*line == '\0') {
        return 0;  /* empty line */
    }

    /* ---- exit / quit ---- */
    if (strcasecmp(line, "exit") == 0 || strcasecmp(line, "quit") == 0) {
        ssh_puts(ssh, "Bye!" CRLF);
        return 1;
    }

    /* ---- help ---- */
    if (strcasecmp(line, "help") == 0) {
        ssh_puts(ssh, s_help);
        return 0;
    }

    /* ---- off ---- */
    if (strcasecmp(line, "off") == 0) {
#if CONFIG_HARDWARE_JC3248W535
        screen_off();
        ssh_puts(ssh, "Screen off." CRLF);
#else
        led_off();
        ssh_puts(ssh, "LED off." CRLF);
#endif
        return 0;
    }

    /* ---- status ---- */
    if (strcasecmp(line, "status") == 0) {
#if CONFIG_HARDWARE_JC3248W535
        screen_get_color(&r, &g, &b);
        if (r == 0 && g == 0 && b == 0) {
            ssh_puts(ssh, "Screen is off (black)." CRLF);
        } else {
            snprintf(tmp, sizeof(tmp),
                     "Screen: R:%-3d G:%-3d B:%-3d  (#%02X%02X%02X)" CRLF,
                     r, g, b, r, g, b);
            ssh_puts(ssh, tmp);
        }
#else
        led_get_color(&r, &g, &b);
        if (r == 0 && g == 0 && b == 0) {
            ssh_puts(ssh, "LED is off." CRLF);
        } else {
            snprintf(tmp, sizeof(tmp),
                     "LED: R:%-3d G:%-3d B:%-3d  (#%02X%02X%02X)" CRLF,
                     r, g, b, r, g, b);
            ssh_puts(ssh, tmp);
        }
#endif
        return 0;
    }

    /* ---- color ---- */
    if (strncasecmp(line, "color", 5) == 0 ||
        strncasecmp(line, "colour", 6) == 0) {

        /* Advance past the keyword */
        const char *arg = line + (strncasecmp(line, "colour", 6) == 0 ? 6 : 5);
        arg = skip_ws(arg);

        int ok = 0;

        if (*arg == '#') {
            ok = parse_hex(arg, &r, &g, &b);
        } else if (isdigit((unsigned char)*arg)) {
            ok = parse_rgb(arg, &r, &g, &b);
        } else {
            ok = parse_name(arg, &r, &g, &b);
        }

        if (!ok) {
            ssh_puts(ssh, "Unknown colour.  Try: color red  |  color 255 128 0  |  color #FF8800" CRLF);
            return 0;
        }

#if CONFIG_HARDWARE_JC3248W535
        screen_set_color(r, g, b);
        snprintf(tmp, sizeof(tmp),
                 "Screen set to R:%-3d G:%-3d B:%-3d  (#%02X%02X%02X)" CRLF,
                 r, g, b, r, g, b);
#else
        led_set_color(r, g, b);
        snprintf(tmp, sizeof(tmp),
                 "LED set to R:%-3d G:%-3d B:%-3d  (#%02X%02X%02X)" CRLF,
                 r, g, b, r, g, b);
#endif
        ssh_puts(ssh, tmp);
        return 0;
    }

    snprintf(tmp, sizeof(tmp), "Unknown command: '%s'.  Type 'help'." CRLF, line);
    ssh_puts(ssh, tmp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Per-connection state passed from listener_task to session_task      */
/* ------------------------------------------------------------------ */

typedef struct {
    WOLFSSH *ssh;
    char     exec_cmd[LINE_BUF_SZ]; /* non-empty when exec request arrived */
} ssh_session_t;

/* Called by wolfSSH inside wolfSSH_accept() when an exec channel
 * request is received.  Runs in listener_task context — no
 * cross-task memory ordering concerns. */
static int exec_req_cb(WOLFSSH_CHANNEL *channel, void *ctx)
{
    ssh_session_t *s = (ssh_session_t *)ctx;
    if (s) {
        const char *cmd = wolfSSH_ChannelGetSessionCommand(channel);
        if (cmd && cmd[0] != '\0') {
            strncpy(s->exec_cmd, cmd, LINE_BUF_SZ - 1);
            s->exec_cmd[LINE_BUF_SZ - 1] = '\0';
        }
    }
    return 0;  /* 0 = accept */
}

/* ------------------------------------------------------------------ */
/* Per-connection task                                                 */
/* ------------------------------------------------------------------ */

static void session_task(void *pvParam)
{
    ssh_session_t *s   = (ssh_session_t *)pvParam;
    WOLFSSH       *ssh = s->ssh;

    /* ---- Non-interactive exec: ssh host 'command' ---- */
    if (s->exec_cmd[0] != '\0') {
        handle_command(ssh, s->exec_cmd);
        wolfSSH_stream_exit(ssh, 0);
        int fd = wolfSSH_get_fd(ssh);
        free(s);
        wolfSSH_free(ssh);
        close(fd);
        ESP_LOGI(TAG, "Exec session closed");
        vTaskDelete(NULL);
        return;
    }
    free(s);

    /* ---- Interactive shell ---- */
    char   line[LINE_BUF_SZ];
    hist_t hist;
    memset(&hist, 0, sizeof(hist));

    /* Send banner and initial prompt */
    ssh_puts(ssh, s_banner);
    ssh_puts(ssh, PROMPT);

    while (1) {
        char *got = read_line(ssh, line, sizeof(line), &hist);
        if (!got) break;          /* channel closed */

        /* Add non-empty lines to history before handling */
        if (line[0] != '\0') hist_add(&hist, line);

        int done = handle_command(ssh, line);
        if (done) break;

        ssh_puts(ssh, PROMPT);
    }

    /* Graceful shutdown */
    wolfSSH_stream_exit(ssh, 0);

    int fd = wolfSSH_get_fd(ssh);
    wolfSSH_free(ssh);
    close(fd);

    ESP_LOGI(TAG, "Session closed");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Listener / acceptor task                                            */
/* ------------------------------------------------------------------ */

static void listener_task(void *pvParam)
{
    (void)pvParam;

    /* ------- Load or generate the server host key ------- */
    static byte s_hostkey[KEY_BUF_SZ];
    size_t hk_sz = load_hostkey_from_nvs(s_hostkey, sizeof(s_hostkey));
    if (hk_sz == 0) {
        hk_sz = generate_and_store_hostkey(s_hostkey, sizeof(s_hostkey));
    }
    if (hk_sz == 0) {
        ESP_LOGE(TAG, "Cannot obtain host key — SSH server will not start.");
        vTaskDelete(NULL);
        return;
    }

    /* ------- wolfSSH context ------- */
    s_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
    if (!s_ctx) {
        ESP_LOGE(TAG, "wolfSSH_CTX_new failed");
        vTaskDelete(NULL);
        return;
    }

    wolfSSH_SetUserAuth(s_ctx, user_auth_cb);
    wolfSSH_CTX_SetBanner(s_ctx, "");  /* We send our own banner per session */
    wolfSSH_CTX_SetChannelReqExecCb(s_ctx, exec_req_cb);

    int rc = wolfSSH_CTX_UsePrivateKey_buffer(s_ctx,
                                              s_hostkey, (word32)hk_sz,
                                              WOLFSSH_FORMAT_ASN1);
    if (rc != WS_SUCCESS) {
        ESP_LOGE(TAG, "UsePrivateKey_buffer failed: %d", rc);
        wolfSSH_CTX_free(s_ctx);
        vTaskDelete(NULL);
        return;
    }

    /* ------- TCP listener ------- */
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed");
        wolfSSH_CTX_free(s_ctx);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(CONFIG_SSH_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind() failed on port %d", CONFIG_SSH_PORT);
        close(listen_fd);
        wolfSSH_CTX_free(s_ctx);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, 3) != 0) {
        ESP_LOGE(TAG, "listen() failed");
        close(listen_fd);
        wolfSSH_CTX_free(s_ctx);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "SSH server listening on port %d", CONFIG_SSH_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t          client_len = sizeof(client_addr);

        int client_fd = accept(listen_fd,
                               (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            ESP_LOGW(TAG, "accept() failed: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        ESP_LOGI(TAG, "New connection from %s", client_ip);

        /* Create WOLFSSH object for this connection */
        WOLFSSH *ssh = wolfSSH_new(s_ctx);
        if (!ssh) {
            ESP_LOGE(TAG, "wolfSSH_new failed");
            close(client_fd);
            continue;
        }

        wolfSSH_set_fd(ssh, client_fd);

        /* Allocate per-session state; the exec callback will fill exec_cmd */
        ssh_session_t *sess = (ssh_session_t *)malloc(sizeof(ssh_session_t));
        if (!sess) {
            ESP_LOGE(TAG, "malloc session state failed");
            wolfSSH_free(ssh);
            close(client_fd);
            continue;
        }
        sess->ssh = ssh;
        sess->exec_cmd[0] = '\0';
        wolfSSH_SetChannelReqCtx(ssh, sess);

        /* SSH handshake (exec_req_cb fires here if client sends exec) */
        int error;
        do {
            rc    = wolfSSH_accept(ssh);
            error = wolfSSH_get_error(ssh);
        } while (error == WS_WANT_READ || error == WS_WANT_WRITE);

        if (rc != WS_SUCCESS) {
            ESP_LOGW(TAG, "wolfSSH_accept failed: %d", rc);
            free(sess);
            wolfSSH_free(ssh);
            close(client_fd);
            continue;
        }

        /* Spawn a task for this session so we can accept the next connection */
        xTaskCreate(session_task, "ssh_sess",
                    SSH_TASK_STACK, sess,
                    SSH_TASK_PRIO, NULL);
    }

    /* Never reached */
    close(listen_fd);
    wolfSSH_CTX_free(s_ctx);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t ssh_server_start(void)
{
    if (wolfSSH_Init() != WS_SUCCESS) {
        ESP_LOGE(TAG, "wolfSSH_Init failed");
        return ESP_FAIL;
    }

    BaseType_t ret = xTaskCreate(listener_task, "ssh_listen",
                                 SSH_TASK_STACK, NULL,
                                 SSH_TASK_PRIO, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SSH listener task");
        wolfSSH_Cleanup();
        return ESP_FAIL;
    }

    return ESP_OK;
}
