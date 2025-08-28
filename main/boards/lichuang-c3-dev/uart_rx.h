#ifndef __UART_RX_H__
#define __UART_RX_H__

#include "driver/uart.h"
#include "esp_log.h"
#include <functional>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "freertos/queue.h"

// UART配置定义 - ESP32-C3串口接收
#define UART_RX_PORT_NUM        UART_NUM_1          // 使用UART1
#define UART_RX_PIN             GPIO_NUM_10         // ESP32-C3 GPIO10作为RX引脚
#define UART_RX_BUFFER_SIZE     256                 // 接收缓冲区大小
#define UART_RX_BAUDRATE        9600                // 波特率设置为9600

// 全局变量声明
extern char uart_rx_button_value;              // 按键值字符
extern int uart_rx_button_value_int;           // 按键值整数
extern bool uart_rx_key_press;                 // 按键按下标志

#ifdef __cplusplus
extern "C" {
#endif

// 函数声明
void UART_RX_Init(void);                       // 初始化串口RX
void UART_RX_DATA(void);                       // 接收串口数据处理
bool UART_RX_IsInitialized(void);              // 检查UART是否成功初始化

#ifdef __cplusplus
}
#endif

#endif // __UART_RX_H__
