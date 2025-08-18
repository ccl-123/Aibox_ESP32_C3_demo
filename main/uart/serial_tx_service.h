#pragma once

#include <memory>
#include <mutex>
#include <cstdint>
#include "uart_port.h"

// 业务服务层：提供简单的发送接口，屏蔽底层 UART 细节
class SerialTxService {
public:
    SerialTxService() = default;
    ~SerialTxService() = default;

    // 使用默认配置(UART1, GPIO12, 9600 8N1) 初始化
    bool Init();
    bool SendByte(uint8_t value);
    bool SendBytes(const uint8_t* data, size_t len);
    void Deinit();

private:
    static constexpr const char* TAG = "SERIAL_TX";
    std::unique_ptr<UartPort> port_;
    std::mutex mutex_;
    bool initialized_ = false;
};

