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

static uint8_t linear2alaw(int16_t pcm)
{
    int16_t sign = (~pcm >> 8) & 0x80;   /* a-law: sign bit set for positive */
    if (!sign) pcm = -pcm;
    if (pcm > G711_CLIP) pcm = G711_CLIP;
    uint8_t out;
    if (pcm >= 256) {
        int exponent = 7;
        for (int mask = 0x4000; (pcm & mask) == 0 && exponent > 0; mask >>= 1) {
            exponent--;
        }
        int mantissa = (pcm >> (exponent + 3)) & 0x0F;
        out = (uint8_t)((exponent << 4) | mantissa);
    } else {
        out = (uint8_t)(pcm >> 4);
    }
    return out ^ (sign ^ 0x55);
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
static TaskHandle_t        s_rx_task = NULL;
static sip_codec_t         s_codec = SIP_CODEC_PCMU;
static sip_rtp_rx_cb_t     s_rx_cb = NULL;
static void               *s_rx_ctx = NULL;
static SemaphoreHandle_t   s_remote_lock = NULL;
static struct sockaddr_in  s_remote;

/* TX RTP header counters (only touched by the sender). */
static uint16_t s_seq;
static uint32_t s_timestamp;
static uint32_t s_ssrc;

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

static void rtp_rx_task(void *arg)
{
    uint8_t  pkt[1500];
    int16_t  pcm[512];
    ESP_LOGI(TAG, "RX task running");
    while (s_running) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(s_sock, pkt, sizeof(pkt), 0,
                         (struct sockaddr *)&src, &slen);
        if (n < 0) {
            if (!s_running) break;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (n < 12) continue;   /* shorter than an RTP header */

        /* RTP header: skip 12 bytes + 4*CSRC count. Ignore extensions. */
        int csrc = pkt[0] & 0x0F;
        int hdr = 12 + csrc * 4;
        if (n <= hdr) continue;

        const uint8_t *payload = pkt + hdr;
        int plen = n - hdr;
        if (plen > (int)(sizeof(pcm) / sizeof(pcm[0]))) {
            plen = sizeof(pcm) / sizeof(pcm[0]);
        }
        if (s_codec == SIP_CODEC_PCMA) {
            for (int i = 0; i < plen; i++) pcm[i] = alaw2linear(payload[i]);
        } else {
            for (int i = 0; i < plen; i++) pcm[i] = ulaw2linear(payload[i]);
        }
        if (s_rx_cb) s_rx_cb(pcm, (size_t)plen, s_rx_ctx);
    }
    ESP_LOGI(TAG, "RX task exit");
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t sip_rtp_start(uint16_t local_port,
                        const char *remote_ip, uint16_t remote_port,
                        sip_codec_t codec,
                        sip_rtp_rx_cb_t rx_cb, void *ctx)
{
    if (s_running) sip_rtp_stop();

    if (!s_remote_lock) {
        s_remote_lock = xSemaphoreCreateMutex();
        if (!s_remote_lock) return ESP_ERR_NO_MEM;
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

    /* Short recv timeout so the RX task can observe s_running going false. */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_codec  = codec;
    s_rx_cb  = rx_cb;
    s_rx_ctx = ctx;
    s_seq       = (uint16_t)esp_random();
    s_timestamp = esp_random();
    s_ssrc      = esp_random();
    sip_rtp_set_remote(remote_ip, remote_port);

    s_running = true;
    BaseType_t ok = xTaskCreate(rtp_rx_task, "rtp_rx", 4096, NULL, 6, &s_rx_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "rtp_rx task create failed");
        s_running = false;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

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
    sendto(s_sock, pkt, 12 + samples, 0,
           (struct sockaddr *)&dst, sizeof(dst));
}

void sip_rtp_stop(void)
{
    if (!s_running && s_sock < 0) return;
    s_running = false;

    /* Wait for the RX task to drain (recv timeout is 200 ms). */
    for (int i = 0; i < 20 && s_rx_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    s_rx_cb = NULL;
    s_rx_ctx = NULL;
    ESP_LOGI(TAG, "RTP stopped");
}
