#pragma once

/*
 * Minimal SIP UAC (RFC 3261) for the Core2 speakerphone.
 *
 * Signaling runs over a single persistent TCP/TLS connection (SIPS, default
 * port 5061) reusing esp-tls in the project's insecure no-verify mode (accepts
 * any server cert). One account, one call at a time. Media is handled by
 * sip_rtp.c; this module negotiates the codec/ports via SDP and reports call
 * events through a callback.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sip_rtp.h"

typedef struct {
    char     server[64];    /* registrar/proxy host */
    uint16_t port;          /* TLS port (0 → default 5061) */
    char     user[48];      /* auth username / extension */
    char     password[64];
    char     domain[64];    /* SIP domain for URIs (empty → use server) */
} sip_config_t;

typedef enum {
    SIP_EVT_REGISTERED,       /* successful REGISTER */
    SIP_EVT_REGISTER_FAILED,  /* REGISTER rejected / unreachable */
    SIP_EVT_INCOMING,         /* incoming INVITE (caller + media params set) */
    SIP_EVT_RINGING,          /* 180/183 for our outgoing call */
    SIP_EVT_ANSWERED,         /* outgoing call answered (media params set) */
    SIP_EVT_ENDED,            /* call ended (BYE/CANCEL/decline/local hangup) */
} sip_event_t;

typedef struct {
    sip_event_t type;
    char        caller[96];        /* user part / display of remote party */
    char        remote_ip[40];     /* negotiated remote RTP IP (from SDP) */
    uint16_t    remote_rtp_port;   /* negotiated remote RTP port */
    uint16_t    local_rtp_port;    /* local RTP port we advertised */
    sip_codec_t codec;             /* negotiated codec */
} sip_event_info_t;

typedef void (*sip_event_cb_t)(const sip_event_info_t *ev, void *ctx);

/* Start the SIP task: connect, register, and service the account. */
esp_err_t sip_client_start(const sip_config_t *cfg, sip_event_cb_t cb, void *ctx);
void      sip_client_stop(void);
bool      sip_is_registered(void);
bool      sip_in_call(void);

/* Place an outgoing call to an extension or full "sip:user@host" URI. */
esp_err_t sip_call(const char *uri_or_ext);
/* Answer the current incoming call (sends 200 OK). */
void      sip_answer(void);
/* Reject an incoming call, or hang up an active/outgoing call. */
void      sip_hangup(void);
