/**
 * INA226 驱动实现（ESP-IDF 5.2 legacy I2C API）
 * 寄存器：
 *   0x00 Configuration = 0x4127（AVG=16, VBUSCT=1.1ms, VSHCT=1.1ms, 连续 shunt+bus）
 *   0x05 Calibration  = 51200（10mΩ 分流 → 满量程 1A）
 *   0x01 Shunt 电压   = raw × 2.5uV → I = uV / 10mΩ
 *   0x02 Bus 电压     = raw × 1.25mV
 */
#include "ina226.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define INA226_ADDR        0x40
#define INA226_REG_CFG     0x00
#define INA226_REG_SHUNT   0x01
#define INA226_REG_BUS     0x02
#define INA226_REG_CAL     0x05

#define INA226_CFG_VAL     0x4127
#define INA226_CAL_VAL     51200

#define I2C_FREQ_HZ        400000
#define I2C_TIMEOUT_MS     50

static const char *TAG = "ina226";
static int s_port = -1;

static esp_err_t ina226_write_reg(uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_master_write_to_device(s_port, INA226_ADDR, buf, sizeof(buf),
                                      I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
}

static esp_err_t ina226_read_reg(uint8_t reg, uint16_t *val)
{
    esp_err_t err = i2c_master_write_to_device(s_port, INA226_ADDR, &reg, 1,
                                               I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (err != ESP_OK) return err;
    uint8_t buf[2];
    err = i2c_master_read_from_device(s_port, INA226_ADDR, buf, 2,
                                      I2C_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (err != ESP_OK) return err;
    *val = ((uint16_t)buf[0] << 8) | buf[1];
    return ESP_OK;
}

bool ina226_init(int sda_gpio, int scl_gpio, int i2c_port)
{
    s_port = i2c_port;
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(s_port, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = i2c_driver_install(s_port, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }

    /* 写 Config + Calibration，随后回读 Config 验证设备在线 */
    ina226_write_reg(INA226_REG_CFG, INA226_CFG_VAL);
    ina226_write_reg(INA226_REG_CAL, INA226_CAL_VAL);
    uint16_t rd = 0;
    if (ina226_read_reg(INA226_REG_CFG, &rd) != ESP_OK || rd != INA226_CFG_VAL) {
        ESP_LOGW(TAG, "INA226 not detected (cfg read=0x%04X)", rd);
        return false;
    }
    ESP_LOGI(TAG, "INA226 online @0x%02X (cfg=0x%04X)", INA226_ADDR, rd);
    return true;
}

bool ina226_read(ina226_data_t *d)
{
    uint16_t bus_raw, shunt_raw;
    if (ina226_read_reg(INA226_REG_BUS, &bus_raw) != ESP_OK) return false;
    if (ina226_read_reg(INA226_REG_SHUNT, &shunt_raw) != ESP_OK) return false;

    d->volt  = (float)bus_raw * 1.25e-3f;            /* 总线电压 V */
    d->curr  = (float)(int16_t)shunt_raw * 0.25e-3f; /* I = 2.5uV/10mΩ = 0.25mA/LSB */
    d->power = d->volt * d->curr;                    /* 功率 W */
    return true;
}
