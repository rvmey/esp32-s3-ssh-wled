#include "mpu6886.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mpu6886";

#define MPU6886_ADDR        0x68
#define MPU6886_I2C_PORT    I2C_NUM_0

/* ── Register map ────────────────────────────────────────────────────────── */
#define REG_ACCEL_CONFIG2   0x1D
#define REG_WOM_THR         0x1F
#define REG_INT_PIN_CFG     0x37
#define REG_INT_ENABLE      0x38
#define REG_INT_STATUS      0x3A
#define REG_ACCEL_INTEL_CTRL 0x69
#define REG_PWR_MGMT_1      0x6B
#define REG_PWR_MGMT_2      0x6C
#define REG_WHO_AM_I        0x75

#define MPU6886_WHO_AM_I_VAL 0x19

/* portMAX_DELAY: BT A2DP + WiFi coexistence can delay the I2C ISR 50+ ms;
 * a finite timeout would trigger i2c_hw_fsm_reset which deadlocks with the
 * ISR on the driver spinlock (same pattern as AXP192 / FT6336U reads). */
static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_write_to_device(MPU6886_I2C_PORT, MPU6886_ADDR,
                                      buf, sizeof(buf), portMAX_DELAY);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(MPU6886_I2C_PORT, MPU6886_ADDR,
                                        &reg, 1, val, 1, portMAX_DELAY);
}

esp_err_t mpu6886_init(void)
{
    /* Hard reset — clears all registers including any armed WOM config. */
    esp_err_t ret = write_reg(REG_PWR_MGMT_1, 0x80);   /* DEVICE_RESET */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "reset write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Wake from sleep, internal 20 MHz oscillator. */
    ret = write_reg(REG_PWR_MGMT_1, 0x00);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t who = 0;
    ret = read_reg(REG_WHO_AM_I, &who);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (who == MPU6886_WHO_AM_I_VAL) {
        ESP_LOGI(TAG, "MPU6886 found (WHO_AM_I=0x%02X)", who);
    } else {
        ESP_LOGW(TAG, "WHO_AM_I=0x%02X (expected 0x%02X)", who, MPU6886_WHO_AM_I_VAL);
    }

    /* Gyro axes standby — only accelerometer needed for WOM. */
    ret = write_reg(REG_PWR_MGMT_2, 0x07);   /* STBY_XG | STBY_YG | STBY_ZG */
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "MPU6886 initialised (full-power, gyro standby)");
    return ESP_OK;
}

esp_err_t mpu6886_configure_wom(uint8_t threshold)
{
    esp_err_t ret;

    /* Disable interrupts while reconfiguring to prevent spurious fires. */
    ret = write_reg(REG_INT_ENABLE, 0x00);
    if (ret != ESP_OK) return ret;

    /* Wake to full power before switching to cycle mode. */
    ret = write_reg(REG_PWR_MGMT_1, 0x00);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(5));

    /* DLPF off for accelerometer — required for WOM to function correctly. */
    ret = write_reg(REG_ACCEL_CONFIG2, 0x00);
    if (ret != ESP_OK) return ret;

    /* WOM threshold: 1 LSB = ~4 mg. */
    ret = write_reg(REG_WOM_THR, threshold);
    if (ret != ESP_OK) return ret;

    /* Enable accelerometer intelligence: compare current to previous sample. */
    ret = write_reg(REG_ACCEL_INTEL_CTRL, 0xC0);   /* ACCEL_INTEL_EN | ACCEL_INTEL_MODE */
    if (ret != ESP_OK) return ret;

    /* INT pin: active-high, push-pull, latched until INT_STATUS is read. */
    ret = write_reg(REG_INT_PIN_CFG, 0x20);         /* LATCH_INT_EN */
    if (ret != ESP_OK) return ret;

    /* Enable WOM interrupt only. */
    ret = write_reg(REG_INT_ENABLE, 0x40);          /* WOM_EN */
    if (ret != ESP_OK) return ret;

    /* Low-power wake rate 5 Hz; gyro axes in standby. */
    ret = write_reg(REG_PWR_MGMT_2, 0x47);          /* LP_WAKE_CTRL=01 | STBY_XYZ_G */
    if (ret != ESP_OK) return ret;

    /* Enter cycle mode (low-power accel polling), gyro standby, temp disabled. */
    ret = write_reg(REG_PWR_MGMT_1, 0x28);          /* CYCLE | GYRO_STANDBY | TEMP_DIS */
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "MPU6886 WOM armed (threshold=%u, ~%u mg)",
             (unsigned)threshold, (unsigned)threshold * 4u);
    return ESP_OK;
}

void mpu6886_clear_interrupt(void)
{
    uint8_t status = 0;
    read_reg(REG_INT_STATUS, &status);   /* Reading INT_STATUS deasserts INT pin. */
    (void)status;
}
