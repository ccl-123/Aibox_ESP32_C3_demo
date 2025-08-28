#include "uart_rx.h"
#include <esp_log.h>
#include <string.h>
#include <driver/uart.h>

#define TAG "UART_RX"

// 全局变量定义
char uart_rx_button_value = '\0';              // 按键值字符
int uart_rx_button_value_int = 0;              // 按键值整数
bool uart_rx_key_press = false;                // 按键按下标志

// 按键消抖相关变量
static uint8_t uart_rx_key_count = 0;          // 按键计数
static uint8_t uart_rx_key_count_now = 1;      // 当前按键计数

/**
 * @brief 初始化UART RX功能
 */
// 全局标志，指示UART是否成功初始化
static bool uart_initialized = false;

void UART_RX_Init(void) {
    ESP_LOGI(TAG, "Initializing UART RX on GPIO%d...", UART_RX_PIN);

    // 尝试多种UART配置方案
    const uart_sclk_t clock_sources[] = {
        UART_SCLK_XTAL,     // 首选：XTAL时钟源
        UART_SCLK_APB,      // 备选：APB时钟源
        UART_SCLK_DEFAULT   // 最后：默认时钟源
    };
    
    const char* clock_names[] = {"XTAL", "APB", "DEFAULT"};
    
    for (int i = 0; i < 3; i++) {
        ESP_LOGI(TAG, "Attempting UART config with %s clock source...", clock_names[i]);
        
        uart_config_t uart_config;
        memset(&uart_config, 0, sizeof(uart_config));
        uart_config.baud_rate = UART_RX_BAUDRATE;
        uart_config.data_bits = UART_DATA_8_BITS;
        uart_config.parity = UART_PARITY_DISABLE;
        uart_config.stop_bits = UART_STOP_BITS_1;
        uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        uart_config.source_clk = clock_sources[i];
        uart_config.rx_flow_ctrl_thresh = 0;

        esp_err_t ret = uart_param_config(UART_RX_PORT_NUM, &uart_config);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "UART config successful with %s clock source", clock_names[i]);
            break;
        } else {
            ESP_LOGW(TAG, "UART config failed with %s clock source: %s", 
                     clock_names[i], esp_err_to_name(ret));
            
            if (i == 2) {  // 最后一次尝试失败
                ESP_LOGE(TAG, "All UART configuration attempts failed. UART RX功能将不可用");
                uart_initialized = false;
                return;
            }
        }
    }

    // 设置引脚 - 只设置RX引脚，TX引脚设为-1(不使用)
    esp_err_t ret = uart_set_pin(UART_RX_PORT_NUM, 
                                  UART_PIN_NO_CHANGE,  // TX引脚不使用
                                  UART_RX_PIN,         // RX引脚GPIO10
                                  UART_PIN_NO_CHANGE,  // RTS引脚不使用
                                  UART_PIN_NO_CHANGE   // CTS引脚不使用
                                 );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART pin configuration failed: %s", esp_err_to_name(ret));
        uart_initialized = false;
        return;
    }

    // 安装UART驱动 - 只需要RX缓冲区
    ret = uart_driver_install(UART_RX_PORT_NUM,
                              UART_RX_BUFFER_SIZE,  // RX缓冲区大小
                              0,                     // TX缓冲区设为0(不使用)
                              0,                     // 事件队列大小
                              NULL,                  // 无事件队列
                              0                      // 中断分配标志
                             );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART driver installation failed: %s", esp_err_to_name(ret));
        uart_initialized = false;
        return;
    }

    uart_initialized = true;
    ESP_LOGI(TAG, "UART RX initialized successfully");
}

/**
 * @brief 检查UART是否成功初始化
 * @return true 如果UART已成功初始化, false 否则
 */
bool UART_RX_IsInitialized(void) {
    return uart_initialized;
}

/**
 * @brief 串口数据接收和处理函数
 */
void UART_RX_DATA(void) {
    // 检查UART是否已成功初始化
    if (!uart_initialized) {
        return; // UART未初始化，直接返回避免错误
    }
    
    size_t len = 0;
    uint8_t uart_rx_data[UART_RX_BUFFER_SIZE] = {0};

    // 获取缓冲区中的数据长度
    esp_err_t result = uart_get_buffered_data_len(UART_RX_PORT_NUM, &len);
    if (result != ESP_OK) {
        return; // 获取数据长度失败，直接返回
    }

    if (len > 0) {
        // 增加按键计数(用于消抖)
        uart_rx_key_count++;

        // 清空数据缓冲区
        memset(uart_rx_data, 0, UART_RX_BUFFER_SIZE);

        // 读取接收到的数据
        int read_len = uart_read_bytes(UART_RX_PORT_NUM, 
                                      uart_rx_data, 
                                      len, 
                                      100 / portTICK_PERIOD_MS);

        if (read_len > 0) {
            ESP_LOGI(TAG, "Received %d bytes: %s", read_len, uart_rx_data);

            // 数据解析 - 查找固定帧头 "LC:"
            const char* frame_header = "LC:";
            char* frame_data = (char*)uart_rx_data;
            char* header_position = strstr(frame_data, frame_header);

            if (header_position != NULL) {
                // 提取按键值部分 (帧头后5个字符为地址，第6个字符为按键值)
                char* button_value_str = header_position + strlen(frame_header) + 5;
                uart_rx_button_value = *button_value_str;
                
                ESP_LOGI(TAG, "Button Value: %c", uart_rx_button_value);

                // 16进制字符转10进制数字
                if (uart_rx_button_value >= '0' && uart_rx_button_value <= '9') {
                    uart_rx_button_value_int = uart_rx_button_value - '0';  // '0'~'9' -> 0~9
                } else if (uart_rx_button_value >= 'A' && uart_rx_button_value <= 'F') {
                    uart_rx_button_value_int = uart_rx_button_value - 'A' + 10;  // 'A'~'F' -> 10~15
                } else if (uart_rx_button_value >= 'a' && uart_rx_button_value <= 'f') {
                    uart_rx_button_value_int = uart_rx_button_value - 'a' + 10;  // 'a'~'f' -> 10~15
                } else {
                    ESP_LOGW(TAG, "Invalid button character: %c", uart_rx_button_value);
                    uart_rx_button_value_int = -1;  // 无效值
                }

                ESP_LOGI(TAG, "Button hex: %c, decimal: %d", 
                        uart_rx_button_value, uart_rx_button_value_int);
            } else {
                ESP_LOGW(TAG, "Frame header 'LC:' not found in received data");
            }

            // 按键消抖处理
            if (uart_rx_key_count_now != uart_rx_key_count) {
                ESP_LOGI(TAG, "Key press detected - Count: %d -> %d", 
                        uart_rx_key_count_now, uart_rx_key_count);
                uart_rx_key_press = true;
                uart_rx_key_count_now = uart_rx_key_count;
            } else {
                uart_rx_key_press = false;
            }
        }
    }
}
