#pragma once

#include <freertos/FreeRTOS.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <mutex>
#include <cstdint>

// 低耦合硬件抽象层：对 ESP-IDF UART 做最小封装
class UartPort {
public:
    struct Config {
        uart_port_t port = UART_NUM_1;        // 使用 UART1，避免与日志 UART0 冲突
        int tx_gpio = GPIO_NUM_12;            // 默认 TX 引脚：GPIO12 (ESP32-C3)
        int baud_rate = 9600;                 // 9600 波特
        uart_word_length_t data_bits = UART_DATA_8_BITS; // 8数据位
        uart_parity_t parity = UART_PARITY_DISABLE;      // 无校验
        uart_stop_bits_t stop_bits = UART_STOP_BITS_1;   // 1停止位
        uart_hw_flowcontrol_t flow_ctrl = UART_HW_FLOWCTRL_DISABLE; // 关闭流控
        size_t tx_buffer_size = 256;          // 发送缓冲（轻量）
    };

    UartPort() = default;
    ~UartPort();

    // 初始化 UART；仅配置 TX，不使用 RX
    bool Init(const Config& cfg);

    // 同步发送数据；内部做互斥保护
    bool Send(const uint8_t* data, size_t len, TickType_t wait_ticks = pdMS_TO_TICKS(200));

    // 释放资源
    void Deinit();

private:
    static constexpr const char* TAG = "UART_PORT";
    Config config_{};
    bool initialized_ = false;
    std::mutex mutex_;
};

