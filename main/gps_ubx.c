#include "gps_ubx.h"
#include "sdkconfig.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdbool.h>

static const char *TAG = "gps_ubx";

#define UART_NUM       ((uart_port_t)CONFIG_GPS_UART_NUM)
#define UART_BUF_SIZE  512
#define GPS_BAUD       9600

/* UBX protocol constants */
#define UBX_SYNC1  0xB5
#define UBX_SYNC2  0x62

#define UBX_CLASS_NAV   0x01
#define UBX_ID_NAV_PVT  0x07
#define UBX_CLASS_CFG   0x06
#define UBX_ID_CFG_MSG  0x01

/* Maximum payload we need to buffer (NAV-PVT = 92 bytes) */
#define UBX_MAX_PAYLOAD 128

/* ── Module state ────────────────────────────────────────────────────────── */
static gps_pvt_t s_pvt;
static bool      s_pvt_valid = false;

/* ── UTC → Unix timestamp ────────────────────────────────────────────────── */

static uint32_t utc_to_unix(uint16_t y, uint8_t mo, uint8_t d,
                             uint8_t h, uint8_t mi, uint8_t s)
{
    /* Rata Die algorithm — correct for all dates after 1970-03-01.         */
    if (mo < 3) { y--; mo += 12; }
    uint32_t days = (uint32_t)(365 * y) + y / 4 - y / 100 + y / 400
                    + (153u * mo - 457u) / 5u + d - 719469u;
    return days * 86400u + (uint32_t)h * 3600u + (uint32_t)mi * 60u + s;
}

/* ── UBX frame checksum ──────────────────────────────────────────────────── */

static void ubx_checksum(const uint8_t *data, size_t len,
                         uint8_t *ck_a, uint8_t *ck_b)
{
    *ck_a = 0;
    *ck_b = 0;
    for (size_t i = 0; i < len; i++) {
        *ck_a += data[i];
        *ck_b += *ck_a;
    }
}

/* ── Send a UBX frame ────────────────────────────────────────────────────── */

static void send_ubx(uint8_t cls, uint8_t id,
                     const uint8_t *payload, uint16_t plen)
{
    /* Header: sync1, sync2, class, id, len_lo, len_hi */
    uint8_t hdr[6] = {
        UBX_SYNC1, UBX_SYNC2,
        cls, id,
        (uint8_t)(plen & 0xFF), (uint8_t)(plen >> 8)
    };
    uart_write_bytes(UART_NUM, (const char *)hdr, sizeof(hdr));
    if (plen > 0) {
        uart_write_bytes(UART_NUM, (const char *)payload, plen);
    }

    /* Checksum covers class, id, len_lo, len_hi, payload */
    uint8_t ck_buf[4 + UBX_MAX_PAYLOAD];
    ck_buf[0] = cls;  ck_buf[1] = id;
    ck_buf[2] = hdr[4]; ck_buf[3] = hdr[5];
    if (plen > 0 && plen <= UBX_MAX_PAYLOAD) {
        memcpy(ck_buf + 4, payload, plen);
    }
    uint8_t ck_a, ck_b;
    ubx_checksum(ck_buf, 4 + plen, &ck_a, &ck_b);
    uart_write_bytes(UART_NUM, (const char *)&ck_a, 1);
    uart_write_bytes(UART_NUM, (const char *)&ck_b, 1);
}

/* ── Parse NAV-PVT payload ───────────────────────────────────────────────── */

static void parse_nav_pvt(const uint8_t *p, uint16_t len)
{
    if (len < 64) return;   /* need at least through gSpeed field */

    uint8_t  fix_type = p[20];
    uint8_t  flags    = p[21];   /* bit 0 = gnssFixOK */
    uint8_t  num_sv   = p[23];

    /* lon then lat (both int32, 1e-7 deg, little-endian) */
    int32_t lon = (int32_t)((uint32_t)p[24]        | ((uint32_t)p[25] << 8)
                           | ((uint32_t)p[26] << 16) | ((uint32_t)p[27] << 24));
    int32_t lat = (int32_t)((uint32_t)p[28]        | ((uint32_t)p[29] << 8)
                           | ((uint32_t)p[30] << 16) | ((uint32_t)p[31] << 24));
    /* gSpeed at byte 60 (mm/s, int32) */
    int32_t gspeed = (int32_t)((uint32_t)p[60]        | ((uint32_t)p[61] << 8)
                               | ((uint32_t)p[62] << 16) | ((uint32_t)p[63] << 24));

    uint32_t ts = 0;
    if ((flags & 0x01) && len >= 11) {   /* gnssFixOK and time valid */
        uint16_t year  = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
        uint8_t  month = p[6];
        uint8_t  day   = p[7];
        uint8_t  hour  = p[8];
        uint8_t  min   = p[9];
        uint8_t  sec   = p[10];
        ts = utc_to_unix(year, month, day, hour, min, sec);
    }

    s_pvt.lat        = lat;
    s_pvt.lon        = lon;
    s_pvt.speed_mm_s = gspeed;
    s_pvt.unix_ts    = ts;
    s_pvt.fix_type   = fix_type;
    s_pvt.num_sv     = num_sv;
    s_pvt_valid      = true;

    ESP_LOGD(TAG, "NAV-PVT fix=%u sv=%u lat=%ld lon=%ld spd=%ld",
             fix_type, num_sv, (long)lat, (long)lon, (long)gspeed);
}

/* ── UBX frame reader ────────────────────────────────────────────────────── */

typedef enum {
    S_SYNC1, S_SYNC2, S_CLASS, S_ID,
    S_LEN1,  S_LEN2,  S_PAYLOAD, S_CKA, S_CKB
} ubx_state_t;

/**
 * Feed one byte into the UBX frame parser.  Returns true when a complete,
 * valid NAV-PVT frame has just been parsed (s_pvt updated).
 */
static bool feed_byte(uint8_t b)
{
    static ubx_state_t state    = S_SYNC1;
    static uint8_t     cls      = 0, id = 0;
    static uint16_t    plen     = 0, pidx = 0;
    static uint8_t     payload[UBX_MAX_PAYLOAD];
    static uint8_t     ck_a_rx  = 0, ck_a_calc = 0, ck_b_calc = 0;

    switch (state) {
    case S_SYNC1:
        if (b == UBX_SYNC1) state = S_SYNC2;
        break;
    case S_SYNC2:
        state = (b == UBX_SYNC2) ? S_CLASS : S_SYNC1;
        break;
    case S_CLASS:
        cls = b; ck_a_calc = b; ck_b_calc = b; state = S_ID;
        break;
    case S_ID:
        id = b; ck_a_calc += b; ck_b_calc += ck_a_calc; state = S_LEN1;
        break;
    case S_LEN1:
        plen = b; ck_a_calc += b; ck_b_calc += ck_a_calc; state = S_LEN2;
        break;
    case S_LEN2:
        plen |= ((uint16_t)b << 8);
        ck_a_calc += b; ck_b_calc += ck_a_calc;
        pidx = 0;
        state = (plen == 0) ? S_CKA : S_PAYLOAD;
        break;
    case S_PAYLOAD:
        if (pidx < UBX_MAX_PAYLOAD) payload[pidx] = b;
        ck_a_calc += b; ck_b_calc += ck_a_calc;
        if (++pidx >= plen) state = S_CKA;
        break;
    case S_CKA:
        ck_a_rx = b; state = S_CKB;
        break;
    case S_CKB: {
        state = S_SYNC1;
        bool valid = (ck_a_rx == ck_a_calc) && (b == ck_b_calc);
        if (valid && cls == UBX_CLASS_NAV && id == UBX_ID_NAV_PVT) {
            parse_nav_pvt(payload, plen);
            return true;
        }
        break;
    }
    }
    return false;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t gps_ubx_init(void)
{
    s_pvt_valid = false;

#if CONFIG_GPS_POWER_GPIO >= 0
    gpio_reset_pin(CONFIG_GPS_POWER_GPIO);
    gpio_set_direction(CONFIG_GPS_POWER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_GPS_POWER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));   /* allow GPS to power up */
    ESP_LOGI(TAG, "GPS power enabled on GPIO %d", CONFIG_GPS_POWER_GPIO);
#endif

    /* Install UART driver */
    uart_config_t ucfg = {
        .baud_rate  = GPS_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_param_config(UART_NUM, &ucfg);
    if (ret != ESP_OK) return ret;

    ret = uart_set_pin(UART_NUM,
                       CONFIG_GPS_UART_TX_GPIO, CONFIG_GPS_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    ret = uart_driver_install(UART_NUM, UART_BUF_SIZE, 0, 0, NULL, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    /* Wait for GPS to emit its startup messages before sending config.      */
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* Enable NAV-PVT output at 1 Hz on this UART port.
     * UBX-CFG-MSG (0x06 0x01): msgClass, msgID, rate                       */
    uint8_t cfg_msg[] = {UBX_CLASS_NAV, UBX_ID_NAV_PVT, 0x01};
    send_ubx(UBX_CLASS_CFG, UBX_ID_CFG_MSG, cfg_msg, sizeof(cfg_msg));
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "GPS UART%d initialised at %d baud, NAV-PVT enabled",
             CONFIG_GPS_UART_NUM, GPS_BAUD);
    return ESP_OK;
}

esp_err_t gps_ubx_wait_fix(int timeout_s)
{
    int64_t deadline_ms = (int64_t)timeout_s * 1000;
    int64_t elapsed_ms  = 0;
    uint8_t byte;

    ESP_LOGI(TAG, "Waiting up to %d s for 3D fix ...", timeout_s);

    while (elapsed_ms < deadline_ms) {
        int n = uart_read_bytes(UART_NUM, &byte, 1, pdMS_TO_TICKS(200));
        if (n == 1) {
            if (feed_byte(byte) && s_pvt.fix_type >= 3) {
                ESP_LOGI(TAG, "3D fix obtained: lat=%ld lon=%ld sv=%u",
                         (long)s_pvt.lat, (long)s_pvt.lon, s_pvt.num_sv);
                return ESP_OK;
            }
        } else {
            elapsed_ms += 200;
        }
        /* Accumulate time even when reading — approximate but sufficient.  */
        elapsed_ms += 1;
    }

    ESP_LOGW(TAG, "GPS fix timeout after %d s", timeout_s);
    return ESP_ERR_TIMEOUT;
}

esp_err_t gps_ubx_get_pvt(gps_pvt_t *pvt)
{
    /* Drain any fresh bytes from the UART ring buffer and update s_pvt.    */
    uint8_t byte;
    while (uart_read_bytes(UART_NUM, &byte, 1, 0) == 1) {
        feed_byte(byte);
    }

    if (!s_pvt_valid) return ESP_ERR_NOT_FOUND;
    *pvt = s_pvt;
    return ESP_OK;
}

void gps_ubx_deinit(void)
{
    uart_driver_delete(UART_NUM);

#if CONFIG_GPS_POWER_GPIO >= 0
    gpio_set_level(CONFIG_GPS_POWER_GPIO, 0);
    ESP_LOGI(TAG, "GPS power disabled");
#endif

    s_pvt_valid = false;
    ESP_LOGI(TAG, "GPS driver de-initialised");
}
