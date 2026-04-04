#include "mpu6050.h"
#include "sdkconfig.h"

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mpu6050";

/* ── I2C address ─────────────────────────────────────────────────────────── */
#if CONFIG_MPU6050_AD0_HIGH
#define MPU6050_ADDR   0x69
#else
#define MPU6050_ADDR   0x68
#endif

/* ── Register map ────────────────────────────────────────────────────────── */
#define REG_MOT_THR      0x1F   /* Motion detection threshold        */
#define REG_MOT_DUR      0x20   /* Motion detection duration (ms)    */
#define REG_INT_PIN_CFG  0x37   /* INT pin / bypass enable config    */
#define REG_INT_ENABLE   0x38   /* Interrupt enable                  */
#define REG_INT_STATUS   0x3A   /* Interrupt status (read clears)    */
#define REG_PWR_MGMT_1   0x6B   /* Power management 1                */
#define REG_PWR_MGMT_2   0x6C   /* Power management 2                */
#define REG_WHO_AM_I     0x75   /* Device identity (always 0x68)     */

#define I2C_TIMEOUT_MS   50

/* ── Private helpers ─────────────────────────────────────────────────────── */

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(CONFIG_MPU6050_I2C_PORT, MPU6050_ADDR,
                                      buf, sizeof(buf),
                                      pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(CONFIG_MPU6050_I2C_PORT, MPU6050_ADDR,
                                        &reg, 1, val, 1,
                                        pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t mpu6050_init(void)
{
    /* (Re-)install the I2C master driver — peripherals are reset after deep
     * sleep so the driver must be reinstalled on every wakeup.              */
    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = CONFIG_MPU6050_SDA_GPIO,
        .scl_io_num       = CONFIG_MPU6050_SCL_GPIO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    esp_err_t ret = i2c_param_config(CONFIG_MPU6050_I2C_PORT, &cfg);
    if (ret != ESP_OK) return ret;

    ret = i2c_driver_install(CONFIG_MPU6050_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    /* ESP_ERR_INVALID_STATE means the driver was already installed – OK.    */
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    /* Verify chip identity.  WHO_AM_I is always 0x68 on genuine MPU-6050;
     * common clone chips may return other values — warn but continue.       */
    uint8_t who = 0;
    ret = read_reg(REG_WHO_AM_I, &who);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (who != 0x68) {
        ESP_LOGW(TAG, "WHO_AM_I=0x%02X (expected 0x68, may be clone)", who);
    } else {
        ESP_LOGI(TAG, "MPU6050 found at 0x%02X", MPU6050_ADDR);
    }

    /* Wake from sleep, disable temperature sensor, use internal oscillator. */
    ret = write_reg(REG_PWR_MGMT_1, 0x08);   /* TEMP_DIS=1, SLEEP=0, CYCLE=0 */
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));            /* allow oscillator to settle   */

    /* Disable all three gyroscope axes to save power (not needed for
     * motion detection or activity tracking).                               */
    ret = write_reg(REG_PWR_MGMT_2, 0x07);   /* STBY_XG | STBY_YG | STBY_ZG */
    if (ret != ESP_OK) return ret;

    /* Configure INT pin: active-high, push-pull, latched (stays high until
     * INT_STATUS is read), cleared only by reading INT_STATUS.              */
    ret = write_reg(REG_INT_PIN_CFG, 0x20);   /* LATCH_INT_EN=1               */
    if (ret != ESP_OK) return ret;

    /* Enable motion-detect interrupt only.                                  */
    ret = write_reg(REG_INT_ENABLE, 0x40);    /* MOT_EN=1                     */
    if (ret != ESP_OK) return ret;

    /* Motion threshold: ~32 mg at ±2 g full-scale range.                   */
    ret = write_reg(REG_MOT_THR, 0x08);
    if (ret != ESP_OK) return ret;

    /* Motion must persist for 1 ms to trigger interrupt.                   */
    ret = write_reg(REG_MOT_DUR, 0x01);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "MPU6050 initialised (full-power, gyro standby)");
    return ESP_OK;
}

esp_err_t mpu6050_configure_wakeup(void)
{
    /* PWR_MGMT_2: LP_WAKE_CTRL = 5 Hz (bits [7:6] = 0b01), all gyros standby.
     * Bit pattern:  0b 01 000 111  = 0x47                                   */
    esp_err_t ret = write_reg(REG_PWR_MGMT_2, 0x47);
    if (ret != ESP_OK) return ret;

    /* PWR_MGMT_1: CYCLE=1, SLEEP=0, TEMP_DIS=1.
     * Bit pattern:  0b 0 0 1 0 1 0 0 0  = 0x28                             */
    ret = write_reg(REG_PWR_MGMT_1, 0x28);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "MPU6050 in low-power cycle mode (5 Hz), wakeup armed");
    return ESP_OK;
}

void mpu6050_clear_interrupt(void)
{
    uint8_t status = 0;
    read_reg(REG_INT_STATUS, &status);   /* Reading clears the latch.        */
    (void)status;
}

bool mpu6050_is_active(void)
{
    uint8_t status = 0;
    read_reg(REG_INT_STATUS, &status);
    return (status & 0x40) != 0;         /* MOT_INT bit                      */
}
