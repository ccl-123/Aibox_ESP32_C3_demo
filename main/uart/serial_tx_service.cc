#include "serial_tx_service.h"
#include <esp_log.h>

bool SerialTxService::Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        Deinit();
    }

    port_ = std::make_unique<UartPort>();
    UartPort::Config cfg; // 使用默认：UART1 / GPIO12 / 9600 8N1
    if (!port_->Init(cfg)) {
        ESP_LOGE(TAG, "Init failed");
        port_.reset();
        return false;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "SerialTxService initialized");
    return true;
}

bool SerialTxService::SendByte(uint8_t value) {
    return SendBytes(&value, 1);
}

bool SerialTxService::SendBytes(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !port_) {
        ESP_LOGE(TAG, "Send failed: not initialized");
        return false;
    }
    return port_->Send(data, len);
}

void SerialTxService::Deinit() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (port_) {
        port_->Deinit();
        port_.reset();
    }
    initialized_ = false;
    ESP_LOGI(TAG, "SerialTxService deinitialized");
}

