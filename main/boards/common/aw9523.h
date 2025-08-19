#ifndef AW9523_H
#define AW9523_H

#include <driver/gpio.h>
#include "i2c_device.h"

// AW9523B 简易驱动：GPIO 模式优先，满足按钮输入与负载开关控制
// 参考芯片手册与项目中 m5stack-core-s3 示例
class Aw9523 : public I2cDevice {
public:
    // 构造：仅保存设备句柄，不做寄存器初始化
    Aw9523(i2c_master_bus_handle_t i2c_bus, uint8_t addr);

    // 初始化：
    // - 可选 rst 脚低脉冲复位
    // - 配置 LEDMODE_P0/P1 为 0（GPIO 模式）
    // - 配置 GCR 使 P0 端口推挽（按需求）
    // - 根据入参设置方向寄存器 CONFIG_P0/CONFIG_P1
    esp_err_t Init(gpio_num_t rst_gpio,
                   uint8_t config_p0,
                   uint8_t config_p1,
                   bool set_gcr_pushpull = true);

    // 单个管脚方向配置与读写
    esp_err_t pin_mode(uint8_t port /*0 or 1*/, uint8_t bit /*0..7*/, bool input);
    esp_err_t digital_write(uint8_t port, uint8_t bit, bool level);
    esp_err_t digital_read(uint8_t port, uint8_t bit, bool* level);

    // 批量寄存器访问（便于中断服务后的快速轮询）
    esp_err_t read_inputs(uint8_t* p0, uint8_t* p1);
    esp_err_t read_outputs(uint8_t* p0, uint8_t* p1);
    esp_err_t read_config(uint8_t* p0, uint8_t* p1);
    esp_err_t read_int_mask(uint8_t* p0, uint8_t* p1);
    esp_err_t write_outputs(uint8_t port, uint8_t value);

    // 中断屏蔽配置：mask=1 表示屏蔽，0 表示产生中断（请按实际数据手册确认）
    esp_err_t set_int_mask(uint8_t port, uint8_t mask);
    
    // 上拉使能配置：1=启用内部上拉，0=禁用上拉
    esp_err_t set_pullup_enable(uint8_t port, uint8_t pullup_mask);
    esp_err_t read_pullup_enable(uint8_t* p0, uint8_t* p1);
    


private:
    // 常量寄存器地址
    static constexpr uint8_t REG_INPUT_P0   = 0x00;
    static constexpr uint8_t REG_INPUT_P1   = 0x01;
    static constexpr uint8_t REG_OUTPUT_P0  = 0x02;
    static constexpr uint8_t REG_OUTPUT_P1  = 0x03;
    static constexpr uint8_t REG_CONFIG_P0  = 0x04;
    static constexpr uint8_t REG_CONFIG_P1  = 0x05;
    static constexpr uint8_t REG_INTMSK_P0  = 0x06;
    static constexpr uint8_t REG_INTMSK_P1  = 0x07;
    static constexpr uint8_t REG_GCR        = 0x11;
    static constexpr uint8_t REG_LEDMODE_P0 = 0x12;
    static constexpr uint8_t REG_LEDMODE_P1 = 0x13;
    // ❌ 移除保留寄存器定义 (14H~1FH为保留范围，不可操作)
    // 根据AW9523B数据手册，14H~1FH范围为保留寄存器，用户不可操作
    // 违规使用这些寄存器会导致功能错误，这是输入检测失效的根本原因！

    esp_err_t reset_pulse_(gpio_num_t rst_gpio);
};

#endif // AW9523_H

