#pragma once

/*
 * Minimal RTP transport + G.711 codec for the Core2 SIP speakerphone.
 *
 * Carries 8 kHz mono PCM in 20 ms frames (160 samples) over UDP using
 * G.711 µ-law (payload type 0) or a-law (payload type 8). Symmetric RTP:
 * we send from the same local port we advertise in SDP so a NAT-aware server
 * (Asterisk comedia) learns where to return media.
 */

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SIP_RTP_FRAME_SAMPLES 160   /* 20 ms @ 8 kHz mono */
#define SIP_RTP_SAMPLE_RATE   8000

typedef enum {
    SIP_CODEC_PCMU = 0,   /* G.711 µ-law, RTP payload type 0 */
    SIP_CODEC_PCMA = 8,   /* G.711 a-law, RTP payload type 8 */
} sip_codec_t;

/* Invoked from the RX task with one decoded frame of 8 kHz mono PCM. */
typedef void (*sip_rtp_rx_cb_t)(const int16_t *pcm, size_t samples, void *ctx);

/* Pick an even local RTP port in the dynamic range. */
uint16_t sip_rtp_pick_local_port(void);

/* Bind a UDP socket to local_port, target remote_ip:remote_port, spawn RX task. */
esp_err_t sip_rtp_start(uint16_t local_port,
                        const char *remote_ip, uint16_t remote_port,
                        sip_codec_t codec,
                        sip_rtp_rx_cb_t rx_cb, void *ctx);

/* Update the remote address — used when comedia learns a different source port. */
void sip_rtp_set_remote(const char *remote_ip, uint16_t remote_port);

/* Encode and send one frame (160 samples, 8 kHz mono). Safe to call from a
 * different task than the RX task. */
void sip_rtp_send_frame(const int16_t *pcm, size_t samples);

void sip_rtp_stop(void);
