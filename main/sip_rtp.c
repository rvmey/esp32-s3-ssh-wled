#include "sip_rtp.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "sip_rtp";

/* ── G.711 codec ─────────────────────────────────────────────────────────────
 * Classic Sun reference implementation (per-sample; ~8000 ops/s — negligible). */

#define G711_BIAS 0x84
#define G711_CLIP 32635

static uint8_t linear2ulaw(int16_t pcm)
{
    static const uint8_t exp_lut[256] = {
        0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,
        4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
        5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
        5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
        6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
        6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
        6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
        6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7 };
    int16_t sign = (pcm >> 8) & 0x80;
    if (sign) pcm = -pcm;
    if (pcm > G711_CLIP) pcm = G711_CLIP;
    pcm += G711_BIAS;
    int exponent = exp_lut[(pcm >> 7) & 0xFF];
    int mantissa = (pcm >> (exponent + 3)) & 0x0F;
    return (uint8_t)(~(sign | (exponent << 4) | mantissa));
}

static int16_t ulaw2linear(uint8_t u)
{
    u = ~u;
    int t = ((u & 0x0F) << 3) + G711_BIAS;
    t <<= (u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? (G711_BIAS - t) : (t - G711_BIAS));
}

static uint8_t linear2alaw(int16_t pcm16)
{
    /* Canonical G.711 A-law encoder. A-law operates on a 13-bit magnitude, so
     * the 16-bit sample must be shifted down by 3 first (the missing >>3 was
     * making loud audio encode to garbage). */
    static const int seg_end[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
    int pcm = pcm16 >> 3;
    int mask;
    if (pcm >= 0) {
        mask = 0xD5;            /* sign bit = 1 for positive */
    } else {
        mask = 0x55;
        pcm = -pcm - 1;
    }
    int seg = 8;
    for (int i = 0; i < 8; i++) { if (pcm <= seg_end[i]) { seg = i; break; } }
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);   /* out of range → max */
    uint8_t aval = (uint8_t)(seg << 4);
    if (seg < 2) aval |= (pcm >> 1) & 0x0F;
    else         aval |= (pcm >> seg) & 0x0F;
    return aval ^ (uint8_t)mask;
}

static int16_t alaw2linear(uint8_t a)
{
    a ^= 0x55;
    int sign = a & 0x80;
    int exponent = (a & 0x70) >> 4;
    int mantissa = a & 0x0F;
    int sample = (mantissa << 4) + 8;
    if (exponent != 0) {
        sample += 0x100;
        sample <<= (exponent - 1);
    }
    return (int16_t)(sign ? sample : -sample);
}

/* ── Session state ───────────────────────────────────────────────────────── */

static int                 s_sock = -1;
static volatile bool       s_running = false;
static sip_codec_t         s_codec = SIP_CODEC_PCMU;
static uint8_t            *s_rxpkt = NULL;   /* PSRAM recv buffer */
static SemaphoreHandle_t   s_remote_lock = NULL;
static struct sockaddr_in  s_remote;

#define RTP_RXPKT_MAX 1500

/* TX RTP header counters (only touched by the sender). */
static uint16_t s_seq;
static uint32_t s_timestamp;
static uint32_t s_ssrc;

/* Diagnostics: packet counts for this session. */
static uint32_t s_rx_pkts;
static uint32_t s_tx_pkts;
static TickType_t s_last_rx_tick;   /* updated on each received packet */

uint16_t sip_rtp_pick_local_port(void)
{
    /* Even port in 16384..32766 (RTP convention: even = media). */
    return (uint16_t)(16384 + ((esp_random() % 8192) * 2));
}

void sip_rtp_set_remote(const char *remote_ip, uint16_t remote_port)
{
    if (!s_remote_lock) return;
    xSemaphoreTake(s_remote_lock, portMAX_DELAY);
    memset(&s_remote, 0, sizeof(s_remote));
    s_remote.sin_family = AF_INET;
    s_remote.sin_port   = htons(remote_port);
    s_remote.sin_addr.s_addr = inet_addr(remote_ip);
    xSemaphoreGive(s_remote_lock);
    ESP_LOGI(TAG, "remote set to %s:%u", remote_ip, remote_port);
}

size_t sip_rtp_recv(int16_t *pcm, size_t max_samples)
{
    if (!s_running || s_sock < 0 || !s_rxpkt || !pcm) return 0;

    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    int n = recvfrom(s_sock, s_rxpkt, RTP_RXPKT_MAX, 0,
                     (struct sockaddr *)&src, &slen);
    if (n < 12) return 0;   /* timeout / error / shorter than RTP header */

    /* Drop non-audio packets that share this UDP port: ICE/STUN connectivity
     * probes and RTCP reports. linphone uses ICE, so these arrive constantly;
     * decoding them as G.711 produces loud static. Require RTP version 2 and a
     * G.711 payload type (PCMU 0 / PCMA 8) — STUN first byte is 0x00/0x01, RTCP
     * payload types are 200-204. */
    if ((s_rxpkt[0] & 0xC0) != 0x80) return 0;
    uint8_t pt = s_rxpkt[1] & 0x7F;
    if (pt != SIP_CODEC_PCMU && pt != SIP_CODEC_PCMA) return 0;

    /* RTP header: skip 12 bytes + 4*CSRC count. Ignore extensions. */
    int csrc = s_rxpkt[0] & 0x0F;
    int hdr = 12 + csrc * 4;
    if (n <= hdr) return 0;

    const uint8_t *payload = s_rxpkt + hdr;
    int plen = n - hdr;
    if (plen > (int)max_samples) plen = (int)max_samples;
    /* Decode by the packet's actual payload type, not the negotiated codec, so
     * a codec mismatch can't turn into static. */
    if (pt == SIP_CODEC_PCMA) {
        for (int i = 0; i < plen; i++) pcm[i] = alaw2linear(payload[i]);
    } else {
        for (int i = 0; i < plen; i++) pcm[i] = ulaw2linear(payload[i]);
    }
    s_rx_pkts++;
    s_last_rx_tick = xTaskGetTickCount();
    if (s_rx_pkts == 1) {
        ESP_LOGI(TAG, "first RTP RX from %s:%u (%d samples)",
                 inet_ntoa(src.sin_addr), ntohs(src.sin_port), plen);
    } else if (s_rx_pkts % 100 == 0) {
        ESP_LOGI(TAG, "RTP rx=%lu tx=%lu",
                 (unsigned long)s_rx_pkts, (unsigned long)s_tx_pkts);
    }
    return (size_t)plen;
}

esp_err_t sip_rtp_start(uint16_t local_port,
                        const char *remote_ip, uint16_t remote_port,
                        sip_codec_t codec)
{
    if (s_running) sip_rtp_stop();

    if (!s_remote_lock) {
        s_remote_lock = xSemaphoreCreateMutex();
        if (!s_remote_lock) return ESP_ERR_NO_MEM;
    }
    if (!s_rxpkt) {
        s_rxpkt = heap_caps_malloc(RTP_RXPKT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_rxpkt) s_rxpkt = malloc(RTP_RXPKT_MAX);
        if (!s_rxpkt) { ESP_LOGE(TAG, "rxpkt alloc failed"); return ESP_ERR_NO_MEM; }
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        return ESP_FAIL;
    }

    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port   = htons(local_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind(%u) failed: errno %d", local_port, errno);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    /* ~20 ms recv timeout: sip_rtp_recv() paces the media task in listen mode. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_codec  = codec;
    s_seq       = (uint16_t)esp_random();
    s_timestamp = esp_random();
    s_ssrc      = esp_random();
    s_rx_pkts   = 0;
    s_tx_pkts   = 0;
    s_last_rx_tick = xTaskGetTickCount();
    sip_rtp_set_remote(remote_ip, remote_port);
    s_running = true;

    ESP_LOGI(TAG, "RTP started: local=%u remote=%s:%u codec=%s",
             local_port, remote_ip, remote_port,
             codec == SIP_CODEC_PCMA ? "PCMA" : "PCMU");
    return ESP_OK;
}

void sip_rtp_send_frame(const int16_t *pcm, size_t samples)
{
    if (!s_running || s_sock < 0 || !pcm) return;
    if (samples > SIP_RTP_FRAME_SAMPLES) samples = SIP_RTP_FRAME_SAMPLES;

    uint8_t pkt[12 + SIP_RTP_FRAME_SAMPLES];
    pkt[0] = 0x80;                       /* V=2, no padding/extension/CSRC */
    pkt[1] = (uint8_t)s_codec;           /* M=0, PT=codec */
    pkt[2] = (uint8_t)(s_seq >> 8);
    pkt[3] = (uint8_t)(s_seq & 0xFF);
    pkt[4] = (uint8_t)(s_timestamp >> 24);
    pkt[5] = (uint8_t)(s_timestamp >> 16);
    pkt[6] = (uint8_t)(s_timestamp >> 8);
    pkt[7] = (uint8_t)(s_timestamp & 0xFF);
    pkt[8]  = (uint8_t)(s_ssrc >> 24);
    pkt[9]  = (uint8_t)(s_ssrc >> 16);
    pkt[10] = (uint8_t)(s_ssrc >> 8);
    pkt[11] = (uint8_t)(s_ssrc & 0xFF);

    if (s_codec == SIP_CODEC_PCMA) {
        for (size_t i = 0; i < samples; i++) pkt[12 + i] = linear2alaw(pcm[i]);
    } else {
        for (size_t i = 0; i < samples; i++) pkt[12 + i] = linear2ulaw(pcm[i]);
    }

    s_seq++;
    s_timestamp += SIP_RTP_FRAME_SAMPLES;   /* one timestamp tick per sample */

    struct sockaddr_in dst;
    xSemaphoreTake(s_remote_lock, portMAX_DELAY);
    dst = s_remote;
    xSemaphoreGive(s_remote_lock);

    if (dst.sin_addr.s_addr == 0 || dst.sin_addr.s_addr == INADDR_NONE) return;
    int sent = sendto(s_sock, pkt, 12 + samples, 0,
                      (struct sockaddr *)&dst, sizeof(dst));
    s_tx_pkts++;
    if (s_tx_pkts == 1) {
        ESP_LOGI(TAG, "first RTP TX to %s:%u (sent=%d errno=%d)",
                 inet_ntoa(dst.sin_addr), ntohs(dst.sin_port), sent, errno);
    }
}

void sip_rtp_flush(void)
{
    if (!s_running || s_sock < 0) return;
    uint8_t tmp[64];
    int guard = 0;
    /* MSG_DONTWAIT: drain every queued datagram, then stop on EWOULDBLOCK.
     * recvfrom consumes one whole UDP datagram per call even if truncated. */
    while (recvfrom(s_sock, tmp, sizeof(tmp), MSG_DONTWAIT, NULL, NULL) > 0) {
        if (++guard > 1000) break;
    }
    s_last_rx_tick = xTaskGetTickCount();
}

void sip_rtp_drain_rx(void)
{
    if (!s_running || s_sock < 0 || !s_rxpkt) return;
    /* Non-blocking: consume every queued datagram. Update the idle timer only
     * for valid G.711 audio RTP (same filter as sip_rtp_recv) so far-end audio
     * keeps the call alive during talk, while STUN/RTCP noise is ignored. The
     * decoded audio is discarded — talk mode plays the mic, not the far end. */
    int guard = 0;
    for (;;) {
        int n = recvfrom(s_sock, s_rxpkt, RTP_RXPKT_MAX, MSG_DONTWAIT, NULL, NULL);
        if (n < 12) break;                       /* EWOULDBLOCK / too short */
        if (++guard > 1000) break;
        if ((s_rxpkt[0] & 0xC0) != 0x80) continue;   /* not RTP v2 */
        uint8_t pt = s_rxpkt[1] & 0x7F;
        if (pt != SIP_CODEC_PCMU && pt != SIP_CODEC_PCMA) continue;
        s_rx_pkts++;
        s_last_rx_tick = xTaskGetTickCount();
    }
}

uint32_t sip_rtp_idle_ms(void)
{
    if (!s_running) return 0;
    return (uint32_t)((xTaskGetTickCount() - s_last_rx_tick) * portTICK_PERIOD_MS);
}

void sip_rtp_reset_idle(void)
{
    s_last_rx_tick = xTaskGetTickCount();
}

void sip_rtp_stop(void)
{
    if (!s_running && s_sock < 0) return;
    s_running = false;
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    ESP_LOGI(TAG, "RTP stopped");
}
