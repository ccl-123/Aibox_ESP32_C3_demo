#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000   //改16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000   //改16000

#define WS2812_GPIO GPIO_NUM_2

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_8
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_5
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_7
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_4

#define AUDIO_CODEC_USE_PCA9557
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_10
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_0
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_1
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR  0x82

#define BUILTIN_LED_GPIO        GPIO_NUM_NC
#define BOOT_BUTTON_GPIO        GPIO_NUM_9
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_NC
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_NC

// 本项目硬件无显示屏，GPIO3 可用于 AW9523 RSTN
#define DISPLAY_SPI_SCK_PIN     GPIO_NUM_NC
#define DISPLAY_SPI_MOSI_PIN    GPIO_NUM_NC
#define DISPLAY_DC_PIN          GPIO_NUM_NC
#define DISPLAY_SPI_CS_PIN      GPIO_NUM_NC

#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY true

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_2
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true

// ---------------- AW9523B IO 扩展配置 ----------------
// AW9523B I2C地址：根据ADDR引脚电平确定
// ADDR接地: 0x5B, ADDR接VCC: 0x5A
// 实际检测到的地址是0x58
#define AW9523_I2C_ADDR   0x58
#define AW9523_RST_GPIO   GPIO_NUM_3
// VDD_SPI配置为GPIO11用作中断引脚
// 注意：需要确保SPI Flash与芯片共用VDD3P3_CPU供电
#define AW9523_INT_GPIO   GPIO_NUM_11    // VDD_SPI配置为GPIO11（中断引脚）

// 方向寄存器建议配置：1=输入, 0=输出
// P0: 按钮 SUCK(P0_0)/ON(P0_1)/ROCK(P0_2)/VOL(P0_3) 输入，其余输出
#define AW9523_CONFIG_P0  0x0F  // P0_0,P0_1,P0_2,P0_3 输入
// P1: 全部输出（电机/加热器控制）
#define AW9523_CONFIG_P1  0x00

// 中断掩码：1 屏蔽，0 允许中断（仅对 P0_0/1/2/3 允许中断）
#define AW9523_INTMASK_P0 0xF0  // ~0x0F
#define AW9523_INTMASK_P1 0xFF

// 按键极性配置：根据原理图分析，按键按下时为高电平（上拉电阻）
// 按键未按下时为低电平，按下时为高电平 - 因此不启用BUTTON_ACTIVE_LOW
// #define BUTTON_ACTIVE_LOW  // 已禁用，使用高电平有效

#endif // _BOARD_CONFIG_H_
