/*
 * screen_control_cyd.c
 *
 * ILI9342C/ILI9341-style SPI display driver for ESP32-2432S028R boards
 * ("Cheap Yellow Display") using the same screen_control.h API shape as the
 * Core2 implementation.
 *
 * Default wiring used here matches common ESP32-2432S028R layouts:
 *   MOSI=GPIO13  SCK=GPIO14  CS=GPIO15  DC=GPIO2
 *   MISO is not used by this driver path.
 *
 * Orientation:
 *   Landscape 320x240 is the default to mirror Core2 command behavior.
 */

#include "screen_control.h"

#include "sdkconfig.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "screen_cyd";

/* ------------------------------------------------------------------ */
/* Hardware constants (ESP32-2432S028R CYD)                           */
/* ------------------------------------------------------------------ */

#define LCD_HOST     SPI3_HOST
/* ESP32 SPI master on this setup rejects >26.666MHz for the LCD device. */
#define LCD_CLK_HZ   (26 * 1000 * 1000)

#define LCD_MOSI     CONFIG_CYD_LCD_MOSI_GPIO
#define LCD_MISO     CONFIG_CYD_LCD_MISO_GPIO
#define LCD_CLK      CONFIG_CYD_LCD_SCLK_GPIO
#define LCD_CS       CONFIG_CYD_LCD_CS_GPIO
#define LCD_DC       CONFIG_CYD_LCD_DC_GPIO
#define LCD_RST      CONFIG_CYD_LCD_RST_GPIO
#define BL_GPIO      CONFIG_CYD_BACKLIGHT_GPIO

#define LCD_PHYS_W   320   /* panel physical width  (native landscape) */
#define LCD_PHYS_H   240   /* panel physical height (native landscape) */
#define LCD_MAX_DIM  320   /* longest physical dimension, for row-buf   */

/*
 * MADCTL value for native landscape orientation (MV=1, BGR=1).
 *
 * This is a 240x320-native ILI9341 panel (the common ESP32-2432S028R "CYD"
 * panel). On ILI9341, the CASET/RASET valid ranges depend on the MV (row/
 * column exchange) bit: with MV=0, CASET max=239 and RASET max=319; with
 * MV=1 the axes swap, so CASET max=319 and RASET max=239. The 320x240
 * landscape window below (ili_set_window(0,0,LCD_PHYS_W-1,LCD_PHYS_H-1) =
 * CASET(0,319)/RASET(0,239)) is only valid with MV=1. The previous value
 * 0x08 (MV=0) made CASET(0,319) out of range, corrupting GRAM addressing
 * (split black/noise-left, white-right screen).
 *
 * If colours appear swapped (red <-> blue), clear the BGR bit (0x28->0x20).
 * If the image is mirrored/flipped, try adding MX (0x68) or MY (0xA8).
 */
#define MADCTL_LANDSCAPE  0x28   /* MV (row/col exchange) + BGR */

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static spi_device_handle_t s_spi         = NULL;
static uint8_t             s_r, s_g, s_b;
static uint8_t             s_fr = 0xFF, s_fg = 0xFF, s_fb = 0xFF;
static bool                s_landscape   = true;  /* default: landscape */
static int                 s_font_scale  = 2;
static char                s_text[512];

static int               s_scroll_line  = 0;
static int               s_scroll_total = 0;

static SemaphoreHandle_t s_draw_mutex   = NULL;
static screen_touch_handler_t s_touch_handler = NULL;
static spi_device_handle_t s_touch_spi = NULL;
static bool s_touch_i2c_ready = false;

/* Logical width/height — NOTE: landscape is the native/default orientation */
static inline int lcd_w(void) { return s_landscape ? LCD_PHYS_W : LCD_PHYS_H; }
static inline int lcd_h(void) { return s_landscape ? LCD_PHYS_H : LCD_PHYS_W; }

static inline void touch_raw_to_logical(int raw_x, int raw_y,
                                        int *logical_x, int *logical_y)
{
    if (s_landscape) {
        *logical_x = raw_x;
        *logical_y = raw_y;
    } else {
        *logical_x = (LCD_PHYS_H - 1) - raw_y;
        *logical_y = raw_x;
    }
}

/* Row buffer — one physical row = 320 × 2 bytes */
static uint8_t s_row_buf[LCD_MAX_DIM * 2];

static inline void cyd_backlight_set(bool on)
{
    if (BL_GPIO < 0) return;
#if CONFIG_CYD_BACKLIGHT_ACTIVE_HIGH
    gpio_set_level(BL_GPIO, on ? 1 : 0);
#else
    gpio_set_level(BL_GPIO, on ? 0 : 1);
#endif
}

static int clamp_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline void screen_full_redraw_yield(int row)
{
    (void)row;
    /* Aggressive yielding keeps high-priority Wi-Fi/BT interrupts serviced
     * while we stream full-screen SPI redraws on classic ESP32. */
    vTaskDelay(pdMS_TO_TICKS(1));
}

/* ------------------------------------------------------------------ */
/* Low-level ILI9342C SPI helpers                                     */
/* ------------------------------------------------------------------ */

/* Pre-transfer callback: set DC pin according to transaction user field */
static void IRAM_ATTR ili_pre_cb(spi_transaction_t *t)
{
    gpio_set_level(LCD_DC, (int)(intptr_t)t->user);
}

/* Send one command byte (DC=0) */
static void ili_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &cmd,
        .user      = (void *)0,   /* DC = 0 */
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

/* Send data bytes (DC=1); len must be > 0 */
static void ili_data(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = data,
        .user      = (void *)1,   /* DC = 1 */
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

/* Send a single data byte (DC=1) */
static void ili_data_byte(uint8_t b)
{
    ili_data(&b, 1);
}

/* Set the GRAM address window */
static void ili_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t caset[4] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
                         (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
    uint8_t raset[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
                         (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };
    ili_cmd(0x2A);  ili_data(caset, 4);   /* CASET */
    ili_cmd(0x2B);  ili_data(raset, 4);   /* RASET */
    ili_cmd(0x2C);                        /* RAMWR */
}

/* Write one physical row of LCD_PHYS_W pixels (already in s_row_buf) */
static void ili_write_row(void)
{
    spi_transaction_t t = {
        .length    = (size_t)LCD_PHYS_W * 2 * 8,
        .tx_buffer = s_row_buf,
        .user      = (void *)1,   /* DC = 1 */
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

/*
 * Standard ILI9341 power/gamma init sequence (Adafruit_ILI9341-style),
 * sent between SLEEP_OUT and COLMOD/MADCTL/DISPLAY_ON.
 *
 * The previous sequence opened with 0xC8/{0xFF,0x93,0x42} (SETEXTC), an
 * ILI9342C-specific "extended command" unlock, followed by ILI9342C-format
 * 0xB0/0xF6 registers and packed gamma tables. On this board's true ILI9341
 * controller, 0xC8 is not a recognized command and corrupted the rest of
 * the init sequence regardless of MADCTL, producing an all-white screen.
 */
static void ili9341_send_extended_init(void)
{
    { uint8_t d[] = {0x03, 0x80, 0x02};             ili_cmd(0xEF); ili_data(d, sizeof(d)); }
    { uint8_t d[] = {0x00, 0xC1, 0x30};             ili_cmd(0xCF); ili_data(d, sizeof(d)); }
    { uint8_t d[] = {0x64, 0x03, 0x12, 0x81};       ili_cmd(0xED); ili_data(d, sizeof(d)); }
    { uint8_t d[] = {0x85, 0x00, 0x78};             ili_cmd(0xE8); ili_data(d, sizeof(d)); }
    { uint8_t d[] = {0x39, 0x2C, 0x00, 0x34, 0x02}; ili_cmd(0xCB); ili_data(d, sizeof(d)); }
    { uint8_t d[] = {0x20};                         ili_cmd(0xF7); ili_data(d, sizeof(d)); }
    { uint8_t d[] = {0x00, 0x00};                   ili_cmd(0xEA); ili_data(d, sizeof(d)); }
    { uint8_t d[] = {0x23};                         ili_cmd(0xC0); ili_data(d, sizeof(d)); } /* PWCTR1  */
    { uint8_t d[] = {0x10};                         ili_cmd(0xC1); ili_data(d, sizeof(d)); } /* PWCTR2  */
    { uint8_t d[] = {0x3E, 0x28};                   ili_cmd(0xC5); ili_data(d, sizeof(d)); } /* VMCTR1  */
    { uint8_t d[] = {0x86};                         ili_cmd(0xC7); ili_data(d, sizeof(d)); } /* VMCTR2  */
    { uint8_t d[] = {0x00, 0x18};                   ili_cmd(0xB1); ili_data(d, sizeof(d)); } /* FRMCTR1 */
    { uint8_t d[] = {0x08, 0x82, 0x27};             ili_cmd(0xB6); ili_data(d, sizeof(d)); } /* DFUNCTR */
    { uint8_t d[] = {0x00};                         ili_cmd(0xF2); ili_data(d, sizeof(d)); } /* 3GAMMA off */
    ili_cmd(0x26); ili_data_byte(0x01);                                                       /* GAMMA SET */
    {
        uint8_t d[] = {0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,0x37,0x07,0x10,0x03,0x0E,0x09,0x00};
        ili_cmd(0xE0); ili_data(d, sizeof(d)); /* GMCTRP1 */
    }
    {
        uint8_t d[] = {0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F};
        ili_cmd(0xE1); ili_data(d, sizeof(d)); /* GMCTRN1 */
    }
}

/* ------------------------------------------------------------------ */
/* Internal fill                                                       */
/* ------------------------------------------------------------------ */

static void screen_fill(uint8_t r, uint8_t g, uint8_t b)
{
    xSemaphoreTake(s_draw_mutex, portMAX_DELAY);

    uint16_t px = ((uint16_t)(r & 0xF8) << 8)
                | ((uint16_t)(g & 0xFC) << 3)
                | (b >> 3);
    uint8_t ph = (uint8_t)(px >> 8);
    uint8_t pl = (uint8_t)(px & 0xFF);

    for (int i = 0; i < LCD_PHYS_W * 2; i += 2) {
        s_row_buf[i]     = ph;
        s_row_buf[i + 1] = pl;
    }

    ili_set_window(0, 0, LCD_PHYS_W - 1, LCD_PHYS_H - 1);
    for (int y = 0; y < LCD_PHYS_H; y++) {
        ili_write_row();
        screen_full_redraw_yield(y);
    }

    xSemaphoreGive(s_draw_mutex);
}

/* ------------------------------------------------------------------ */
/* Rendering — font + word-wrap (shared algorithm with JC3248W535)    */
/* ------------------------------------------------------------------ */

#define TEXT_COLS_MAX (LCD_MAX_DIM / 8)
#define TEXT_ROWS_MAX (LCD_MAX_DIM / 16)
#define ALL_LINES_MAX 128

/* IBM VGA 8×16 font, ASCII 0x20–0x7E */
static const uint8_t s_font8x16[95][16] = {
    /* 0x20 */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x21 */ {0x00,0x00,0x18,0x3c,0x3c,0x3c,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 0x22 */ {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x23 */ {0x00,0x00,0x00,0x6c,0x6c,0xfe,0x6c,0x6c,0x6c,0xfe,0x6c,0x6c,0x00,0x00,0x00,0x00},
    /* 0x24 */ {0x18,0x18,0x7c,0xc6,0xc2,0xc0,0x7c,0x06,0x06,0x86,0xc6,0x7c,0x18,0x18,0x00,0x00},
    /* 0x25 */ {0x00,0x00,0x00,0x00,0xc2,0xc6,0x0c,0x18,0x30,0x60,0xc6,0x86,0x00,0x00,0x00,0x00},
    /* 0x26 */ {0x00,0x00,0x38,0x6c,0x6c,0x38,0x76,0xdc,0xcc,0xcc,0xcc,0x76,0x00,0x00,0x00,0x00},
    /* 0x27 */ {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x28 */ {0x00,0x00,0x0c,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0c,0x00,0x00,0x00,0x00},
    /* 0x29 */ {0x00,0x00,0x30,0x18,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 0x2a */ {0x00,0x00,0x00,0x00,0x00,0x66,0x3c,0xff,0x3c,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x2b */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7e,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x2c */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00},
    /* 0x2d */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xfe,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x2e */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 0x2f */ {0x00,0x00,0x00,0x00,0x02,0x06,0x0c,0x18,0x30,0x60,0xc0,0x80,0x00,0x00,0x00,0x00},
    /* 0x30 */ {0x00,0x00,0x38,0x6c,0xc6,0xc6,0xd6,0xd6,0xc6,0xc6,0x6c,0x38,0x00,0x00,0x00,0x00},
    /* 0x31 */ {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7e,0x00,0x00,0x00,0x00},
    /* 0x32 */ {0x00,0x00,0x7c,0xc6,0x06,0x0c,0x18,0x30,0x60,0xc0,0xc6,0xfe,0x00,0x00,0x00,0x00},
    /* 0x33 */ {0x00,0x00,0x7c,0xc6,0x06,0x06,0x3c,0x06,0x06,0x06,0xc6,0x7c,0x00,0x00,0x00,0x00},
    /* 0x34 */ {0x00,0x00,0x0c,0x1c,0x3c,0x6c,0xcc,0xfe,0x0c,0x0c,0x0c,0x1e,0x00,0x00,0x00,0x00},
    /* 0x35 */ {0x00,0x00,0xfe,0xc0,0xc0,0xc0,0xfc,0x06,0x06,0x06,0xc6,0x7c,0x00,0x00,0x00,0x00},
    /* 0x36 */ {0x00,0x00,0x38,0x60,0xc0,0xc0,0xfc,0xc6,0xc6,0xc6,0xc6,0x7c,0x00,0x00,0x00,0x00},
    /* 0x37 */ {0x00,0x00,0xfe,0xc6,0x06,0x06,0x0c,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00},
    /* 0x38 */ {0x00,0x00,0x7c,0xc6,0xc6,0xc6,0x7c,0xc6,0xc6,0xc6,0xc6,0x7c,0x00,0x00,0x00,0x00},
    /* 0x39 */ {0x00,0x00,0x7c,0xc6,0xc6,0xc6,0x7e,0x06,0x06,0x06,0x0c,0x78,0x00,0x00,0x00,0x00},
    /* 0x3a */ {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 0x3b */ {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 0x3c */ {0x00,0x00,0x00,0x06,0x0c,0x18,0x30,0x60,0x30,0x18,0x0c,0x06,0x00,0x00,0x00,0x00},
    /* 0x3d */ {0x00,0x00,0x00,0x00,0x00,0x7e,0x00,0x00,0x7e,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x3e */ {0x00,0x00,0x00,0x60,0x30,0x18,0x0c,0x06,0x0c,0x18,0x30,0x60,0x00,0x00,0x00,0x00},
    /* 0x3f */ {0x00,0x00,0x7c,0xc6,0xc6,0x0c,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 0x40 */ {0x00,0x00,0x00,0x7c,0xc6,0xc6,0xde,0xde,0xde,0xdc,0xc0,0x7c,0x00,0x00,0x00,0x00},
    /* 0x41 */ {0x00,0x00,0x10,0x38,0x6c,0xc6,0xc6,0xfe,0xc6,0xc6,0xc6,0xc6,0x00,0x00,0x00,0x00},
    /* 0x42 */ {0x00,0x00,0xfc,0x66,0x66,0x66,0x7c,0x66,0x66,0x66,0x66,0xfc,0x00,0x00,0x00,0x00},
    /* 0x43 */ {0x00,0x00,0x3c,0x66,0xc2,0xc0,0xc0,0xc0,0xc0,0xc2,0x66,0x3c,0x00,0x00,0x00,0x00},
    /* 0x44 */ {0x00,0x00,0xf8,0x6c,0x66,0x66,0x66,0x66,0x66,0x66,0x6c,0xf8,0x00,0x00,0x00,0x00},
    /* 0x45 */ {0x00,0x00,0xfe,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xfe,0x00,0x00,0x00,0x00},
    /* 0x46 */ {0x00,0x00,0xfe,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xf0,0x00,0x00,0x00,0x00},
    /* 0x47 */ {0x00,0x00,0x3c,0x66,0xc2,0xc0,0xc0,0xde,0xc6,0xc6,0x66,0x3a,0x00,0x00,0x00,0x00},
    /* 0x48 */ {0x00,0x00,0xc6,0xc6,0xc6,0xc6,0xfe,0xc6,0xc6,0xc6,0xc6,0xc6,0x00,0x00,0x00,0x00},
    /* 0x49 */ {0x00,0x00,0x3c,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3c,0x00,0x00,0x00,0x00},
    /* 0x4a */ {0x00,0x00,0x1e,0x0c,0x0c,0x0c,0x0c,0x0c,0xcc,0xcc,0xcc,0x78,0x00,0x00,0x00,0x00},
    /* 0x4b */ {0x00,0x00,0xe6,0x66,0x66,0x6c,0x78,0x78,0x6c,0x66,0x66,0xe6,0x00,0x00,0x00,0x00},
    /* 0x4c */ {0x00,0x00,0xf0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xfe,0x00,0x00,0x00,0x00},
    /* 0x4d */ {0x00,0x00,0xc6,0xee,0xfe,0xfe,0xd6,0xc6,0xc6,0xc6,0xc6,0xc6,0x00,0x00,0x00,0x00},
    /* 0x4e */ {0x00,0x00,0xc6,0xe6,0xf6,0xfe,0xde,0xce,0xc6,0xc6,0xc6,0xc6,0x00,0x00,0x00,0x00},
    /* 0x4f */ {0x00,0x00,0x7c,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00,0x00,0x00,0x00},
    /* 0x50 */ {0x00,0x00,0xfc,0x66,0x66,0x66,0x7c,0x60,0x60,0x60,0x60,0xf0,0x00,0x00,0x00,0x00},
    /* 0x51 */ {0x00,0x00,0x7c,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0xd6,0xde,0x7c,0x0c,0x0e,0x00,0x00},
    /* 0x52 */ {0x00,0x00,0xfc,0x66,0x66,0x66,0x7c,0x6c,0x66,0x66,0x66,0xe6,0x00,0x00,0x00,0x00},
    /* 0x53 */ {0x00,0x00,0x7c,0xc6,0xc6,0x60,0x38,0x0c,0x06,0xc6,0xc6,0x7c,0x00,0x00,0x00,0x00},
    /* 0x54 */ {0x00,0x00,0x7e,0x7e,0x5a,0x18,0x18,0x18,0x18,0x18,0x18,0x3c,0x00,0x00,0x00,0x00},
    /* 0x55 */ {0x00,0x00,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00,0x00,0x00,0x00},
    /* 0x56 */ {0x00,0x00,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x6c,0x38,0x10,0x00,0x00,0x00,0x00},
    /* 0x57 */ {0x00,0x00,0xc6,0xc6,0xc6,0xc6,0xd6,0xd6,0xd6,0xfe,0xee,0x6c,0x00,0x00,0x00,0x00},
    /* 0x58 */ {0x00,0x00,0xc6,0xc6,0x6c,0x7c,0x38,0x38,0x7c,0x6c,0xc6,0xc6,0x00,0x00,0x00,0x00},
    /* 0x59 */ {0x00,0x00,0x66,0x66,0x66,0x66,0x3c,0x18,0x18,0x18,0x18,0x3c,0x00,0x00,0x00,0x00},
    /* 0x5a */ {0x00,0x00,0xfe,0xc6,0x86,0x0c,0x18,0x30,0x60,0xc2,0xc6,0xfe,0x00,0x00,0x00,0x00},
    /* 0x5b */ {0x00,0x00,0x3c,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3c,0x00,0x00,0x00,0x00},
    /* 0x5c */ {0x00,0x00,0x00,0x80,0xc0,0xe0,0x70,0x38,0x1c,0x0e,0x06,0x02,0x00,0x00,0x00,0x00},
    /* 0x5d */ {0x00,0x00,0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x3c,0x00,0x00,0x00,0x00},
    /* 0x5e */ {0x10,0x38,0x6c,0xc6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x5f */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x00,0x00},
    /* 0x60 */ {0x00,0x30,0x18,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 0x61 */ {0x00,0x00,0x00,0x00,0x00,0x78,0x0c,0x7c,0xcc,0xcc,0xcc,0x76,0x00,0x00,0x00,0x00},
    /* 0x62 */ {0x00,0x00,0xe0,0x60,0x60,0x78,0x6c,0x66,0x66,0x66,0x66,0x7c,0x00,0x00,0x00,0x00},
    /* 0x63 */ {0x00,0x00,0x00,0x00,0x00,0x7c,0xc6,0xc0,0xc0,0xc0,0xc6,0x7c,0x00,0x00,0x00,0x00},
    /* 0x64 */ {0x00,0x00,0x1c,0x0c,0x0c,0x3c,0x6c,0xcc,0xcc,0xcc,0xcc,0x76,0x00,0x00,0x00,0x00},
    /* 0x65 */ {0x00,0x00,0x00,0x00,0x00,0x7c,0xc6,0xfe,0xc0,0xc0,0xc6,0x7c,0x00,0x00,0x00,0x00},
    /* 0x66 */ {0x00,0x00,0x1c,0x36,0x32,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    /* 0x67 */ {0x00,0x00,0x00,0x00,0x00,0x76,0xcc,0xcc,0xcc,0xcc,0xcc,0x7c,0x0c,0xcc,0x78,0x00},
    /* 0x68 */ {0x00,0x00,0xe0,0x60,0x60,0x6c,0x76,0x66,0x66,0x66,0x66,0xe6,0x00,0x00,0x00,0x00},
    /* 0x69 */ {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3c,0x00,0x00,0x00,0x00},
    /* 0x6a */ {0x00,0x00,0x06,0x06,0x00,0x0e,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3c,0x00},
    /* 0x6b */ {0x00,0x00,0xe0,0x60,0x60,0x66,0x6c,0x78,0x78,0x6c,0x66,0xe6,0x00,0x00,0x00,0x00},
    /* 0x6c */ {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3c,0x00,0x00,0x00,0x00},
    /* 0x6d */ {0x00,0x00,0x00,0x00,0x00,0xec,0xfe,0xd6,0xd6,0xd6,0xd6,0xc6,0x00,0x00,0x00,0x00},
    /* 0x6e */ {0x00,0x00,0x00,0x00,0x00,0xdc,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /* 0x6f */ {0x00,0x00,0x00,0x00,0x00,0x7c,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00,0x00,0x00,0x00},
    /* 0x70 */ {0x00,0x00,0x00,0x00,0x00,0xdc,0x66,0x66,0x66,0x66,0x66,0x7c,0x60,0x60,0xf0,0x00},
    /* 0x71 */ {0x00,0x00,0x00,0x00,0x00,0x76,0xcc,0xcc,0xcc,0xcc,0xcc,0x7c,0x0c,0x0c,0x1e,0x00},
    /* 0x72 */ {0x00,0x00,0x00,0x00,0x00,0xdc,0x76,0x66,0x60,0x60,0x60,0xf0,0x00,0x00,0x00,0x00},
    /* 0x73 */ {0x00,0x00,0x00,0x00,0x00,0x7c,0xc6,0x60,0x38,0x0c,0xc6,0x7c,0x00,0x00,0x00,0x00},
    /* 0x74 */ {0x00,0x00,0x10,0x30,0x30,0xfc,0x30,0x30,0x30,0x30,0x36,0x1c,0x00,0x00,0x00,0x00},
    /* 0x75 */ {0x00,0x00,0x00,0x00,0x00,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0x76,0x00,0x00,0x00,0x00},
    /* 0x76 */ {0x00,0x00,0x00,0x00,0x00,0xc6,0xc6,0xc6,0xc6,0xc6,0x6c,0x38,0x00,0x00,0x00,0x00},
    /* 0x77 */ {0x00,0x00,0x00,0x00,0x00,0xc6,0xc6,0xd6,0xd6,0xd6,0xfe,0x6c,0x00,0x00,0x00,0x00},
    /* 0x78 */ {0x00,0x00,0x00,0x00,0x00,0xc6,0x6c,0x38,0x38,0x38,0x6c,0xc6,0x00,0x00,0x00,0x00},
    /* 0x79 */ {0x00,0x00,0x00,0x00,0x00,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x7e,0x06,0x0c,0xf8,0x00},
    /* 0x7a */ {0x00,0x00,0x00,0x00,0x00,0xfe,0xcc,0x18,0x30,0x60,0xc6,0xfe,0x00,0x00,0x00,0x00},
    /* 0x7b */ {0x00,0x00,0x0e,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0e,0x00,0x00,0x00,0x00},
    /* 0x7c */ {0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 0x7d */ {0x00,0x00,0x70,0x18,0x18,0x18,0x0e,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00},
    /* 0x7e */ {0x00,0x76,0xdc,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

static bool text_pixel_is_fg(int logical_x, int logical_y,
                              const char (*lines)[TEXT_COLS_MAX + 1],
                              const int *line_len, int num_lines,
                              int logical_w, int start_y)
{
    int scale = s_font_scale;
    int cw    = 8  * scale;
    int ch    = 16 * scale;

    int rel_y = logical_y - start_y;
    if (rel_y < 0 || rel_y >= num_lines * ch) return false;

    int trow     = rel_y / ch;
    int ci_count = line_len[trow];
    int start_x  = (logical_w - ci_count * cw) / 2;
    int rel_x    = logical_x - start_x;
    if (rel_x < 0 || rel_x >= ci_count * cw) return false;

    int char_index = rel_x / cw;
    unsigned char glyph = (unsigned char)lines[trow][char_index];
    if (glyph < 0x20 || glyph > 0x7E) glyph = '?';
    int gi = glyph - 0x20;

    int font_col = (rel_x % cw) / scale;
    int font_row = (rel_y % ch) / scale;
    return (s_font8x16[gi][font_row] & (0x80u >> font_col)) != 0;
}

static bool text_is_scrollable(void)
{
    if (s_scroll_total == 0) return false;
    int max_rows = lcd_h() / (16 * s_font_scale);
    return s_scroll_total > max_rows;
}

static void apply_scroll_abs(int target)
{
    if (s_scroll_total == 0) return;
    int max_rows  = lcd_h() / (16 * s_font_scale);
    int max_scroll = s_scroll_total - max_rows;
    if (max_scroll <= 0) return;
    if (target < 0)          target = 0;
    if (target > max_scroll) target = max_scroll;
    if (target == s_scroll_line) return;
    s_scroll_line = target;
    screen_draw_text(NULL);  /* redraw at new scroll position without resetting it */
}

static bool touch_read_point_physical(int *raw_x, int *raw_y)
{
#if CONFIG_CYD_TOUCH_XPT2046
    if (!s_touch_spi) return false;

    uint8_t tx_x[3] = { 0xD0, 0x00, 0x00 };
    uint8_t tx_y[3] = { 0x90, 0x00, 0x00 };
    uint8_t rx_x[3] = { 0 };
    uint8_t rx_y[3] = { 0 };

    xSemaphoreTake(s_draw_mutex, portMAX_DELAY);

    spi_transaction_t t1 = {
        .length    = 24,
        .tx_buffer = tx_x,
        .rx_buffer = rx_x,
    };
    if (spi_device_polling_transmit(s_touch_spi, &t1) != ESP_OK) {
        xSemaphoreGive(s_draw_mutex);
        return false;
    }

    spi_transaction_t t2 = {
        .length    = 24,
        .tx_buffer = tx_y,
        .rx_buffer = rx_y,
    };
    if (spi_device_polling_transmit(s_touch_spi, &t2) != ESP_OK) {
        xSemaphoreGive(s_draw_mutex);
        return false;
    }

    xSemaphoreGive(s_draw_mutex);

    int x = ((int)rx_x[1] << 8 | rx_x[2]) >> 3;
    int y = ((int)rx_y[1] << 8 | rx_y[2]) >> 3;
    if (x <= 0 || y <= 0) return false;

#if CONFIG_CYD_XPT2046_SWAP_XY
    int t = x;
    x = y;
    y = t;
#endif

#if CONFIG_CYD_XPT2046_INVERT_X
    x = CONFIG_CYD_XPT2046_X_MAX - (x - CONFIG_CYD_XPT2046_X_MIN);
#endif
#if CONFIG_CYD_XPT2046_INVERT_Y
    y = CONFIG_CYD_XPT2046_Y_MAX - (y - CONFIG_CYD_XPT2046_Y_MIN);
#endif

    int x_den = CONFIG_CYD_XPT2046_X_MAX - CONFIG_CYD_XPT2046_X_MIN;
    int y_den = CONFIG_CYD_XPT2046_Y_MAX - CONFIG_CYD_XPT2046_Y_MIN;
    if (x_den < 10 || y_den < 10) return false;

    int sx = ((x - CONFIG_CYD_XPT2046_X_MIN) * (LCD_PHYS_W - 1)) / x_den;
    int sy = ((y - CONFIG_CYD_XPT2046_Y_MIN) * (LCD_PHYS_H - 1)) / y_den;

    *raw_x = clamp_i(sx, 0, LCD_PHYS_W - 1);
    *raw_y = clamp_i(sy, 0, LCD_PHYS_H - 1);
    return true;
#elif CONFIG_CYD_TOUCH_CST816
    if (!s_touch_i2c_ready) return false;

    uint8_t reg = 0x01;
    uint8_t buf[6] = {0};
    esp_err_t err = i2c_master_write_read_device(
        I2C_NUM_0,
        CONFIG_CYD_CST816_I2C_ADDR,
        &reg,
        1,
        buf,
        sizeof(buf),
        pdMS_TO_TICKS(20)
    );
    if (err != ESP_OK) return false;

    int points = buf[1] & 0x0F;
    if (points <= 0) return false;

    int x = ((buf[2] & 0x0F) << 8) | buf[3];
    int y = ((buf[4] & 0x0F) << 8) | buf[5];

    *raw_x = clamp_i(x, 0, LCD_PHYS_W - 1);
    *raw_y = clamp_i(y, 0, LCD_PHYS_H - 1);
    return true;
#else
    (void)raw_x;
    (void)raw_y;
    return false;
#endif
}

static void touch_poll_task(void *arg)
{
    (void)arg;

    bool touching = false;
    int start_scroll = 0;
    int start_v = 0;
    int start_lx = 0;
    int start_ly = 0;
    int last_lx = 0;
    int last_ly = 0;
    bool scroll_consumed = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_CYD_TOUCH_POLL_MS));

        int raw_x = 0;
        int raw_y = 0;
        bool have = touch_read_point_physical(&raw_x, &raw_y);

        int lx = 0;
        int ly = 0;
        if (have) {
            touch_raw_to_logical(raw_x, raw_y, &lx, &ly);
        }

        int raw_v = s_landscape ? raw_y : raw_x;
        int sign = -1;

        if (have && !touching) {
            touching = true;
            start_scroll = s_scroll_line;
            start_v = raw_v;
            start_lx = lx;
            start_ly = ly;
            last_lx = lx;
            last_ly = ly;
            scroll_consumed = false;
        } else if (have) {
            int row_h = 16 * s_font_scale;
            if (row_h == 0) continue;

            int delta_lines = sign * (raw_v - start_v) / row_h;
            if (delta_lines != 0) {
                scroll_consumed = true;
                apply_scroll_abs(start_scroll + delta_lines);
            }
            last_lx = lx;
            last_ly = ly;
        } else if (touching) {
            if (s_touch_handler) {
                int dx = last_lx - start_lx;
                int dy = last_ly - start_ly;
                int abs_dx = dx < 0 ? -dx : dx;
                int abs_dy = dy < 0 ? -dy : dy;

                if (abs_dx <= CONFIG_CYD_TOUCH_TAP_THRESHOLD &&
                    abs_dy <= CONFIG_CYD_TOUCH_TAP_THRESHOLD) {
                    if (!scroll_consumed) {
                        (void)s_touch_handler(last_lx, last_ly, SCREEN_GESTURE_TAP);
                    }
                } else {
                    screen_gesture_t g;
                    bool vertical;

                    if (abs_dx >= abs_dy) {
                        g = (dx > 0) ? SCREEN_GESTURE_SWIPE_RIGHT : SCREEN_GESTURE_SWIPE_LEFT;
                        vertical = false;
                    } else {
                        g = (dy > 0) ? SCREEN_GESTURE_SWIPE_DOWN : SCREEN_GESTURE_SWIPE_UP;
                        vertical = true;
                    }

                    if (!vertical || !text_is_scrollable() ||
                        (abs_dx >= CONFIG_CYD_TOUCH_SWIPE_THRESHOLD || abs_dy >= CONFIG_CYD_TOUCH_SWIPE_THRESHOLD)) {
                        (void)s_touch_handler(last_lx, last_ly, g);
                    }
                }
            }
            touching = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t screen_init(void)
{
    s_draw_mutex = xSemaphoreCreateMutex();

    if (BL_GPIO >= 0) {
        gpio_config_t bl_cfg = {
            .pin_bit_mask = BIT64(BL_GPIO),
            .mode         = GPIO_MODE_OUTPUT,
        };
        gpio_config(&bl_cfg);
        cyd_backlight_set(false);
    }

    /* ── DC pin (GPIO output) ────────────────────────────────────────── */
    gpio_config_t dc_cfg = {
        .pin_bit_mask = BIT64(LCD_DC),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&dc_cfg);
    gpio_set_level(LCD_DC, 1);

#if CONFIG_CYD_LCD_RST_GPIO >= 0
    {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = BIT64(LCD_RST),
            .mode         = GPIO_MODE_OUTPUT,
        };
        gpio_config(&rst_cfg);
        gpio_set_level(LCD_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#endif

    /* ── SPI bus ─────────────────────────────────────────────────────── */
    spi_bus_config_t bus = {
        .mosi_io_num   = LCD_MOSI,
        .miso_io_num   = LCD_MISO,
        .sclk_io_num   = LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (LCD_PHYS_W * 2) + 4,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_CLK_HZ,
        .mode           = 0,
        .spics_io_num   = LCD_CS,
        .queue_size     = 1,
        .pre_cb         = ili_pre_cb,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &dev, &s_spi));

    /* ── ILI9342C initialisation ─────────────────────────────────────── */
    vTaskDelay(pdMS_TO_TICKS(20));

    ili_cmd(0x01);                              /* SW_RESET          */
    vTaskDelay(pdMS_TO_TICKS(150));

    ili_cmd(0x11);                              /* SLEEP_OUT         */
    vTaskDelay(pdMS_TO_TICKS(120));

    ili9341_send_extended_init();

    ili_cmd(0x3A); ili_data_byte(0x55);         /* COLMOD: RGB565    */
    ili_cmd(0x36); ili_data_byte(MADCTL_LANDSCAPE); /* MADCTL        */
    /* No INVERT_ON (0x21): this true ILI9341 panel renders inverted
     * colours (black bg/white text shows as white bg/black text) when
     * inversion is enabled, so leave it at its post-reset default. */

    ili_cmd(0x29);                              /* DISPLAY_ON        */
    vTaskDelay(pdMS_TO_TICKS(20));

    screen_off();   /* clear GRAM before backlight is visible */
    cyd_backlight_set(true);

    ESP_LOGI(TAG, "ILI9342C ready (%d x %d @ %d MHz SPI)",
             lcd_w(), lcd_h(), LCD_CLK_HZ / 1000000);

#if CONFIG_CYD_TOUCH_XPT2046
    if (CONFIG_CYD_XPT2046_CS_GPIO >= 0) {
        spi_device_interface_config_t tdev = {
            .clock_speed_hz = CONFIG_CYD_XPT2046_CLK_HZ,
            .mode           = 0,
            .spics_io_num   = CONFIG_CYD_XPT2046_CS_GPIO,
            .queue_size     = 1,
        };
        esp_err_t terr = spi_bus_add_device(LCD_HOST, &tdev, &s_touch_spi);
        if (terr == ESP_OK) {
            ESP_LOGI(TAG, "XPT2046 touch enabled (CS=%d)", CONFIG_CYD_XPT2046_CS_GPIO);
            xTaskCreate(touch_poll_task, "cyd_touch", 3072, NULL, 4, NULL);
        } else {
            ESP_LOGW(TAG, "XPT2046 init failed: %s", esp_err_to_name(terr));
        }
    }
#elif CONFIG_CYD_TOUCH_CST816
    if (CONFIG_CYD_CST816_I2C_SDA_GPIO >= 0 && CONFIG_CYD_CST816_I2C_SCL_GPIO >= 0) {
        i2c_config_t i2c_cfg = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = CONFIG_CYD_CST816_I2C_SDA_GPIO,
            .scl_io_num = CONFIG_CYD_CST816_I2C_SCL_GPIO,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = 400000,
        };
        esp_err_t ierr = i2c_param_config(I2C_NUM_0, &i2c_cfg);
        if (ierr == ESP_OK) {
            ierr = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
            if (ierr == ESP_ERR_INVALID_STATE) ierr = ESP_OK;
        }
        if (ierr == ESP_OK) {
            s_touch_i2c_ready = true;
            ESP_LOGI(TAG, "CST816 touch enabled (SDA=%d SCL=%d addr=0x%02X)",
                     CONFIG_CYD_CST816_I2C_SDA_GPIO,
                     CONFIG_CYD_CST816_I2C_SCL_GPIO,
                     CONFIG_CYD_CST816_I2C_ADDR);
            xTaskCreate(touch_poll_task, "cyd_touch", 3072, NULL, 4, NULL);
        } else {
            ESP_LOGW(TAG, "CST816 init failed: %s", esp_err_to_name(ierr));
        }
    }
#endif

    return ESP_OK;
}

void screen_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_r = r;  s_g = g;  s_b = b;
    if (s_text[0]) screen_draw_text(s_text);
    else           screen_fill(r, g, b);
}

void screen_off(void)
{
    s_r = s_g = s_b = 0;
    s_text[0] = '\0';
    screen_fill(0, 0, 0);
}

void screen_backlight_off(void)
{
    cyd_backlight_set(false);
}

void screen_get_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = s_r;  *g = s_g;  *b = s_b;
}

void screen_set_landscape(bool landscape)
{
    s_landscape = landscape;
    if (s_text[0]) screen_draw_text(s_text);
    else           screen_fill(s_r, s_g, s_b);
}

void screen_get_landscape(bool *landscape)
{
    *landscape = s_landscape;
}

void screen_set_font_scale(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    s_font_scale = scale;
    if (s_text[0]) screen_draw_text(s_text);
}

void screen_set_font_scale_silent(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    s_font_scale = scale;
}

void screen_get_font_scale(int *scale) { *scale = s_font_scale; }

void screen_set_text_color(uint8_t r, uint8_t g, uint8_t b)
{
    s_fr = r;  s_fg = g;  s_fb = b;
    if (s_text[0]) screen_draw_text(s_text);
}

void screen_get_text_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = s_fr;  *g = s_fg;  *b = s_fb;
}

bool screen_spi_lock(uint32_t timeout_ms)
{
    if (!s_draw_mutex) return false;

    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms == 0) {
        ticks = 0;
    } else if (ticks == 0) {
        ticks = 1;
    }

    if (xSemaphoreTake(s_draw_mutex, ticks) != pdTRUE) {
        return false;
    }

    return (s_spi != NULL);
}

void screen_spi_unlock(void)
{
    if (s_draw_mutex) {
        xSemaphoreGive(s_draw_mutex);
    }
}

void screen_set_touch_handler(screen_touch_handler_t handler)
{
    s_touch_handler = handler;
}

void screen_draw_text(const char *text)
{
    xSemaphoreTake(s_draw_mutex, portMAX_DELAY);

    if (text) {
        strncpy(s_text, text, sizeof(s_text) - 1);
        s_text[sizeof(s_text) - 1] = '\0';
        s_scroll_line = 0;  /* new text — start at top */
    }

    static char all_lines[ALL_LINES_MAX][TEXT_COLS_MAX + 1];
    static int  all_len[ALL_LINES_MAX];
    int total_lines = 0;

    int max_cols = lcd_w()  / (8  * s_font_scale);
    int max_rows = lcd_h()  / (16 * s_font_scale);

    const char *src = s_text;
    while (*src && total_lines < ALL_LINES_MAX) {
        while (*src == ' ') src++;
        if (*src == '\0') break;
        if (*src == '\n') {
            all_lines[total_lines][0] = '\0';
            all_len[total_lines] = 0;
            total_lines++;
            src++;
            continue;
        }
        const char *nl = strchr(src, '\n');
        int line_len = nl ? (int)(nl - src) : (int)strlen(src);
        int avail    = max_cols;
        int take;
        if (line_len <= avail) {
            take = line_len;
        } else {
            take = avail;
            while (take > 0 && src[take] != ' ') take--;
            if (take == 0) take = avail;
        }
        memcpy(all_lines[total_lines], src, (size_t)take);
        all_lines[total_lines][take] = '\0';
        all_len[total_lines] = take;
        total_lines++;
        src += take;
        if (take == line_len && nl != NULL) src++;
    }

    if (total_lines == 0) {
        ESP_LOGW(TAG, "screen_draw_text: total_lines=0 for text='%.40s' — skip", s_text);
        xSemaphoreGive(s_draw_mutex);
        return;
    }

    s_scroll_total = total_lines;

    int start_y;
    int first, num_visible;
    int ch = 16 * s_font_scale;

    if (total_lines <= max_rows) {
        s_scroll_line = 0;
        first       = 0;
        num_visible = total_lines;
        start_y     = (lcd_h() - total_lines * ch) / 2;
    } else {
        int max_scroll = total_lines - max_rows;
        if (s_scroll_line < 0)          s_scroll_line = 0;
        if (s_scroll_line > max_scroll) s_scroll_line = max_scroll;
        first       = s_scroll_line;
        num_visible = max_rows;
        start_y     = 0;
    }

    uint16_t bg = ((uint16_t)(s_r & 0xF8) << 8)
                | ((uint16_t)(s_g & 0xFC) << 3)
                | (s_b >> 3);
    uint8_t bg_h = (uint8_t)(bg >> 8);
    uint8_t bg_l = (uint8_t)(bg & 0xFF);
    uint16_t fg  = ((uint16_t)(s_fr & 0xF8) << 8)
                 | ((uint16_t)(s_fg & 0xFC) << 3)
                 | (s_fb >> 3);
    uint8_t fg_h = (uint8_t)(fg >> 8);
    uint8_t fg_l = (uint8_t)(fg & 0xFF);

    ESP_LOGI(TAG, "screen_draw_text: lines=%d text='%.40s' bg=(%u,%u,%u)=0x%04X fg=(%u,%u,%u)=0x%04X scale=%d",
             total_lines, s_text, s_r, s_g, s_b, bg, s_fr, s_fg, s_fb, fg, s_font_scale);

    int logical_w = lcd_w();

    ili_set_window(0, 0, LCD_PHYS_W - 1, LCD_PHYS_H - 1);
    for (int y = 0; y < LCD_PHYS_H; y++) {
        for (int i = 0; i < LCD_PHYS_W * 2; i += 2) {
            s_row_buf[i]     = bg_h;
            s_row_buf[i + 1] = bg_l;
        }

        for (int x = 0; x < LCD_PHYS_W; x++) {
            int lx, ly;
            if (s_landscape) {
                lx = x;
                ly = y;
            } else {
                lx = (LCD_PHYS_H - 1) - y;
                ly = x;
            }

            if (text_pixel_is_fg(lx, ly,
                    (const char (*)[TEXT_COLS_MAX + 1])&all_lines[first],
                    &all_len[first], num_visible, logical_w, start_y)) {
                s_row_buf[x * 2]     = fg_h;
                s_row_buf[x * 2 + 1] = fg_l;
            }
        }

        ili_write_row();
        screen_full_redraw_yield(y);
    }

    ESP_LOGI(TAG, "screen_draw_text: done");
    xSemaphoreGive(s_draw_mutex);
}

void screen_reinit_display(void)
{
    /* Re-send the display init sequence without PMU reset control. */
    ESP_LOGI(TAG, "screen_reinit_display: re-initializing CYD SPI panel");

    if (LCD_RST >= 0) {
        gpio_set_level(LCD_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    xSemaphoreTake(s_draw_mutex, portMAX_DELAY);

    ili_cmd(0x01);                              /* SW_RESET          */
    xSemaphoreGive(s_draw_mutex);
    vTaskDelay(pdMS_TO_TICKS(150));

    xSemaphoreTake(s_draw_mutex, portMAX_DELAY);
    ili_cmd(0x11);                              /* SLEEP_OUT         */
    xSemaphoreGive(s_draw_mutex);
    vTaskDelay(pdMS_TO_TICKS(120));

    xSemaphoreTake(s_draw_mutex, portMAX_DELAY);
    ili9341_send_extended_init();
    ili_cmd(0x3A); ili_data_byte(0x55);         /* COLMOD: RGB565    */
    ili_cmd(0x36); ili_data_byte(MADCTL_LANDSCAPE);  /* MADCTL (portrait is software-rotated) */
    /* No INVERT_ON (0x21) — see screen_init() for why. */
    ili_cmd(0x29);                              /* DISPLAY_ON        */
    xSemaphoreGive(s_draw_mutex);
    cyd_backlight_set(true);

    ESP_LOGI(TAG, "screen_reinit_display: done");
}

void screen_draw_rgb565(const uint8_t *rgb565, int src_w, int src_h)
{
    xSemaphoreTake(s_draw_mutex, portMAX_DELAY);

    s_text[0] = '\0';

    int dst_w = lcd_w();
    int dst_h = lcd_h();

    int img_w, img_h, img_x0, img_y0;
    if (src_w * dst_h > src_h * dst_w) {
        img_w = dst_w;
        img_h = (src_h * dst_w) / src_w;
    } else {
        img_h = dst_h;
        img_w = (src_w * dst_h) / src_h;
    }
    img_x0 = (dst_w - img_w) / 2;
    img_y0 = (dst_h - img_h) / 2;

    uint16_t bg_px = ((uint16_t)(s_r & 0xF8) << 8)
                   | ((uint16_t)(s_g & 0xFC) << 3)
                   | (s_b >> 3);
    uint8_t bg_h = (uint8_t)(bg_px >> 8);
    uint8_t bg_l = (uint8_t)(bg_px & 0xFF);

    const uint16_t *src = (const uint16_t *)rgb565;

    ili_set_window(0, 0, LCD_PHYS_W - 1, LCD_PHYS_H - 1);
    for (int y = 0; y < LCD_PHYS_H; y++) {
        for (int i = 0; i < LCD_PHYS_W * 2; i += 2) {
            s_row_buf[i]     = bg_h;
            s_row_buf[i + 1] = bg_l;
        }

        for (int x = 0; x < LCD_PHYS_W; x++) {
            int lx, ly;
            if (s_landscape) {
                lx = x;
                ly = y;
            } else {
                lx = (LCD_PHYS_H - 1) - y;
                ly = x;
            }

            if (lx < img_x0 || lx >= img_x0 + img_w ||
                ly < img_y0 || ly >= img_y0 + img_h) {
                continue;
            }

            int src_col = ((lx - img_x0) * src_w) / img_w;
            int src_row = ((ly - img_y0) * src_h) / img_h;
            uint16_t pixel = src[src_row * src_w + src_col];
            s_row_buf[x * 2]     = (uint8_t)(pixel >> 8);
            s_row_buf[x * 2 + 1] = (uint8_t)(pixel & 0xFF);
        }

        ili_write_row();
        screen_full_redraw_yield(y);
    }

    xSemaphoreGive(s_draw_mutex);
}
