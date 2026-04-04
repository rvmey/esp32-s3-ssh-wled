#include "ride_log.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "ride_log";

#define BIKE_NVS_PARTITION  "bike_nvs"
#define NVS_NAMESPACE       "bike_rides"
#define KEY_RIDE_COUNT      "ride_count"

/* RAM accumulation buffer: flush to NVS when full or on ride_log_finish().  */
#define BUF_CAPACITY  512

static track_point_t *s_buf      = NULL;
static size_t         s_buf_n    = 0;
static uint32_t       s_ride_idx = 0;
static bool           s_open     = false;

/* ── Key helpers ─────────────────────────────────────────────────────────── */

static void make_key(uint32_t idx, char *key, size_t ksize)
{
    snprintf(key, ksize, "ride%04" PRIu32, idx);   /* e.g. "ride0042" */
}

static void make_ts_key(uint32_t idx, char *key, size_t ksize)
{
    snprintf(key, ksize, "ride%04" PRIu32 "_t", idx);  /* e.g. "ride0042_t" */
}

/* ── NVS flush ───────────────────────────────────────────────────────────── */

static esp_err_t flush_buffer(void)
{
    if (s_buf_n == 0) return ESP_OK;

    nvs_handle_t hdl;
    esp_err_t ret = nvs_open_from_partition(BIKE_NVS_PARTITION, NVS_NAMESPACE,
                                            NVS_READWRITE, &hdl);
    if (ret != ESP_OK) return ret;

    char key[16];
    make_key(s_ride_idx, key, sizeof(key));

    /* Read any previously flushed blob, append new points, write back.     */
    size_t existing_sz = 0;
    nvs_get_blob(hdl, key, NULL, &existing_sz);  /* get current size */

    size_t total_pts = existing_sz / sizeof(track_point_t) + s_buf_n;
    size_t total_sz  = total_pts * sizeof(track_point_t);

    track_point_t *combined = malloc(total_sz);
    if (!combined) {
        nvs_close(hdl);
        return ESP_ERR_NO_MEM;
    }

    if (existing_sz > 0) {
        nvs_get_blob(hdl, key, combined, &existing_sz);
    }
    memcpy(combined + existing_sz / sizeof(track_point_t), s_buf,
           s_buf_n * sizeof(track_point_t));

    ret = nvs_set_blob(hdl, key, combined, total_sz);
    free(combined);

    if (ret == ESP_OK) ret = nvs_commit(hdl);
    nvs_close(hdl);

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Flushed %zu points for ride %" PRIu32
                      " (total %" PRIu32 ")",
                 s_buf_n, s_ride_idx, (uint32_t)total_pts);
        s_buf_n = 0;
    }
    return ret;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t ride_log_init(void)
{
    esp_err_t ret = nvs_flash_init_partition(BIKE_NVS_PARTITION);
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(BIKE_NVS_PARTITION);
        ret = nvs_flash_init_partition(BIKE_NVS_PARTITION);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bike_nvs init failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "bike_nvs partition ready");
    }
    return ret;
}

esp_err_t ride_log_start(uint32_t start_ts)
{
    if (s_open) return ESP_ERR_INVALID_STATE;

    s_buf = malloc(BUF_CAPACITY * sizeof(track_point_t));
    if (!s_buf) return ESP_ERR_NO_MEM;
    s_buf_n = 0;
    s_open  = true;

    /* Determine index = current ride_count, then increment the counter.    */
    nvs_handle_t hdl;
    esp_err_t ret = nvs_open_from_partition(BIKE_NVS_PARTITION, NVS_NAMESPACE,
                                            NVS_READWRITE, &hdl);
    if (ret != ESP_OK) { free(s_buf); s_buf = NULL; return ret; }

    uint32_t count = 0;
    nvs_get_u32(hdl, KEY_RIDE_COUNT, &count);
    s_ride_idx = count;

    /* Store start timestamp */
    char ts_key[16];
    make_ts_key(s_ride_idx, ts_key, sizeof(ts_key));
    nvs_set_u32(hdl, ts_key, start_ts);

    /* Increment count */
    nvs_set_u32(hdl, KEY_RIDE_COUNT, count + 1);
    ret = nvs_commit(hdl);
    nvs_close(hdl);

    ESP_LOGI(TAG, "Ride %" PRIu32 " started (ts=%" PRIu32 ")",
             s_ride_idx, start_ts);
    return ret;
}

esp_err_t ride_log_append(const track_point_t *pt)
{
    if (!s_open || !s_buf) return ESP_ERR_INVALID_STATE;

    s_buf[s_buf_n++] = *pt;

    if (s_buf_n >= BUF_CAPACITY) {
        return flush_buffer();
    }
    return ESP_OK;
}

uint32_t ride_log_finish(void)
{
    if (!s_open) return UINT32_MAX;

    flush_buffer();   /* ignore error — best effort */

    free(s_buf);
    s_buf  = NULL;
    s_buf_n = 0;
    s_open  = false;

    ESP_LOGI(TAG, "Ride %" PRIu32 " finished", s_ride_idx);
    return s_ride_idx;
}

esp_err_t ride_log_count(uint32_t *count)
{
    nvs_handle_t hdl;
    esp_err_t ret = nvs_open_from_partition(BIKE_NVS_PARTITION, NVS_NAMESPACE,
                                            NVS_READONLY, &hdl);
    if (ret != ESP_OK) return ret;

    *count = 0;
    ret = nvs_get_u32(hdl, KEY_RIDE_COUNT, count);
    nvs_close(hdl);
    if (ret == ESP_ERR_NVS_NOT_FOUND) { *count = 0; ret = ESP_OK; }
    return ret;
}

esp_err_t ride_log_read(uint32_t idx, track_point_t **pts,
                        size_t *n_pts, uint32_t *start_ts)
{
    nvs_handle_t hdl;
    esp_err_t ret = nvs_open_from_partition(BIKE_NVS_PARTITION, NVS_NAMESPACE,
                                            NVS_READONLY, &hdl);
    if (ret != ESP_OK) return ret;

    char key[16], ts_key[16];
    make_key(idx, key, sizeof(key));
    make_ts_key(idx, ts_key, sizeof(ts_key));

    *start_ts = 0;
    nvs_get_u32(hdl, ts_key, start_ts);

    size_t sz = 0;
    ret = nvs_get_blob(hdl, key, NULL, &sz);
    if (ret != ESP_OK) { nvs_close(hdl); return ret; }

    *pts = malloc(sz);
    if (!*pts) { nvs_close(hdl); return ESP_ERR_NO_MEM; }

    ret = nvs_get_blob(hdl, key, *pts, &sz);
    nvs_close(hdl);

    if (ret == ESP_OK) {
        *n_pts = sz / sizeof(track_point_t);
    } else {
        free(*pts);
        *pts = NULL;
        *n_pts = 0;
    }
    return ret;
}

esp_err_t ride_log_delete(uint32_t idx)
{
    nvs_handle_t hdl;
    esp_err_t ret = nvs_open_from_partition(BIKE_NVS_PARTITION, NVS_NAMESPACE,
                                            NVS_READWRITE, &hdl);
    if (ret != ESP_OK) return ret;

    char key[16], ts_key[16];
    make_key(idx, key, sizeof(key));
    make_ts_key(idx, ts_key, sizeof(ts_key));

    nvs_erase_key(hdl, key);
    nvs_erase_key(hdl, ts_key);
    ret = nvs_commit(hdl);
    nvs_close(hdl);

    ESP_LOGI(TAG, "Ride %" PRIu32 " deleted", idx);
    return ret;
}
