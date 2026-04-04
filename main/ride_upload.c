#include "ride_upload.h"
#include "ride_log.h"
#include "sdkconfig.h"

#include "esp_http_client.h"
#include "esp_log.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "ride_upload";

/* Maximum bytes per JSON point: {"lat":-999999999,"lon":-999999999,"speed_kmh":32767}, */
#define JSON_BYTES_PER_POINT  56
#define JSON_HEADER_MAX       64   /* {"start_ts":4294967295,"points":[ */
#define JSON_FOOTER           "]}"

/* ── Upload a single ride ────────────────────────────────────────────────── */

static esp_err_t upload_ride(uint32_t idx)
{
    track_point_t *pts    = NULL;
    size_t         n_pts  = 0;
    uint32_t       ts     = 0;

    esp_err_t ret = ride_log_read(idx, &pts, &n_pts, &ts);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ride %" PRIu32 " unreadable: %s — skipping",
                 idx, esp_err_to_name(ret));
        return ESP_OK;  /* not a fatal error; continue with other rides */
    }
    if (n_pts == 0) {
        free(pts);
        return ESP_OK;
    }

    /* --- Build JSON body ------------------------------------------------- */
    size_t json_sz = JSON_HEADER_MAX
                     + n_pts * JSON_BYTES_PER_POINT
                     + sizeof(JSON_FOOTER);
    char *json = malloc(json_sz);
    if (!json) {
        free(pts);
        return ESP_ERR_NO_MEM;
    }

    int off = snprintf(json, json_sz,
                       "{\"start_ts\":%" PRIu32 ",\"points\":[", ts);

    for (size_t i = 0; i < n_pts && off < (int)json_sz - 10; i++) {
        off += snprintf(json + off, json_sz - off,
                        "%s{\"lat\":%" PRId32 ",\"lon\":%" PRId32
                        ",\"speed_kmh\":%" PRId16 "}",
                        (i > 0) ? "," : "",
                        pts[i].lat, pts[i].lon, pts[i].speed_kmh);
    }
    off += snprintf(json + off, json_sz - off, "%s", JSON_FOOTER);
    free(pts);

    /* --- HTTP POST ------------------------------------------------------- */
    esp_http_client_config_t cfg = {
        .url             = CONFIG_TRACKER_UPLOAD_URL,
        .method          = HTTP_METHOD_POST,
        .timeout_ms      = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(json);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, off);

    ret = esp_http_client_perform(client);
    int status = -1;
    if (ret == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Ride %" PRIu32 ": %zu points → HTTP %d",
                 idx, n_pts, status);
    } else {
        ESP_LOGW(TAG, "Ride %" PRIu32 " upload failed: %s",
                 idx, esp_err_to_name(ret));
    }

    esp_http_client_cleanup(client);
    free(json);

    if (ret == ESP_OK && (status == 200 || status == 201)) {
        ride_log_delete(idx);
    }

    return ESP_OK;   /* non-fatal: leave ride in NVS on any error */
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t ride_upload_all(void)
{
    const char *url = CONFIG_TRACKER_UPLOAD_URL;
    if (!url || url[0] == '\0') {
        ESP_LOGI(TAG, "No upload URL configured — skipping");
        return ESP_OK;
    }

    uint32_t count = 0;
    esp_err_t ret = ride_log_count(&count);
    if (ret != ESP_OK || count == 0) return ret;

    ESP_LOGI(TAG, "Uploading %" PRIu32 " ride(s) to %s", count, url);

    for (uint32_t i = 0; i < count; i++) {
        upload_ride(i);   /* errors per-ride are non-fatal */
    }

    return ESP_OK;
}
