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

/* Pick an even local RTP port in the dynamic range. */
uint16_t sip_rtp_pick_local_port(void);

/* Bind a UDP socket to local_port, target remote_ip:remote_port. No task is
 * created — the caller pumps audio via sip_rtp_recv()/sip_rtp_send_frame(). */
esp_err_t sip_rtp_start(uint16_t local_port,
                        const char *remote_ip, uint16_t remote_port,
                        sip_codec_t codec);

/* Update the remote address — used when comedia learns a different source port. */
void sip_rtp_set_remote(const char *remote_ip, uint16_t remote_port);

/* Receive one RTP packet (blocks up to ~20 ms). Decodes to 8 kHz mono PCM into
 * pcm[], returns the number of samples (0 if none/timeout). */
size_t sip_rtp_recv(int16_t *pcm, size_t max_samples);

/* Encode and send one frame (160 samples, 8 kHz mono). */
void sip_rtp_send_frame(const int16_t *pcm, size_t samples);

/* Discard all queued inbound RTP without decoding (non-blocking). Call when
 * resuming RX after a talk period so a backlog doesn't flood the speaker. */
void sip_rtp_flush(void);

/* Drain all queued inbound RTP without decoding (non-blocking), but keep the
 * idle timer alive for valid G.711 audio packets. Call during talk mode so a
 * remote hangup (loss of far-end RTP) is detected even while transmitting. */
void sip_rtp_drain_rx(void);

/* Milliseconds since the last RTP packet was received (for hangup detection
 * when the peer/server doesn't deliver a BYE). Returns 0 when not running. */
uint32_t sip_rtp_idle_ms(void);
/* Reset the idle timer (call when resuming RX after a talk period). */
void sip_rtp_reset_idle(void);

void sip_rtp_stop(void);
