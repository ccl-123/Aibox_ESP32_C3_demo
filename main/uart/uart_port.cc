#include "uart_port.h"

#include <driver/uart.h>
#include <driver/gpio.h>
#include <cstring>

UartPort::~UartPort() {
    Deinit();
}

bool UartPort::Init(const Config& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        Deinit();
    }

    config_ = cfg;

    uart_config_t uart_config = {
        .baud_rate = config_.baud_rate,
        .data_bits = config_.data_bits,
        .parity = config_.parity,
        .stop_bits = config_.stop_bits,
        .flow_ctrl = config_.flow_ctrl,
#if SOC_UART_SUPPORT_REF_TICK
        .source_clk = UART_SCLK_REF_TICK,
#else
        .source_clk = UART_SCLK_APB,
#endif
    };

    // 注意：ESP-IDF 在部分芯片上要求 rx_buffer_size > UART_FIFO_LEN，否则会报 "rx buffer length error"
    // C3 的 UART_FIFO_LEN 通常为 128 字节，这里给 256 字节以确保通过参数校验；仍然不绑定 RX 引脚，不使用接收功能
    const int rx_buffer_size = 256;
    esp_err_t err = uart_driver_install(config_.port, rx_buffer_size, (int)config_.tx_buffer_size, 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }

    err = uart_param_config(config_.port, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        uart_driver_delete(config_.port);
        return false;
    }

    err = uart_set_pin(config_.port, /*tx*/config_.tx_gpio, /*rx*/UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(config_.port);
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "Initialized: port=%d tx_gpio=%d baud=%d format=8N1", (int)config_.port, config_.tx_gpio, config_.baud_rate);
    return true;
}

bool UartPort::Send(const uint8_t* data, size_t len, TickType_t wait_ticks) {
    if (!initialized_) {
        ESP_LOGE(TAG, "Send failed: not initialized");
        return false;
    }
    if (!data || len == 0) return true;

    std::lock_guard<std::mutex> lock(mutex_);
    int written = uart_write_bytes(config_.port, (const char*)data, len);
    if (written < 0) {
        ESP_LOGE(TAG, "uart_write_bytes failed");
        return false;
    }
    esp_err_t err = uart_wait_tx_done(config_.port, wait_ticks);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_wait_tx_done: %s", esp_err_to_name(err));
        // 认为已提交，返回 true，但打印警告
    }
    ESP_LOGD(TAG, "TX %d bytes", written);
    return true;
}

void UartPort::Deinit() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return;

    uart_driver_delete(config_.port);
    initialized_ = false;
    ESP_LOGI(TAG, "Deinitialized: port=%d", (int)config_.port);
}

