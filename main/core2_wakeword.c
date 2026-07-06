#include "core2_wakeword.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "core2_audio.h"
#include "core2_mic.h"

/* Provided by picture_frame.c — the gate aggregates all "someone needs the
 * speaker or mic" conditions (SIP, music/TTS, voice query/note in flight). */
extern bool pf_wakeword_gate_open(void);
extern void pf_signal_voice_query(void);

static const char *TAG = "core2_ww";

/* Gate must stay open this long before grabbing the mic, so back-to-back
 * audio activity (e.g. TTS reply right after a query) doesn't thrash the
 * GPIO0/I2S handoff, and so a just-emptied speaker ring can finish its last
 * DMA chunk before the channel is torn down. */
#define WW_GATE_SETTLE_MS      700
#define WW_IDLE_POLL_MS        150
#define WW_ARM_FAIL_BACKOFF_MS 5000
#define WW_TASK_STACK          12288
#define WW_TASK_PRIO           4      /* below mp3 (5) and i2s writer (6) */
#define WW_READ_STEP           320    /* matches SIP_RTP_FRAME_SAMPLES*2 — see read loop comment */
/* Confirmed on hardware: short runs of a few consecutive ESP_ERR_TIMEOUT
 * reads (no new DMA data within 60 ms) are NORMAL jitter and self-recover.
 * But the I2S RX channel also genuinely dies outright at unpredictable
 * points (anywhere from ~100ms to several seconds into a session) — after
 * which every read fails instantly (not a real 60ms timeout) — confirmed by
 * measuring elapsed time from "armed" to "stalled" match count*~1ms, not
 * count*60ms. WW_SOFT_RECOVER_LIMIT catches that quickly and tries a cheap
 * fix (disable+enable the SAME channel — no DMA alloc, doesn't touch the
 * speaker at all). Only if that doesn't help does WW_ZERO_READ_STALL_LIMIT
 * escalate to the expensive full teardown+recreate (which was previously
 * happening on every single stall, and repeatedly bouncing the speaker's
 * channel alongside it fragmented the scarce internal DMA-capable heap
 * badly enough to break unrelated features — observed i2s_alloc_dma_desc
 * failures, then SIP client startup failing with ESP_ERR_NO_MEM). */
#define WW_SOFT_RECOVER_LIMIT    50
#define WW_ZERO_READ_STALL_LIMIT 300

static const esp_wn_iface_t *s_wn = NULL;
static model_iface_data_t   *s_wn_data = NULL;
static srmodel_list_t       *s_models = NULL;
static int      s_chunk = 0;          /* samples per detect() call */
static int16_t *s_buf = NULL;         /* one detect() chunk */
static TaskHandle_t   s_task = NULL;
static volatile bool  s_enabled = true;
static volatile bool  s_armed = false;
static volatile bool  s_yield_req = false;

/* Release the mic and restore the speaker DMA. Task context only. */
static void ww_disarm(void)
{
    if (!s_armed) return;
    core2_mic_stream_stop();
    core2_mic_deinit();
    core2_audio_acquire_dma();
    s_armed = false;
    ESP_LOGI(TAG, "disarmed (mic released, speaker restored)");
}

/* Free the speaker DMA (GPIO 0) and take the mic — same handoff the SIP talk
 * toggle uses. Task context only. */
static bool ww_arm(void)
{
    core2_audio_release_dma();
    if (core2_mic_init() != ESP_OK) {
        ESP_LOGW(TAG, "arm: mic init failed (DMA alloc)");
        core2_audio_acquire_dma();
        return false;
    }
    if (core2_mic_stream_start() != ESP_OK) {
        core2_mic_deinit();
        core2_audio_acquire_dma();
        return false;
    }
    /* NOT calling s_wn->clean(s_wn_data) here: on this prebuilt wn9_hiesp
     * binary it crashes (LoadProhibited in dl_convq_queue_bzero) when called
     * before the model's internal conv-queue has been allocated by a first
     * detect() call — which never happens on a freshly created detector, i.e.
     * exactly this re-arm path. A few stale sliding-window frames from the
     * previous listening session at the very start of a new one are harmless
     * (core2_mic_stream_start() already flushes stale I2S DMA frames). */
    s_armed = true;
    ESP_LOGI(TAG, "armed (listening for wake word)");
    return true;
}

/* Never touches NVS/flash writes — required because the stack may live in
 * PSRAM (same constraint as pf_backup_task). Model reads are mmap'd flash
 * cache, which is fine. */
static void ww_task(void *arg)
{
    (void)arg;
    TickType_t gate_open_since = 0;
    size_t fill = 0;
    int zero_reads = 0;

    for (;;) {
        bool want = s_enabled && !s_yield_req && pf_wakeword_gate_open();

        if (!want) {
            if (s_armed) { ww_disarm(); fill = 0; }
            gate_open_since = 0;
            vTaskDelay(pdMS_TO_TICKS(WW_IDLE_POLL_MS));
            continue;
        }

        if (!s_armed) {
            TickType_t now = xTaskGetTickCount();
            if (gate_open_since == 0) gate_open_since = now;
            if ((TickType_t)(now - gate_open_since) <
                pdMS_TO_TICKS(WW_GATE_SETTLE_MS)) {
                vTaskDelay(pdMS_TO_TICKS(WW_IDLE_POLL_MS));
                continue;
            }
            fill = 0;
            zero_reads = 0;
            if (!ww_arm()) {
                gate_open_since = 0;
                vTaskDelay(pdMS_TO_TICKS(WW_ARM_FAIL_BACKOFF_MS));
                continue;
            }
        }

        /* read_frame blocks ≤60 ms and may return a partial chunk. Cap each
         * request at WW_READ_STEP (the same size the proven SIP RTP path
         * uses, SIP_RTP_FRAME_SAMPLES*2) rather than requesting the model's
         * full 512-sample chunk in one call. Accumulates into s_buf via
         * `fill` either way. */
        size_t remaining = (size_t)s_chunk - fill;
        size_t read_req = remaining < WW_READ_STEP ? remaining : WW_READ_STEP;
        size_t n = core2_mic_read_frame(s_buf + fill, read_req);
        if (n == 0) {
            zero_reads++;
            if (zero_reads == WW_SOFT_RECOVER_LIMIT) {
                /* Cheap first attempt: reset the same channel object without
                 * touching the speaker or doing any DMA allocation. */
                ESP_LOGW(TAG, "mic read stalled, soft-recovering (same channel)");
                core2_mic_stream_stop();
                core2_mic_stream_start();
                fill = 0;
            } else if (zero_reads > WW_ZERO_READ_STALL_LIMIT) {
                ESP_LOGW(TAG, "mic stream stalled, full re-arm");
                ww_disarm();
                gate_open_since = 0;
                vTaskDelay(pdMS_TO_TICKS(WW_ARM_FAIL_BACKOFF_MS));
            }
            continue;
        }
        zero_reads = 0;
        fill += n;
        if (fill < (size_t)s_chunk) continue;
        fill = 0;

        if (s_wn->detect(s_wn_data, s_buf) == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "wake word detected — starting voice query");
            ww_disarm();
            gate_open_since = 0;
            /* Same convergence point as the PWR key and the "listen" command;
             * the main loop consumes the flag and runs do_core2_voice_query().
             * The pending flag also closes the gate so we don't re-arm while
             * the query is in flight. */
            pf_signal_voice_query();
        }
    }
}

esp_err_t core2_wakeword_init(void)
{
    if (s_task) return ESP_OK;

    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "no models found in 'model' partition");
        return ESP_FAIL;
    }
    char *name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    if (!name) {
        ESP_LOGE(TAG, "no wakenet model in 'model' partition");
        return ESP_FAIL;
    }
    s_wn = esp_wn_handle_from_name(name);
    if (!s_wn) {
        ESP_LOGE(TAG, "no wakenet iface for %s", name);
        return ESP_FAIL;
    }
    /* One-time (boot only) measurement: how much internal RAM does the
     * closed-source WakeNet create() call actually cost? SIP client startup
     * has been failing with ESP_ERR_NO_MEM even before the first arm cycle,
     * so this permanent footprint — not just the arm/disarm churn — may be
     * the dominant contributor. */
    size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_wn_data = s_wn->create(name, DET_MODE_90);
    size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "wakenet create() internal RAM cost: %d bytes (before=%u after=%u)",
             (int)internal_before - (int)internal_after,
             (unsigned)internal_before, (unsigned)internal_after);
    if (!s_wn_data) {
        ESP_LOGE(TAG, "wakenet create failed for %s", name);
        return ESP_FAIL;
    }
    s_chunk = s_wn->get_samp_chunksize(s_wn_data);
    int rate = s_wn->get_samp_rate(s_wn_data);
    if (s_chunk <= 0 || rate != 16000) {   /* PDM mic is fixed at 16 kHz */
        ESP_LOGE(TAG, "unusable model: chunk=%d rate=%d", s_chunk, rate);
        s_wn->destroy(s_wn_data);
        s_wn_data = NULL;
        return ESP_FAIL;
    }
    s_buf = heap_caps_malloc((size_t)s_chunk * sizeof(int16_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) s_buf = malloc((size_t)s_chunk * sizeof(int16_t));
    if (!s_buf) {
        s_wn->destroy(s_wn_data);
        s_wn_data = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Stack in PSRAM, internal fallback — same rationale as pf_backup_task.
     * The task never exits, so the WithCaps teardown rule never applies.
     * Pinned to core 0: every other consumer of I2S/GPIO0 (main task, mp3
     * task, SIP media pump) runs there ("main_task: Started on CPU0" at
     * boot), and xTaskCreateWithCaps defaults to tskNO_AFFINITY — on
     * hardware this let the task land on core 1, and the mic's I2S channel
     * would come up "enabled" (create/init/enable all report ESP_OK, "PDM RX
     * ready" logs) but every subsequent read failed instantly, and disable()
     * later reported "the channel has not been enabled yet" — consistent
     * with the I2S driver/GPIO-matrix state not being safely shared across
     * cores here. Confirmed the crash before the v2.0.597 fix was also on
     * core 1. */
    if (xTaskCreatePinnedToCoreWithCaps(ww_task, "core2_ww", WW_TASK_STACK, NULL,
                            WW_TASK_PRIO, &s_task, 0,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS &&
        xTaskCreatePinnedToCoreWithCaps(ww_task, "core2_ww", WW_TASK_STACK, NULL,
                            WW_TASK_PRIO, &s_task, 0,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "failed to create task");
        free(s_buf);
        s_buf = NULL;
        s_wn->destroy(s_wn_data);
        s_wn_data = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ready: model=%s word=%s chunk=%d",
             name, esp_wn_wakeword_from_name(name), s_chunk);
    return ESP_OK;
}

void core2_wakeword_set_enabled(bool enabled)
{
    s_enabled = enabled;
    ESP_LOGI(TAG, "%s", enabled ? "enabled" : "disabled");
    /* The task notices on its next poll (≤150 ms) and disarms if needed. */
}

bool core2_wakeword_get_enabled(void)
{
    return s_enabled;
}

bool core2_wakeword_armed(void)
{
    return s_armed;
}

void core2_wakeword_yield(void)
{
    if (!s_task || !s_armed) return;
    s_yield_req = true;
    /* ww_arm() takes ~150 ms worst case before the task re-checks the flag,
     * so allow generous headroom. */
    for (int i = 0; i < 200 && s_armed; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_yield_req = false;
    if (s_armed) ESP_LOGW(TAG, "yield timed out — task still armed");
}
