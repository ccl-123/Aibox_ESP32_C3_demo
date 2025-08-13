#include "aw9523.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "AW9523";

Aw9523::Aw9523(i2c_master_bus_handle_t i2c_bus, uint8_t addr)
    : I2cDevice(i2c_bus, addr) {
}

esp_err_t Aw9523::reset_pulse_(gpio_num_t rst_gpio) {
    if (rst_gpio == GPIO_NUM_NC) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Reset pulse on gpio %d", (int)rst_gpio);
    gpio_config_t io_cfg = {};
    io_cfg.pin_bit_mask = 1ULL << rst_gpio;
    io_cfg.mode = GPIO_MODE_OUTPUT;
    io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    io_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    gpio_set_level(rst_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(rst_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

esp_err_t Aw9523::Init(gpio_num_t rst_gpio,
                       uint8_t config_p0,
                       uint8_t config_p1,
                       bool set_gcr_pushpull) {
    esp_err_t err = reset_pulse_(rst_gpio);
    if (err != ESP_OK) return err;

    // 全部 GPIO 模式（需求：LED 不用 LED 模式）
    WriteReg(REG_LEDMODE_P0, 0x00);
    WriteReg(REG_LEDMODE_P1, 0x00);

    if (set_gcr_pushpull) {
        // 0x10: P0 推挽输出（按项目中示例）
        WriteReg(REG_GCR, 0x10);
    }

    // 配置方向：1 输入，0 输出
    WriteReg(REG_CONFIG_P0, config_p0);
    WriteReg(REG_CONFIG_P1, config_p1);

    // 缺省关闭所有输出
    WriteReg(REG_OUTPUT_P0, 0x00);
    WriteReg(REG_OUTPUT_P1, 0x00);

    ESP_LOGI(TAG, "AW9523 initialized: cfg_p0=0x%02x cfg_p1=0x%02x", config_p0, config_p1);
    return ESP_OK;
}

esp_err_t Aw9523::pin_mode(uint8_t port, uint8_t bit, bool input) {
    uint8_t reg = (port == 0) ? REG_CONFIG_P0 : REG_CONFIG_P1;
    uint8_t val = ReadReg(reg);
    if (input) val |= (1u << bit); else val &= ~(1u << bit);
    WriteReg(reg, val);
    return ESP_OK;
}

esp_err_t Aw9523::digital_write(uint8_t port, uint8_t bit, bool level) {
    uint8_t reg = (port == 0) ? REG_OUTPUT_P0 : REG_OUTPUT_P1;
    uint8_t val = ReadReg(reg);
    if (level) val |= (1u << bit); else val &= ~(1u << bit);
    WriteReg(reg, val);
    return ESP_OK;
}

esp_err_t Aw9523::digital_read(uint8_t port, uint8_t bit, bool* level) {
    if (level == nullptr) return ESP_ERR_INVALID_ARG;
    uint8_t reg = (port == 0) ? REG_INPUT_P0 : REG_INPUT_P1;
    uint8_t val = ReadReg(reg);
    *level = (val >> bit) & 0x1;
    return ESP_OK;
}

esp_err_t Aw9523::read_inputs(uint8_t* p0, uint8_t* p1) {
    if (!p0 || !p1) return ESP_ERR_INVALID_ARG;
    *p0 = ReadReg(REG_INPUT_P0);
    *p1 = ReadReg(REG_INPUT_P1);
    return ESP_OK;
}

esp_err_t Aw9523::write_outputs(uint8_t port, uint8_t value) {
    uint8_t reg = (port == 0) ? REG_OUTPUT_P0 : REG_OUTPUT_P1;
    WriteReg(reg, value);
    return ESP_OK;
}

esp_err_t Aw9523::set_int_mask(uint8_t port, uint8_t mask) {
    uint8_t reg = (port == 0) ? REG_INTMSK_P0 : REG_INTMSK_P1;
    WriteReg(reg, mask);
    return ESP_OK;
}

esp_err_t Aw9523::read_outputs(uint8_t* p0, uint8_t* p1) {
    *p0 = ReadReg(REG_OUTPUT_P0);
    *p1 = ReadReg(REG_OUTPUT_P1);
    return ESP_OK;
}

esp_err_t Aw9523::read_config(uint8_t* p0, uint8_t* p1) {
    *p0 = ReadReg(REG_CONFIG_P0);
    *p1 = ReadReg(REG_CONFIG_P1);
    return ESP_OK;
}

esp_err_t Aw9523::read_int_mask(uint8_t* p0, uint8_t* p1) {
    *p0 = ReadReg(REG_INTMSK_P0);
    *p1 = ReadReg(REG_INTMSK_P1);
    return ESP_OK;
}

