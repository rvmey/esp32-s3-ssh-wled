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

#define LCD_PHYS_W   320   /* physical panel columns */
#define LCD_PHYS_H   480   /* physical panel rows    */
#define LCD_MAX_DIM  480   /* longest dimension (landscape row width) */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static spi_device_handle_t s_spi       = NULL;
static uint8_t             s_r, s_g, s_b;
static bool                s_landscape = false;

/* Logical width/height — swapped in landscape mode */
static inline int lcd_w(void) { return s_landscape ? LCD_PHYS_H : LCD_PHYS_W; }
static inline int lcd_h(void) { return s_landscape ? LCD_PHYS_W : LCD_PHYS_H; }

/*
 * Row pixel buffer – one row of RGB565 pixels, 4-byte aligned so it is
 * valid as a DMA source.  Sized for the widest possible row (landscape).
 */
static uint8_t s_row_buf[LCD_MAX_DIM * 2] __attribute__((aligned(4)));

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
 * Write one row of pixel data (lcd_w() pixels × 2 bytes each).
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
            .length    = ((unsigned)lcd_w() * 2) * 8,
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

    int w = lcd_w();
    int h = lcd_h();

    /* Fill row buffer */
    for (int i = 0; i < w * 2; i += 2) {
        s_row_buf[i]     = ph;
        s_row_buf[i + 1] = pl;
    }

    /* Set full-screen address window */
    uint8_t caset[] = { 0x00, 0x00,
                        (uint8_t)((w - 1) >> 8),
                        (uint8_t)((w - 1) & 0xFF) };
    uint8_t raset[] = { 0x00, 0x00,
                        (uint8_t)((h - 1) >> 8),
                        (uint8_t)((h - 1) & 0xFF) };

    axs_cmd(0x2A, caset, sizeof(caset));   /* CASET */
    axs_cmd(0x2B, raset, sizeof(raset));   /* RASET */

    /* Stream rows: first row uses RAMWR, remaining use RAMWRC */
    axs_write_row(false);
    for (int y = 1; y < h; y++) {
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
        .max_transfer_sz = (LCD_MAX_DIM * 2) + 4,   /* header + one row (max dim) */
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

    ESP_LOGI(TAG, "AXS15231 ready (%d x %d @ 40 MHz QSPI)", lcd_w(), lcd_h());
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

void screen_set_landscape(bool landscape)
{
    s_landscape = landscape;
    uint8_t madctl = landscape ? 0x60 : 0x00;  /* MV|MX = 90° landscape */
    axs_cmd(0x36, &madctl, 1);
    screen_fill(s_r, s_g, s_b);
}

void screen_get_landscape(bool *landscape)
{
    *landscape = s_landscape;
}

/* ------------------------------------------------------------------ */
/* Bitmap font (8 × 8, ASCII 0x20 ' ' – 0x7E '~', MSB = left pixel)  */
/* ------------------------------------------------------------------ */
static const uint8_t s_font8x8[95][8] = {
    /* 20 sp */ { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 21 !  */ { 0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00 },
    /* 22 "  */ { 0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 23 #  */ { 0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00 },
    /* 24 $  */ { 0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00 },
    /* 25 %  */ { 0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00 },
    /* 26 &  */ { 0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00 },
    /* 27 '  */ { 0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00 },
    /* 28 (  */ { 0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00 },
    /* 29 )  */ { 0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00 },
    /* 2A *  */ { 0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00 },
    /* 2B +  */ { 0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00 },
    /* 2C ,  */ { 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06 },
    /* 2D -  */ { 0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00 },
    /* 2E .  */ { 0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00 },
    /* 2F /  */ { 0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00 },
    /* 30 0  */ { 0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00 },
    /* 31 1  */ { 0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00 },
    /* 32 2  */ { 0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00 },
    /* 33 3  */ { 0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00 },
    /* 34 4  */ { 0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00 },
    /* 35 5  */ { 0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00 },
    /* 36 6  */ { 0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00 },
    /* 37 7  */ { 0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00 },
    /* 38 8  */ { 0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00 },
    /* 39 9  */ { 0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00 },
    /* 3A :  */ { 0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00 },
    /* 3B ;  */ { 0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06 },
    /* 3C <  */ { 0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00 },
    /* 3D =  */ { 0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00 },
    /* 3E >  */ { 0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00 },
    /* 3F ?  */ { 0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00 },
    /* 40 @  */ { 0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00 },
    /* 41 A  */ { 0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00 },
    /* 42 B  */ { 0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00 },
    /* 43 C  */ { 0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00 },
    /* 44 D  */ { 0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00 },
    /* 45 E  */ { 0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00 },
    /* 46 F  */ { 0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00 },
    /* 47 G  */ { 0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00 },
    /* 48 H  */ { 0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00 },
    /* 49 I  */ { 0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00 },
    /* 4A J  */ { 0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00 },
    /* 4B K  */ { 0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00 },
    /* 4C L  */ { 0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00 },
    /* 4D M  */ { 0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00 },
    /* 4E N  */ { 0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00 },
    /* 4F O  */ { 0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00 },
    /* 50 P  */ { 0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00 },
    /* 51 Q  */ { 0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00 },
    /* 52 R  */ { 0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00 },
    /* 53 S  */ { 0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00 },
    /* 54 T  */ { 0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00 },
    /* 55 U  */ { 0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00 },
    /* 56 V  */ { 0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00 },
    /* 57 W  */ { 0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00 },
    /* 58 X  */ { 0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00 },
    /* 59 Y  */ { 0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00 },
    /* 5A Z  */ { 0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00 },
    /* 5B [  */ { 0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00 },
    /* 5C \  */ { 0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00 },
    /* 5D ]  */ { 0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00 },
    /* 5E ^  */ { 0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00 },
    /* 5F _  */ { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF },
    /* 60 `  */ { 0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00 },
    /* 61 a  */ { 0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00 },
    /* 62 b  */ { 0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00 },
    /* 63 c  */ { 0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00 },
    /* 64 d  */ { 0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00 },
    /* 65 e  */ { 0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00 },
    /* 66 f  */ { 0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00 },
    /* 67 g  */ { 0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F },
    /* 68 h  */ { 0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00 },
    /* 69 i  */ { 0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00 },
    /* 6A j  */ { 0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E },
    /* 6B k  */ { 0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00 },
    /* 6C l  */ { 0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00 },
    /* 6D m  */ { 0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00 },
    /* 6E n  */ { 0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00 },
    /* 6F o  */ { 0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00 },
    /* 70 p  */ { 0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F },
    /* 71 q  */ { 0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78 },
    /* 72 r  */ { 0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00 },
    /* 73 s  */ { 0x00,0x00,0x1E,0x03,0x1E,0x30,0x1F,0x00 },
    /* 74 t  */ { 0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00 },
    /* 75 u  */ { 0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00 },
    /* 76 v  */ { 0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00 },
    /* 77 w  */ { 0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00 },
    /* 78 x  */ { 0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00 },
    /* 79 y  */ { 0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F },
    /* 7A z  */ { 0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00 },
    /* 7B {  */ { 0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00 },
    /* 7C |  */ { 0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00 },
    /* 7D }  */ { 0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00 },
    /* 7E ~  */ { 0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00 },
};

/* Rendering parameters */
#define FONT_SCALE    2                         /* each font pixel → 2×2 screen pixels */
#define CHAR_W        (8 * FONT_SCALE)           /* 16 screen pixels wide */
#define CHAR_H        (8 * FONT_SCALE)           /* 16 screen pixels tall */
/* Max cols/rows are for the landscape or portrait max dimension (480/16 = 30) */
#define TEXT_COLS_MAX (LCD_MAX_DIM / CHAR_W)     /* 30 — covers landscape cols & portrait rows */
#define TEXT_ROWS_MAX (LCD_MAX_DIM / CHAR_H)     /* 30 — covers portrait rows & landscape cols */

void screen_draw_text(const char *text)
{
    /* ---- Word-wrap into lines of at most avail chars ---- */
    static char lines[TEXT_ROWS_MAX][TEXT_COLS_MAX + 1];
    int line_len[TEXT_ROWS_MAX];
    int num_lines = 0;

    int max_cols = lcd_w() / CHAR_W;
    int max_rows = lcd_h() / CHAR_H;

    const char *src = text;
    while (*src && num_lines < max_rows) {
        /* Skip leading spaces at the start of each line */
        while (*src == ' ') src++;
        if (*src == '\0') break;

        /* Find how much fits: try to break at a word boundary */
        int avail = max_cols;
        int take  = 0;

        /* Count remaining non-NUL chars */
        int remaining = (int)strlen(src);
        if (remaining <= avail) {
            take = remaining;          /* everything fits */
        } else {
            /* Walk back from avail to find a space to break on */
            take = avail;
            while (take > 0 && src[take] != ' ' && src[take] != '\0') take--;
            if (take == 0) take = avail;   /* no space found: hard break */
        }

        memcpy(lines[num_lines], src, (size_t)take);
        lines[num_lines][take] = '\0';
        line_len[num_lines] = take;
        num_lines++;
        src += take;
    }
    if (num_lines == 0) return;

    /* ---- Colours ---- */
    uint16_t bg = ((uint16_t)(s_r & 0xF8) << 8)
                | ((uint16_t)(s_g & 0xFC) << 3)
                | (s_b >> 3);
    uint8_t bg_h = (uint8_t)(bg >> 8);
    uint8_t bg_l = (uint8_t)(bg & 0xFF);
    uint8_t fg_h = 0xFF;
    uint8_t fg_l = 0xFF;

    int w = lcd_w();
    int h = lcd_h();

    /* Centre the block vertically */
    int start_y = (h - num_lines * CHAR_H) / 2;

    /* Full-screen address window */
    uint8_t caset[] = { 0x00, 0x00,
                        (uint8_t)((w - 1) >> 8),
                        (uint8_t)((w - 1) & 0xFF) };
    uint8_t raset[] = { 0x00, 0x00,
                        (uint8_t)((h - 1) >> 8),
                        (uint8_t)((h - 1) & 0xFF) };
    axs_cmd(0x2A, caset, sizeof(caset));
    axs_cmd(0x2B, raset, sizeof(raset));

    for (int y = 0; y < h; y++) {
        /* Fill row buffer with background */
        for (int i = 0; i < w * 2; i += 2) {
            s_row_buf[i]     = bg_h;
            s_row_buf[i + 1] = bg_l;
        }

        int rel_y = y - start_y;
        if (rel_y >= 0 && rel_y < num_lines * CHAR_H) {
            int trow      = rel_y / CHAR_H;
            int font_scan = (rel_y % CHAR_H) / FONT_SCALE;
            int ci_count  = line_len[trow];
            const char *row_text = lines[trow];

            /* Centre this line horizontally */
            int start_x = (w - ci_count * CHAR_W) / 2;

            for (int c = 0; c < ci_count; c++) {
                unsigned char ch = (unsigned char)row_text[c];
                if (ch < 0x20 || ch > 0x7E) ch = '?';
                uint8_t frow = s_font8x8[ch - 0x20][font_scan];

                for (int bit = 0; bit < 8; bit++) {
                    bool is_fg = (frow & (0x01u << bit)) != 0;
                    uint8_t ph = is_fg ? fg_h : bg_h;
                    uint8_t pl = is_fg ? fg_l : bg_l;
                    int px = start_x + c * CHAR_W + bit * FONT_SCALE;
                    for (int sc = 0; sc < FONT_SCALE; sc++) {
                        unsigned int x = (unsigned int)(px + sc);
                        if (x < (unsigned int)w) {
                            s_row_buf[x * 2]     = ph;
                            s_row_buf[x * 2 + 1] = pl;
                        }
                    }
                }
            }
        }

        axs_write_row(y != 0);
    }
}
