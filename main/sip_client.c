#include "sip_client.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_rom_md5.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"

static const char *TAG = "sip";

#define SIP_DEFAULT_PORT   5061
#define SIP_REG_EXPIRES    300
#define SIP_KEEPALIVE_SEC  30
#define SIP_RX_BUF         4096
#define SIP_MSG_BUF        3072

/* ── Configuration / connection state ─────────────────────────────────────── */

static sip_config_t   s_cfg;
static sip_event_cb_t s_cb;
static void          *s_cb_ctx;
static char           s_local_ip[40];

static volatile bool  s_running;
static TaskHandle_t   s_task;
static QueueHandle_t  s_cmd_q;

static esp_tls_t     *s_tls;
static bool           s_connected;
static volatile bool  s_registered;

static char          *s_rxbuf;     /* SIP_RX_BUF bytes, allocated from PSRAM */
static size_t         s_rxlen;
/* Off-stack scratch (PSRAM) so the sip task stack stays small: s_msgbuf builds
 * outgoing messages, s_inbuf holds the incoming message being dispatched. Only
 * the single-threaded sip task touches them, and never both as the same role. */
static char          *s_msgbuf;
static char          *s_inbuf;

/* Registration dialog */
static char     s_reg_callid[96];
static char     s_reg_fromtag[24];
static int      s_reg_cseq;
static TickType_t s_next_register;
static TickType_t s_next_keepalive;

/* Auth challenge (most recent) */
static char     s_auth_realm[96];
static char     s_auth_nonce[160];
static char     s_auth_qop[24];
static char     s_auth_opaque[96];
static bool     s_auth_proxy;     /* true → 407/Proxy-Authorization */
static bool     s_auth_have;

/* ── Single-call dialog ───────────────────────────────────────────────────── */

typedef enum {
    CALL_NONE,
    CALL_OUT_TRYING,
    CALL_OUT_RINGING,
    CALL_IN_RINGING,
    CALL_UP,
} call_state_t;

static struct {
    call_state_t state;
    bool     outgoing;
    char     call_id[96];
    char     local_tag[24];
    int      cseq;
    char     invite_branch[40];
    char     hdr_local[256];   /* From-spec for requests we originate */
    char     hdr_peer[256];    /* To-spec for requests we originate */
    char     ruri[160];        /* in-dialog request URI (peer Contact) */
    char     peer_user[96];
    /* stored incoming-INVITE headers, for building responses */
    char     in_via[600];
    char     in_from[200];
    char     in_to[200];
    char     in_cseq[64];
    char     in_contact[200];
    /* media */
    char     remote_ip[40];
    uint16_t remote_port;
    uint16_t local_port;
    sip_codec_t codec;
} s_call;

/* ── Command queue (cross-task requests) ──────────────────────────────────── */

typedef enum { CMD_CALL, CMD_ANSWER, CMD_HANGUP } cmd_kind_t;
typedef struct { cmd_kind_t kind; char arg[96]; } sip_cmd_msg_t;

/* ── Small helpers ────────────────────────────────────────────────────────── */

static void rand_hex(char *out, int nbytes)
{
    for (int i = 0; i < nbytes; i++)
        sprintf(out + i * 2, "%02x", (unsigned)(esp_random() & 0xFF));
    out[nbytes * 2] = '\0';
}

static void md5_hex(const char *s, char out[33])
{
    unsigned char d[16];
    md5_context_t ctx;
    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, s, (uint32_t)strlen(s));
    esp_rom_md5_final(d, &ctx);
    for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", d[i]);
    out[32] = '\0';
}

static void get_local_ip(void)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK) {
        snprintf(s_local_ip, sizeof(s_local_ip), IPSTR, IP2STR(&ip.ip));
    } else {
        strlcpy(s_local_ip, "0.0.0.0", sizeof(s_local_ip));
    }
}

/* Copy the first header value (text after "Name:") into out, trimmed. */
static bool hdr_get(const char *msg, const char *name, char *out, size_t outsz)
{
    size_t nlen = strlen(name);
    const char *p = msg;
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        if (linelen > nlen && strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vlen = (p + linelen) - v;
            if (vlen >= outsz) vlen = outsz - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return true;
        }
        if (!eol) break;
        p = eol + 2;
    }
    out[0] = '\0';
    return false;
}

/* Concatenate all header lines named `name` (e.g. multiple Via) into out,
 * each terminated with CRLF, preserving the original "Name: value" text. */
static void hdr_get_all(const char *msg, const char *name, char *out, size_t outsz)
{
    size_t nlen = strlen(name);
    const char *p = msg;
    size_t used = 0;
    out[0] = '\0';
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        if (linelen == 0) break;   /* blank line → end of headers */
        if (linelen > nlen && strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            if (used + linelen + 3 < outsz) {
                memcpy(out + used, p, linelen);
                used += linelen;
                out[used++] = '\r';
                out[used++] = '\n';
                out[used] = '\0';
            }
        }
        if (!eol) break;
        p = eol + 2;
    }
}

/* Extract param value: key="quoted" or key=token from a header value string. */
static bool param_val(const char *src, const char *key, char *out, size_t outsz)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(src, pat);
    if (!p) return false;
    p += strlen(pat);
    if (*p == '"') {
        p++;
        const char *end = strchr(p, '"');
        if (!end) return false;
        size_t len = end - p;
        if (len >= outsz) len = outsz - 1;
        memcpy(out, p, len);
        out[len] = '\0';
    } else {
        size_t len = 0;
        while (p[len] && p[len] != ',' && p[len] != ' ' && p[len] != '\r' &&
               p[len] != ';' && len < outsz - 1) len++;
        memcpy(out, p, len);
        out[len] = '\0';
    }
    return true;
}

static int response_code(const char *msg)
{
    /* "SIP/2.0 200 OK" */
    if (strncmp(msg, "SIP/2.0 ", 8) != 0) return -1;
    return atoi(msg + 8);
}

static bool is_response(const char *msg)
{
    return strncmp(msg, "SIP/2.0 ", 8) == 0;
}

static void request_method(const char *msg, char *out, size_t outsz)
{
    size_t i = 0;
    while (msg[i] && msg[i] != ' ' && i < outsz - 1) { out[i] = msg[i]; i++; }
    out[i] = '\0';
}

/* ── TLS I/O ──────────────────────────────────────────────────────────────── */

static bool tls_send(const char *buf, size_t len)
{
    if (!s_tls) return false;
    size_t off = 0;
    while (off < len) {
        int w = esp_tls_conn_write(s_tls, buf + off, len - off);
        if (w > 0) {
            off += w;
        } else if (w == ESP_TLS_ERR_SSL_WANT_WRITE || w == ESP_TLS_ERR_SSL_WANT_READ) {
            vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            ESP_LOGW(TAG, "tls write failed: %d", w);
            return false;
        }
    }
    return true;
}

static bool sip_send(const char *msg)
{
    ESP_LOGD(TAG, ">>> %.80s", msg);
    return tls_send(msg, strlen(msg));
}

/* ── Digest auth ──────────────────────────────────────────────────────────── */

static void parse_challenge(const char *msg)
{
    char chal[400];
    if (hdr_get(msg, "WWW-Authenticate", chal, sizeof(chal))) {
        s_auth_proxy = false;
    } else if (hdr_get(msg, "Proxy-Authenticate", chal, sizeof(chal))) {
        s_auth_proxy = true;
    } else {
        return;
    }
    param_val(chal, "realm", s_auth_realm, sizeof(s_auth_realm));
    param_val(chal, "nonce", s_auth_nonce, sizeof(s_auth_nonce));
    if (!param_val(chal, "qop", s_auth_qop, sizeof(s_auth_qop))) s_auth_qop[0] = '\0';
    if (!param_val(chal, "opaque", s_auth_opaque, sizeof(s_auth_opaque))) s_auth_opaque[0] = '\0';
    s_auth_have = true;
    ESP_LOGI(TAG, "auth challenge realm=%s qop=%s proxy=%d",
             s_auth_realm, s_auth_qop, s_auth_proxy);
}

/* Build "Authorization:"/"Proxy-Authorization:" line (incl. trailing CRLF). */
static void build_auth_header(const char *method, const char *uri,
                              char *out, size_t outsz)
{
    char ha1[33], ha2[33], resp[33], buf[400];
    snprintf(buf, sizeof(buf), "%s:%s:%s", s_cfg.user, s_auth_realm, s_cfg.password);
    md5_hex(buf, ha1);
    snprintf(buf, sizeof(buf), "%s:%s", method, uri);
    md5_hex(buf, ha2);

    const char *hname = s_auth_proxy ? "Proxy-Authorization" : "Authorization";

    if (s_auth_qop[0]) {
        char cnonce[17];
        rand_hex(cnonce, 8);
        const char *nc = "00000001";
        snprintf(buf, sizeof(buf), "%s:%s:%s:%s:auth:%s",
                 ha1, s_auth_nonce, nc, cnonce, ha2);
        md5_hex(buf, resp);
        snprintf(out, outsz,
            "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
            "response=\"%s\", algorithm=MD5, qop=auth, nc=%s, cnonce=\"%s\"",
            hname, s_cfg.user, s_auth_realm, s_auth_nonce, uri, resp, nc, cnonce);
    } else {
        snprintf(buf, sizeof(buf), "%s:%s:%s", ha1, s_auth_nonce, ha2);
        md5_hex(buf, resp);
        snprintf(out, outsz,
            "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
            "response=\"%s\", algorithm=MD5",
            hname, s_cfg.user, s_auth_realm, s_auth_nonce, uri, resp);
    }
    if (s_auth_opaque[0]) {
        size_t l = strlen(out);
        snprintf(out + l, outsz - l, ", opaque=\"%s\"", s_auth_opaque);
    }
    size_t l = strlen(out);
    snprintf(out + l, outsz - l, "\r\n");
}

static const char *sip_domain(void)
{
    return s_cfg.domain[0] ? s_cfg.domain : s_cfg.server;
}

/* ── SDP ──────────────────────────────────────────────────────────────────── */

static int build_sdp(char *out, size_t outsz, uint16_t rtp_port, int single_pt)
{
    uint32_t sid = esp_random();
    if (single_pt >= 0) {
        return snprintf(out, outsz,
            "v=0\r\no=- %lu %lu IN IP4 %s\r\ns=core2\r\nc=IN IP4 %s\r\nt=0 0\r\n"
            "m=audio %u RTP/AVP %d\r\na=rtpmap:%d %s/8000\r\na=ptime:20\r\na=sendrecv\r\n",
            (unsigned long)sid, (unsigned long)sid, s_local_ip, s_local_ip,
            rtp_port, single_pt, single_pt, single_pt == 8 ? "PCMA" : "PCMU");
    }
    /* Offer PCMU only: A-law (PCMA) audio is noticeably worse on this device,
     * and PCMU (µ-law) is mandatory G.711 that every server supports. Offering
     * only PCMU forces the cleaner codec for outgoing calls. */
    return snprintf(out, outsz,
        "v=0\r\no=- %lu %lu IN IP4 %s\r\ns=core2\r\nc=IN IP4 %s\r\nt=0 0\r\n"
        "m=audio %u RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n"
        "a=ptime:20\r\na=sendrecv\r\n",
        (unsigned long)sid, (unsigned long)sid, s_local_ip, s_local_ip, rtp_port);
}

/* Parse peer SDP: fill remote ip/port and choose a codec (prefer PCMU). */
static bool parse_sdp(const char *body, char *ip, size_t ipsz,
                      uint16_t *port, sip_codec_t *codec)
{
    const char *c = strstr(body, "c=IN IP4 ");
    const char *m = strstr(body, "m=audio ");
    if (!c || !m) return false;
    c += 9;
    size_t i = 0;
    while (c[i] && c[i] != '\r' && c[i] != '\n' && c[i] != ' ' && i < ipsz - 1) {
        ip[i] = c[i]; i++;
    }
    ip[i] = '\0';
    *port = (uint16_t)atoi(m + 8);

    /* Payload type list follows "RTP/AVP "; prefer 0 (PCMU), else 8 (PCMA). */
    const char *avp = strstr(m, "RTP/AVP");
    *codec = SIP_CODEC_PCMU;
    if (avp) {
        avp += 7;
        bool has0 = false, has8 = false;
        char tok[8];
        int t = 0;
        for (const char *q = avp; ; q++) {
            if (*q == ' ') {
                continue;
            } else if (*q >= '0' && *q <= '9') {
                if (t < 7) tok[t++] = *q;
            } else {
                if (t) {
                    tok[t] = '\0';
                    int pt = atoi(tok);
                    if (pt == 0) has0 = true;
                    if (pt == 8) has8 = true;
                    t = 0;
                }
                if (*q == '\r' || *q == '\n' || *q == '\0') break;
            }
        }
        if (!has0 && has8) *codec = SIP_CODEC_PCMA;
    }
    return true;
}

static const char *msg_body(const char *msg)
{
    const char *b = strstr(msg, "\r\n\r\n");
    return b ? b + 4 : "";
}

/* ── Event dispatch ───────────────────────────────────────────────────────── */

static void emit(sip_event_t type)
{
    if (!s_cb) return;
    sip_event_info_t ev = { .type = type };
    strlcpy(ev.caller, s_call.peer_user, sizeof(ev.caller));
    strlcpy(ev.remote_ip, s_call.remote_ip, sizeof(ev.remote_ip));
    ev.remote_rtp_port = s_call.remote_port;
    ev.local_rtp_port  = s_call.local_port;
    ev.codec           = s_call.codec;
    s_cb(&ev, s_cb_ctx);
}

/* ── REGISTER ─────────────────────────────────────────────────────────────── */

static void send_register(bool with_auth)
{
    char branch[40]; rand_hex(branch, 10);
    char uri[100]; snprintf(uri, sizeof(uri), "sip:%s", sip_domain());
    char auth[500]; auth[0] = '\0';
    if (with_auth && s_auth_have) build_auth_header("REGISTER", uri, auth, sizeof(auth));

    s_reg_cseq++;
    char *msg = s_msgbuf;
    snprintf(msg, SIP_MSG_BUF,
        "REGISTER %s SIP/2.0\r\n"
        "Via: SIP/2.0/TLS %s:%d;branch=z9hG4bK%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %d REGISTER\r\n"
        "Contact: <sip:%s@%s:%d;transport=tls>\r\n"
        "Expires: %d\r\n"
        "User-Agent: Core2-SIP/1.0\r\n"
        "%s"
        "Content-Length: 0\r\n\r\n",
        uri, s_local_ip, SIP_DEFAULT_PORT, branch,
        s_cfg.user, sip_domain(), s_reg_fromtag,
        s_cfg.user, sip_domain(),
        s_reg_callid, s_reg_cseq,
        s_cfg.user, s_local_ip, SIP_DEFAULT_PORT,
        SIP_REG_EXPIRES, auth);
    sip_send(msg);
}

/* ── Outgoing INVITE ──────────────────────────────────────────────────────── */

static void send_invite(bool with_auth)
{
    char auth[500]; auth[0] = '\0';
    if (with_auth && s_auth_have)
        build_auth_header("INVITE", s_call.ruri, auth, sizeof(auth));

    char sdp[400];
    int sdplen = build_sdp(sdp, sizeof(sdp), s_call.local_port, -1);

    /* Each INVITE (including the auth-retry after 401/407) is a NEW client
     * transaction → fresh Via branch. Reusing the branch makes the server
     * treat the retry as a duplicate (482 Loop Detected). The ACK for the
     * preceding failure was already sent with the old branch before this. */
    rand_hex(s_call.invite_branch, 10);

    s_call.cseq++;
    char *msg = s_msgbuf;
    snprintf(msg, SIP_MSG_BUF,
        "INVITE %s SIP/2.0\r\n"
        "Via: SIP/2.0/TLS %s:%d;branch=z9hG4bK%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %d INVITE\r\n"
        "Contact: <sip:%s@%s:%d;transport=tls>\r\n"
        "User-Agent: Core2-SIP/1.0\r\n"
        "%s"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n\r\n%s",
        s_call.ruri, s_local_ip, SIP_DEFAULT_PORT, s_call.invite_branch,
        s_call.hdr_local, s_call.hdr_peer, s_call.call_id, s_call.cseq,
        s_cfg.user, s_local_ip, SIP_DEFAULT_PORT, auth, sdplen, sdp);
    sip_send(msg);
}

/* ACK a non-2xx final response to our INVITE (same branch + CSeq number). */
static void send_ack_failure(const char *to_with_tag)
{
    char *msg = s_msgbuf;
    snprintf(msg, SIP_MSG_BUF,
        "ACK %s SIP/2.0\r\n"
        "Via: SIP/2.0/TLS %s:%d;branch=z9hG4bK%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %d ACK\r\n"
        "Content-Length: 0\r\n\r\n",
        s_call.ruri, s_local_ip, SIP_DEFAULT_PORT, s_call.invite_branch,
        s_call.hdr_local, to_with_tag, s_call.call_id, s_call.cseq);
    sip_send(msg);
}

/* ACK a 2xx (new transaction → new branch). */
static void send_ack_ok(void)
{
    char branch[40]; rand_hex(branch, 10);
    char *msg = s_msgbuf;
    snprintf(msg, SIP_MSG_BUF,
        "ACK %s SIP/2.0\r\n"
        "Via: SIP/2.0/TLS %s:%d;branch=z9hG4bK%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %d ACK\r\n"
        "Content-Length: 0\r\n\r\n",
        s_call.ruri, s_local_ip, SIP_DEFAULT_PORT, branch,
        s_call.hdr_local, s_call.hdr_peer, s_call.call_id, s_call.cseq);
    sip_send(msg);
}

/* In-dialog BYE. */
static void send_bye(void)
{
    char branch[40]; rand_hex(branch, 10);
    s_call.cseq++;
    char *msg = s_msgbuf;
    snprintf(msg, SIP_MSG_BUF,
        "BYE %s SIP/2.0\r\n"
        "Via: SIP/2.0/TLS %s:%d;branch=z9hG4bK%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %d BYE\r\n"
        "User-Agent: Core2-SIP/1.0\r\n"
        "Content-Length: 0\r\n\r\n",
        s_call.ruri, s_local_ip, SIP_DEFAULT_PORT, branch,
        s_call.hdr_local, s_call.hdr_peer, s_call.call_id, s_call.cseq);
    sip_send(msg);
}

/* CANCEL our pending outgoing INVITE (same branch + CSeq number). */
static void send_cancel(void)
{
    char *msg = s_msgbuf;
    snprintf(msg, SIP_MSG_BUF,
        "CANCEL %s SIP/2.0\r\n"
        "Via: SIP/2.0/TLS %s:%d;branch=z9hG4bK%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %d CANCEL\r\n"
        "Content-Length: 0\r\n\r\n",
        s_call.ruri, s_local_ip, SIP_DEFAULT_PORT, s_call.invite_branch,
        s_call.hdr_local, s_call.hdr_peer, s_call.call_id, s_call.cseq);
    sip_send(msg);
}

/* ── Responses to incoming requests ───────────────────────────────────────── */

/* Build a response echoing Via/From/Call-ID/CSeq from the request; To gets our
 * tag appended if absent. Optionally append Contact + SDP body. */
static void send_response(const char *req, int code, const char *reason,
                          bool add_contact, const char *sdp)
{
    char via[600], from[200], to[200], callid[96], cseq[64];
    hdr_get_all(req, "Via", via, sizeof(via));
    hdr_get(req, "From", from, sizeof(from));
    hdr_get(req, "To", to, sizeof(to));
    hdr_get(req, "Call-ID", callid, sizeof(callid));
    hdr_get(req, "CSeq", cseq, sizeof(cseq));

    /* Append our tag to To if it has none (needed for >100 dialog-forming). */
    char to_tagged[240];
    if (code > 100 && !strstr(to, "tag=")) {
        snprintf(to_tagged, sizeof(to_tagged), "%s;tag=%s", to, s_call.local_tag);
    } else {
        strlcpy(to_tagged, to, sizeof(to_tagged));
    }

    char contact[200]; contact[0] = '\0';
    if (add_contact) {
        snprintf(contact, sizeof(contact),
                 "Contact: <sip:%s@%s:%d;transport=tls>\r\n",
                 s_cfg.user, s_local_ip, SIP_DEFAULT_PORT);
    }

    char *msg = s_msgbuf;
    if (sdp && sdp[0]) {
        snprintf(msg, SIP_MSG_BUF,
            "SIP/2.0 %d %s\r\n%sFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n"
            "%sContent-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s",
            code, reason, via, from, to_tagged, callid, cseq, contact,
            (int)strlen(sdp), sdp);
    } else {
        snprintf(msg, SIP_MSG_BUF,
            "SIP/2.0 %d %s\r\n%sFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n"
            "%sContent-Length: 0\r\n\r\n",
            code, reason, via, from, to_tagged, callid, cseq, contact);
    }
    sip_send(msg);
}

static void call_reset(void)
{
    memset(&s_call, 0, sizeof(s_call));
    s_call.state = CALL_NONE;
}

/* ── Incoming request handling ────────────────────────────────────────────── */

static void handle_invite(const char *msg)
{
    if (s_call.state != CALL_NONE) {
        send_response(msg, 486, "Busy Here", false, NULL);
        return;
    }
    call_reset();
    s_call.outgoing = false;
    rand_hex(s_call.local_tag, 8);

    hdr_get_all(msg, "Via", s_call.in_via, sizeof(s_call.in_via));
    hdr_get(msg, "From", s_call.in_from, sizeof(s_call.in_from));
    hdr_get(msg, "To", s_call.in_to, sizeof(s_call.in_to));
    hdr_get(msg, "Call-ID", s_call.call_id, sizeof(s_call.call_id));
    hdr_get(msg, "CSeq", s_call.in_cseq, sizeof(s_call.in_cseq));
    hdr_get(msg, "Contact", s_call.in_contact, sizeof(s_call.in_contact));

    /* Caller display: user part of From URI. */
    {
        const char *u = strstr(s_call.in_from, "sip:");
        if (u) {
            u += 4;
            size_t i = 0;
            while (u[i] && u[i] != '@' && u[i] != '>' && i < sizeof(s_call.peer_user) - 1) {
                s_call.peer_user[i] = u[i]; i++;
            }
            s_call.peer_user[i] = '\0';
        }
    }

    /* In-dialog targets for our future responses' BYE: From=us(To+tag), To=caller(From). */
    snprintf(s_call.hdr_local, sizeof(s_call.hdr_local), "%s;tag=%s",
             s_call.in_to, s_call.local_tag);
    strlcpy(s_call.hdr_peer, s_call.in_from, sizeof(s_call.hdr_peer));
    /* Request URI for in-dialog requests = peer Contact (strip <>). */
    {
        const char *lt = strchr(s_call.in_contact, '<');
        const char *gt = lt ? strchr(lt, '>') : NULL;
        if (lt && gt) {
            size_t len = gt - (lt + 1);
            if (len >= sizeof(s_call.ruri)) len = sizeof(s_call.ruri) - 1;
            memcpy(s_call.ruri, lt + 1, len);
            s_call.ruri[len] = '\0';
        } else {
            strlcpy(s_call.ruri, s_call.in_contact, sizeof(s_call.ruri));
        }
    }

    if (!parse_sdp(msg_body(msg), s_call.remote_ip, sizeof(s_call.remote_ip),
                   &s_call.remote_port, &s_call.codec)) {
        ESP_LOGW(TAG, "INVITE without parseable SDP");
        send_response(msg, 488, "Not Acceptable Here", false, NULL);
        call_reset();
        return;
    }
    s_call.local_port = sip_rtp_pick_local_port();
    s_call.state = CALL_IN_RINGING;

    if (strstr(msg_body(msg), "a=candidate")) {
        ESP_LOGW(TAG, "INVITE offer uses ICE — media may not flow without ICE; "
                      "relying on symmetric RTP latching");
    }

    send_response(msg, 100, "Trying", false, NULL);
    send_response(msg, 180, "Ringing", false, NULL);
    ESP_LOGI(TAG, "incoming call from %s (%s:%u codec=%d)",
             s_call.peer_user, s_call.remote_ip, s_call.remote_port, s_call.codec);
    emit(SIP_EVT_INCOMING);
}

/* Answer the stored incoming INVITE: 200 OK with our SDP. We rebuild the 200
 * from the original INVITE headers we stashed. */
static void answer_incoming(void)
{
    if (s_call.state != CALL_IN_RINGING) return;
    char sdp[400];
    build_sdp(sdp, sizeof(sdp), s_call.local_port, (int)s_call.codec);

    char to_tagged[240];
    snprintf(to_tagged, sizeof(to_tagged), "%s;tag=%s", s_call.in_to, s_call.local_tag);

    char contact[200];
    snprintf(contact, sizeof(contact), "Contact: <sip:%s@%s:%d;transport=tls>\r\n",
             s_cfg.user, s_local_ip, SIP_DEFAULT_PORT);

    char *msg = s_msgbuf;
    snprintf(msg, SIP_MSG_BUF,
        "SIP/2.0 200 OK\r\n%sFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\nCSeq: %s\r\n"
        "%sContent-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s",
        s_call.in_via, s_call.in_from, to_tagged, s_call.call_id, s_call.in_cseq,
        contact, (int)strlen(sdp), sdp);
    sip_send(msg);
    s_call.state = CALL_UP;
    ESP_LOGI(TAG, "answered; awaiting ACK");
    emit(SIP_EVT_ANSWERED);
}

static void handle_bye(const char *msg)
{
    send_response(msg, 200, "OK", false, NULL);
    if (s_call.state != CALL_NONE) {
        ESP_LOGI(TAG, "peer hung up");
        call_reset();
        emit(SIP_EVT_ENDED);
    }
}

static void handle_cancel(const char *msg)
{
    send_response(msg, 200, "OK", false, NULL);
    if (s_call.state == CALL_IN_RINGING) {
        /* Terminate the INVITE transaction. */
        send_response(msg, 487, "Request Terminated", false, NULL);
        call_reset();
        emit(SIP_EVT_ENDED);
    }
}

/* ── Response handling (to our requests) ──────────────────────────────────── */

static void handle_response(const char *msg)
{
    int code = response_code(msg);
    char cseq[64];
    hdr_get(msg, "CSeq", cseq, sizeof(cseq));
    bool is_register = strstr(cseq, "REGISTER") != NULL;
    bool is_invite   = strstr(cseq, "INVITE") != NULL;

    if (is_register) {
        if (code == 401 || code == 407) {
            parse_challenge(msg);
            send_register(true);
        } else if (code == 200) {
            if (!s_registered) ESP_LOGI(TAG, "registered");
            s_registered = true;
            s_next_register = xTaskGetTickCount() +
                              pdMS_TO_TICKS((SIP_REG_EXPIRES / 2) * 1000);
            if (s_cb) { sip_event_info_t ev = { .type = SIP_EVT_REGISTERED };
                        s_cb(&ev, s_cb_ctx); }
        } else if (code >= 400) {
            ESP_LOGW(TAG, "REGISTER failed: %d", code);
            s_registered = false;
            if (s_cb) { sip_event_info_t ev = { .type = SIP_EVT_REGISTER_FAILED };
                        s_cb(&ev, s_cb_ctx); }
        }
        return;
    }

    if (is_invite) {
        char to[200]; hdr_get(msg, "To", to, sizeof(to));

        if (code == 401 || code == 407) {
            /* ACK the challenge with the response's To (incl. server tag), but
             * the auth-retry INVITE must keep the ORIGINAL tag-less To — reusing
             * the challenge's To-tag makes the proxy see an in-dialog request
             * and reject it (482 Loop Detected). So do NOT capture it here. */
            send_ack_failure(to);
            parse_challenge(msg);
            send_invite(true);
        } else if (code == 100) {
            /* Trying — no action. */
        } else if (code == 180 || code == 183) {
            if (s_call.state == CALL_OUT_TRYING) {
                s_call.state = CALL_OUT_RINGING;
                emit(SIP_EVT_RINGING);
            }
        } else if (code == 200) {
            /* Dialog established — capture the peer To (with tag) for ACK/BYE. */
            if (strstr(to, "tag=")) strlcpy(s_call.hdr_peer, to, sizeof(s_call.hdr_peer));
            char contact[200];
            if (hdr_get(msg, "Contact", contact, sizeof(contact))) {
                const char *lt = strchr(contact, '<');
                const char *gt = lt ? strchr(lt, '>') : NULL;
                if (lt && gt) { size_t len = gt - (lt + 1);
                    if (len >= sizeof(s_call.ruri)) len = sizeof(s_call.ruri) - 1;
                    memcpy(s_call.ruri, lt + 1, len); s_call.ruri[len] = '\0'; }
            }
            parse_sdp(msg_body(msg), s_call.remote_ip, sizeof(s_call.remote_ip),
                      &s_call.remote_port, &s_call.codec);
            send_ack_ok();
            s_call.state = CALL_UP;
            ESP_LOGI(TAG, "call answered by peer (%s:%u)",
                     s_call.remote_ip, s_call.remote_port);
            emit(SIP_EVT_ANSWERED);
        } else if (code >= 400) {
            ESP_LOGW(TAG, "outgoing call failed: %d", code);
            send_ack_failure(to);
            call_reset();
            emit(SIP_EVT_ENDED);
        }
        return;
    }
    /* Responses to BYE/CANCEL: nothing to do. */
}

/* ── Dispatch one complete message ────────────────────────────────────────── */

static void process_message(const char *msg)
{
    if (is_response(msg)) {
        handle_response(msg);
        return;
    }
    char method[16];
    request_method(msg, method, sizeof(method));
    ESP_LOGI(TAG, "<<< request %s", method);

    if (strcmp(method, "INVITE") == 0)      handle_invite(msg);
    else if (strcmp(method, "BYE") == 0)    handle_bye(msg);
    else if (strcmp(method, "CANCEL") == 0) handle_cancel(msg);
    else if (strcmp(method, "ACK") == 0)    { /* established; media already up */ }
    else if (strcmp(method, "OPTIONS") == 0) send_response(msg, 200, "OK", false, NULL);
    else send_response(msg, 200, "OK", false, NULL);
}

/* Pull complete messages out of the RX buffer (header scan + Content-Length). */
static void process_rx(void)
{
    while (s_rxlen > 0) {
        s_rxbuf[s_rxlen] = '\0';
        char *hdr_end = strstr(s_rxbuf, "\r\n\r\n");
        if (!hdr_end) {
            if (s_rxlen >= SIP_RX_BUF - 1) { s_rxlen = 0; }  /* overflow guard */
            return;
        }
        size_t hdr_len = (hdr_end - s_rxbuf) + 4;
        int clen = 0;
        char cl[16];
        /* Temporarily terminate headers for parsing. */
        char saved = s_rxbuf[hdr_len];
        s_rxbuf[hdr_len] = '\0';
        if (hdr_get(s_rxbuf, "Content-Length", cl, sizeof(cl)) ||
            hdr_get(s_rxbuf, "l", cl, sizeof(cl))) {
            clen = atoi(cl);
        }
        s_rxbuf[hdr_len] = saved;

        size_t total = hdr_len + clen;
        if (s_rxlen < total) return;   /* need more bytes */

        /* Isolate and process this message (off-stack scratch). */
        char *one = s_inbuf;
        size_t cp = total < (size_t)(SIP_MSG_BUF - 1) ? total : (size_t)(SIP_MSG_BUF - 1);
        memcpy(one, s_rxbuf, cp);
        one[cp] = '\0';
        process_message(one);

        memmove(s_rxbuf, s_rxbuf + total, s_rxlen - total);
        s_rxlen -= total;
    }
}

/* ── Connection lifecycle ─────────────────────────────────────────────────── */

static bool connect_tls(void)
{
    get_local_ip();
    esp_tls_cfg_t cfg = {
        .skip_common_name = true,      /* accept any cert (project convention) */
        .timeout_ms = 10000,
    };
    s_tls = esp_tls_init();
    if (!s_tls) return false;
    uint16_t port = s_cfg.port ? s_cfg.port : SIP_DEFAULT_PORT;
    int r = esp_tls_conn_new_sync(s_cfg.server, strlen(s_cfg.server), port, &cfg, s_tls);
    if (r != 1) {
        ESP_LOGW(TAG, "TLS connect to %s:%u failed (%d)", s_cfg.server, port, r);
        esp_tls_conn_destroy(s_tls);
        s_tls = NULL;
        return false;
    }
    /* Short read timeout so the loop can do timers/commands. */
    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(s_tls, &sockfd) == ESP_OK && sockfd >= 0) {
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    s_connected = true;
    s_rxlen = 0;
    ESP_LOGI(TAG, "TLS connected to %s:%u", s_cfg.server, port);
    return true;
}

static void disconnect_tls(void)
{
    if (s_tls) { esp_tls_conn_destroy(s_tls); s_tls = NULL; }
    s_connected = false;
    s_registered = false;
}

static void start_register(void)
{
    rand_hex(s_reg_fromtag, 8);
    char cid[24]; rand_hex(cid, 8);
    snprintf(s_reg_callid, sizeof(s_reg_callid), "%s@%s", cid, s_local_ip);
    s_reg_cseq = 0;
    s_auth_have = false;
    send_register(false);
}

/* ── Command handling (from other tasks) ──────────────────────────────────── */

static void do_call(const char *target)
{
    if (s_call.state != CALL_NONE) { ESP_LOGW(TAG, "already in a call"); return; }
    call_reset();
    s_call.outgoing = true;
    rand_hex(s_call.local_tag, 8);
    rand_hex(s_call.invite_branch, 10);
    char cid[24]; rand_hex(cid, 8);
    snprintf(s_call.call_id, sizeof(s_call.call_id), "%s@%s", cid, s_local_ip);
    s_call.cseq = 0;

    /* Build request URI + To. Accept bare extension or full sip: URI. */
    if (strncmp(target, "sip:", 4) == 0) {
        strlcpy(s_call.ruri, target, sizeof(s_call.ruri));
    } else {
        snprintf(s_call.ruri, sizeof(s_call.ruri), "sip:%s@%s", target, sip_domain());
    }
    snprintf(s_call.hdr_local, sizeof(s_call.hdr_local),
             "<sip:%s@%s>;tag=%s", s_cfg.user, sip_domain(), s_call.local_tag);
    snprintf(s_call.hdr_peer, sizeof(s_call.hdr_peer), "<%s>", s_call.ruri);
    strlcpy(s_call.peer_user, target, sizeof(s_call.peer_user));
    s_call.local_port = sip_rtp_pick_local_port();
    s_call.state = CALL_OUT_TRYING;
    s_auth_have = false;
    ESP_LOGI(TAG, "calling %s", s_call.ruri);
    send_invite(false);
}

static void do_hangup(void)
{
    switch (s_call.state) {
        case CALL_OUT_TRYING:
        case CALL_OUT_RINGING:
            send_cancel();
            call_reset();
            emit(SIP_EVT_ENDED);
            break;
        case CALL_IN_RINGING:
            /* Reject the still-ringing incoming INVITE. */
            {
                char *msg = s_msgbuf;
                char to_tagged[240];
                snprintf(to_tagged, sizeof(to_tagged), "%s;tag=%s",
                         s_call.in_to, s_call.local_tag);
                snprintf(msg, SIP_MSG_BUF,
                    "SIP/2.0 486 Busy Here\r\n%sFrom: %s\r\nTo: %s\r\nCall-ID: %s\r\n"
                    "CSeq: %s\r\nContent-Length: 0\r\n\r\n",
                    s_call.in_via, s_call.in_from, to_tagged, s_call.call_id,
                    s_call.in_cseq);
                sip_send(msg);
            }
            call_reset();
            emit(SIP_EVT_ENDED);
            break;
        case CALL_UP:
            send_bye();
            call_reset();
            emit(SIP_EVT_ENDED);
            break;
        default:
            break;
    }
}

static void process_commands(void)
{
    sip_cmd_msg_t cmd;
    while (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) {
        switch (cmd.kind) {
            case CMD_CALL:   do_call(cmd.arg); break;
            case CMD_ANSWER: answer_incoming(); break;
            case CMD_HANGUP: do_hangup(); break;
        }
    }
}

/* ── Main task ────────────────────────────────────────────────────────────── */

static void sip_task(void *arg)
{
    TickType_t backoff = pdMS_TO_TICKS(2000);
    while (s_running) {
        if (!s_connected) {
            if (!connect_tls()) {
                vTaskDelay(backoff);
                if (backoff < pdMS_TO_TICKS(30000)) backoff *= 2;
                continue;
            }
            backoff = pdMS_TO_TICKS(2000);
            start_register();
            s_next_keepalive = xTaskGetTickCount() + pdMS_TO_TICKS(SIP_KEEPALIVE_SEC * 1000);
        }

        /* Read available data. */
        int n = esp_tls_conn_read(s_tls, s_rxbuf + s_rxlen, SIP_RX_BUF - 1 - s_rxlen);
        if (n > 0) {
            s_rxlen += n;
            process_rx();
        } else if (n == 0 || n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            /* timeout / would-block — fall through to timers */
        } else {
            ESP_LOGW(TAG, "connection read error %d — reconnecting", n);
            disconnect_tls();
            continue;
        }

        process_commands();

        TickType_t now = xTaskGetTickCount();
        if (s_registered && (int32_t)(now - s_next_register) >= 0) {
            send_register(s_auth_have);
            s_next_register = now + pdMS_TO_TICKS((SIP_REG_EXPIRES / 2) * 1000);
        }
        if ((int32_t)(now - s_next_keepalive) >= 0) {
            tls_send("\r\n\r\n", 4);   /* CRLF keep-alive */
            s_next_keepalive = now + pdMS_TO_TICKS(SIP_KEEPALIVE_SEC * 1000);
        }
    }
    disconnect_tls();
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

esp_err_t sip_client_start(const sip_config_t *cfg, sip_event_cb_t cb, void *ctx)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (!cfg || !cfg->server[0] || !cfg->user[0]) return ESP_ERR_INVALID_ARG;

    s_cfg = *cfg;
    s_cb = cb;
    s_cb_ctx = ctx;
    call_reset();

    if (!s_cmd_q) {
        s_cmd_q = xQueueCreate(4, sizeof(sip_cmd_msg_t));
        if (!s_cmd_q) { ESP_LOGE(TAG, "cmd queue alloc failed"); return ESP_ERR_NO_MEM; }
    }
    if (!s_rxbuf) {
        s_rxbuf = heap_caps_malloc(SIP_RX_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_rxbuf) s_rxbuf = malloc(SIP_RX_BUF);
        if (!s_rxbuf) { ESP_LOGE(TAG, "rxbuf alloc failed"); return ESP_ERR_NO_MEM; }
    }
    if (!s_msgbuf) {
        s_msgbuf = heap_caps_malloc(SIP_MSG_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_msgbuf) s_msgbuf = malloc(SIP_MSG_BUF);
        if (!s_msgbuf) { ESP_LOGE(TAG, "msgbuf alloc failed"); return ESP_ERR_NO_MEM; }
    }
    if (!s_inbuf) {
        s_inbuf = heap_caps_malloc(SIP_MSG_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_inbuf) s_inbuf = malloc(SIP_MSG_BUF);
        if (!s_inbuf) { ESP_LOGE(TAG, "inbuf alloc failed"); return ESP_ERR_NO_MEM; }
    }
    s_rxlen = 0;

    s_running = true;
    /* 8 KB stack: enough for the TLS handshake and SIP-message building (large
     * snprintf scratch buffers live off-stack). The task stack must come from
     * internal SRAM, which is fragmented after BT+WiFi+TLS, so keep it modest. */
    BaseType_t ok = xTaskCreate(sip_task, "sip", 8192, NULL, 5, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "sip task create failed — internal free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "started for %s@%s", s_cfg.user, s_cfg.server);
    return ESP_OK;
}

void sip_client_stop(void)
{
    if (!s_running) return;
    s_running = false;
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
}

bool sip_is_registered(void) { return s_registered; }
bool sip_in_call(void)       { return s_call.state != CALL_NONE; }

esp_err_t sip_call(const char *uri_or_ext)
{
    if (!s_running || !s_cmd_q || !uri_or_ext) return ESP_ERR_INVALID_STATE;
    sip_cmd_msg_t cmd = { .kind = CMD_CALL };
    strlcpy(cmd.arg, uri_or_ext, sizeof(cmd.arg));
    return xQueueSend(s_cmd_q, &cmd, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

void sip_answer(void)
{
    if (!s_running || !s_cmd_q) return;
    sip_cmd_msg_t cmd = { .kind = CMD_ANSWER };
    xQueueSend(s_cmd_q, &cmd, 0);
}

void sip_hangup(void)
{
    if (!s_running || !s_cmd_q) return;
    sip_cmd_msg_t cmd = { .kind = CMD_HANGUP };
    xQueueSend(s_cmd_q, &cmd, 0);
}
