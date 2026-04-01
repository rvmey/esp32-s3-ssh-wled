/*
 * screen_control.c
 *
 * AXS15231 QSPI display driver for the Guition JC3248W535 (320 x 480).
 * Fills the entire screen with a single solid colour over the SSH shell
 * "color" and "off" commands.
 *
 * Protocol summary
 * ─────────────────
 * Every SPI transaction to the AXS15231 begins with a 4-byte header that
 * is always sent in single-wire SPI mode:
 *
 *   Byte 0 : 0x02  → subsequent data bytes in single-wire SPI (register write)
 *            0x32  → subsequent data bytes in quad-wire SPI   (pixel write)
 *   Byte 1 : 0x00
 *   Byte 2 : register / command byte
 *   Byte 3 : 0x00
 *
 * Using spi_transaction_ext_t:
 *   command_bits = 8   → byte 0 maps to .cmd
 *   address_bits = 24  → bytes 1-3 map to .addr (MSB-first, so addr = reg << 8)
 *   data phase         → follows with SPI_TRANS_MODE_QIO for pixel writes
 */

#include "screen_control.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "screen";

/* ------------------------------------------------------------------ */
/* Hardware constants (Guition JC3248W535)                            */
/* ------------------------------------------------------------------ */

#define LCD_HOST     SPI2_HOST
#define LCD_CLK_HZ   (40 * 1000 * 1000)

#define LCD_D0       21   /* MOSI / quad D0 */
#define LCD_D1       48   /* MISO / quad D1 */
#define LCD_D2       40   /* WP   / quad D2 */
#define LCD_D3       39   /* HD   / quad D3 */
#define LCD_CLK      47
#define LCD_CS       45
#define LCD_BL       1    /* backlight (active-high) */

#define LCD_W        320
#define LCD_H        480

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static spi_device_handle_t s_spi  = NULL;
static uint8_t             s_r, s_g, s_b;

/*
 * Row pixel buffer – one row of RGB565 pixels, 4-byte aligned so it is
 * valid as a DMA source.  Allocated in BSS (internal SRAM on ESP32-S3).
 */
static uint8_t s_row_buf[LCD_W * 2] __attribute__((aligned(4)));

/* ------------------------------------------------------------------ */
/* Low-level SPI helpers                                              */
/* ------------------------------------------------------------------ */

/*
 * Send a register-write command (prefix 0x02, single-wire data).
 * The header [0x02, 0x00, cmd, 0x00] is encoded as the SPI command+address
 * phases, so the whole transaction is a single CS assertion.
 *
 * len may be 0 for commands that carry no data (SLEEP_OUT, DISPLAY_ON, …).
 *
 * The data pointer must remain valid for the duration of the call.
 */
static void axs_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    spi_transaction_ext_t t = {
        .base = {
            .flags      = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR,
            .cmd        = 0x02,
            .addr       = (uint32_t)cmd << 8,   /* [0x00][cmd][0x00] */
            .length     = len * 8,
            .tx_buffer  = (len > 0) ? data : NULL,
        },
        .command_bits = 8,
        .address_bits = 24,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t));
}

/*
 * Write one row of pixel data (LCD_W pixels × 2 bytes each).
 *
 * is_continue=false → RAMWR  (0x2C) — starts writing at the current address
 *                             window origin; use for the first row.
 * is_continue=true  → RAMWRC (0x3C) — continues from where the previous
 *                             RAMWR/RAMWRC left off; use for rows 1…(H-1).
 *
 * Header prefix 0x32 instructs the AXS15231 to receive pixel data in
 * quad-wire SPI; SPI_TRANS_MODE_QIO activates quad output on the ESP32-S3
 * SPI peripheral for the data phase.
 */
static void axs_write_row(bool is_continue)
{
    uint8_t ram_cmd = is_continue ? 0x3C : 0x2C;

    spi_transaction_ext_t t = {
        .base = {
            .flags     = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR
                       | SPI_TRANS_MODE_QIO,
            .cmd       = 0x32,
            .addr      = (uint32_t)ram_cmd << 8,
            .length    = (LCD_W * 2) * 8,
            .tx_buffer = s_row_buf,
        },
        .command_bits = 8,
        .address_bits = 24,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, (spi_transaction_t *)&t));
}

/* ------------------------------------------------------------------ */
/* Internal fill                                                       */
/* ------------------------------------------------------------------ */

static void screen_fill(uint8_t r, uint8_t g, uint8_t b)
{
    /* Convert to RGB565 big-endian */
    uint16_t px = ((uint16_t)(r & 0xF8) << 8)
                | ((uint16_t)(g & 0xFC) << 3)
                | (b >> 3);
    uint8_t ph = (uint8_t)(px >> 8);
    uint8_t pl = (uint8_t)(px & 0xFF);

    /* Fill row buffer */
    for (int i = 0; i < LCD_W * 2; i += 2) {
        s_row_buf[i]     = ph;
        s_row_buf[i + 1] = pl;
    }

    /* Set full-screen address window */
    uint8_t caset[] = { 0x00, 0x00,
                        (uint8_t)((LCD_W - 1) >> 8),
                        (uint8_t)((LCD_W - 1) & 0xFF) };
    uint8_t raset[] = { 0x00, 0x00,
                        (uint8_t)((LCD_H - 1) >> 8),
                        (uint8_t)((LCD_H - 1) & 0xFF) };

    axs_cmd(0x2A, caset, sizeof(caset));   /* CASET */
    axs_cmd(0x2B, raset, sizeof(raset));   /* RASET */

    /* Stream rows: first row uses RAMWR, remaining use RAMWRC */
    axs_write_row(false);
    for (int y = 1; y < LCD_H; y++) {
        axs_write_row(true);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t screen_init(void)
{
    /* Backlight on */
    gpio_config_t bl = {
        .pin_bit_mask = BIT64(LCD_BL),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(LCD_BL, 1);

    /* SPI bus — QSPI, 40 MHz */
    spi_bus_config_t bus = {
        .mosi_io_num   = LCD_D0,
        .miso_io_num   = LCD_D1,
        .sclk_io_num   = LCD_CLK,
        .quadwp_io_num = LCD_D2,
        .quadhd_io_num = LCD_D3,
        .max_transfer_sz = (LCD_W * 2) + 4,   /* header + one row */
        .flags           = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    /* SPI device */
    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_CLK_HZ,
        .mode           = 0,
        .spics_io_num   = LCD_CS,
        .queue_size     = 1,
        .flags          = SPI_DEVICE_HALFDUPLEX,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &dev, &s_spi));

    /* AXS15231 initialisation */
    vTaskDelay(pdMS_TO_TICKS(20));

    axs_cmd(0x01, NULL, 0);              /* SW_RESET             */
    vTaskDelay(pdMS_TO_TICKS(120));

    axs_cmd(0x11, NULL, 0);              /* SLEEP_OUT            */
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Vendor unlock → set internal config → vendor lock */
    static const uint8_t bb_unlock[] = { 0x00,0x00,0x00,0x00,0x00,0x00,0x5A,0xA5 };
    static const uint8_t c1_data[]   = { 0x33 };
    static const uint8_t bb_lock[]   = { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 };
    axs_cmd(0xBB, bb_unlock, sizeof(bb_unlock));
    axs_cmd(0xC1, c1_data,   sizeof(c1_data));
    axs_cmd(0xBB, bb_lock,   sizeof(bb_lock));

    static const uint8_t colmod[] = { 0x55 };   /* 16-bit RGB565 */
    static const uint8_t madctl[] = { 0x00 };   /* normal orientation */
    axs_cmd(0x3A, colmod, sizeof(colmod));       /* COLMOD  */
    axs_cmd(0x36, madctl, sizeof(madctl));       /* MADCTL  */
    axs_cmd(0x20, NULL, 0);                      /* INVERT_OFF */
    axs_cmd(0x29, NULL, 0);                      /* DISPLAY_ON */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Start with a black screen */
    screen_off();

    ESP_LOGI(TAG, "AXS15231 ready (%d x %d @ 40 MHz QSPI)", LCD_W, LCD_H);
    return ESP_OK;
}

void screen_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_r = r;  s_g = g;  s_b = b;
    screen_fill(r, g, b);
}

void screen_off(void)
{
    s_r = s_g = s_b = 0;
    screen_fill(0, 0, 0);
}

void screen_get_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = s_r;  *g = s_g;  *b = s_b;
}
