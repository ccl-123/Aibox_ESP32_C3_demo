#include "wifi_board.h"
#include "audio_codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "aw9523.h"
#include "iot/thing_manager.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/gpio.h>
#include <wifi_station.h>
#include "led/single_led.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#define TAG "LichuangC3DevBoard"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);

class LichuangC3DevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button boot_button_;
    LcdDisplay* display_;
    SingleLed* led_;

    // AW9523 IO 扩展
    Aw9523* aw9523_ = nullptr;
    QueueHandle_t aw_int_queue_ = nullptr; // 中断事件队列
    TaskHandle_t aw_btn_task_ = nullptr;   // 按钮去抖任务

    // 按钮位定义（端口/位）
    static constexpr uint8_t P0_BTN_SUCK_BIT = 0; // P0_0
    static constexpr uint8_t P0_BTN_ON_BIT   = 1; // P0_1
    static constexpr uint8_t P0_BTN_VOL_BIT  = 7; // P0_7

    // 去抖状态
    uint8_t p0_input_last_ = 0;   // 最近一次读取
    uint8_t p0_input_stable_ = 0; // 稳定值
    uint8_t debounce_cnt_ = 0;


    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));

        // 调试：扫描 I2C 设备
        I2cDetect();
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SPI_SCK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
    }

    void InitializeSt7789Display() {
        // 无显示屏，跳过初始化
        ESP_LOGI(TAG, "No display attached, skip ST7789 init");
        display_ = nullptr;
    }

    void InitializeAw9523() {
        ESP_LOGI(TAG, "初始化AW9523B IO扩展芯片...");
        ESP_LOGI(TAG, "硬件配置: RSTN=GPIO%d, INTN=GPIO%d, I2C地址=0x%02X",
                 AW9523_RST_GPIO, AW9523_INT_GPIO, AW9523_I2C_ADDR);

        // 实例化与初始化 AW9523
        aw9523_ = new Aw9523(codec_i2c_bus_, AW9523_I2C_ADDR);
        // 方向配置：按功能映射（P0: 0x83 输入位; P1: 全输出）
        ESP_ERROR_CHECK(aw9523_->Init(AW9523_RST_GPIO, AW9523_CONFIG_P0, AW9523_CONFIG_P1, true));

        // 读取芯片实际配置并打印
        uint8_t actual_p0_config, actual_p1_config;
        uint8_t actual_p0_mask, actual_p1_mask;
        uint8_t actual_p0_input, actual_p1_input;
        uint8_t actual_p0_output, actual_p1_output;

        if (aw9523_->read_config(&actual_p0_config, &actual_p1_config) == ESP_OK) {
            ESP_LOGI(TAG, "芯片实际端口配置: P0=0x%02X, P1=0x%02X", actual_p0_config, actual_p1_config);
        }

        // 中断掩码：仅对按钮位开放中断
        aw9523_->set_int_mask(0, AW9523_INTMASK_P0);
        aw9523_->set_int_mask(1, AW9523_INTMASK_P1);

        if (aw9523_->read_int_mask(&actual_p0_mask, &actual_p1_mask) == ESP_OK) {
            ESP_LOGI(TAG, "芯片实际中断掩码: P0=0x%02X, P1=0x%02X", actual_p0_mask, actual_p1_mask);
        }

        if (aw9523_->read_inputs(&actual_p0_input, &actual_p1_input) == ESP_OK) {
            ESP_LOGI(TAG, "芯片当前输入状态: P0=0x%02X, P1=0x%02X", actual_p0_input, actual_p1_input);
        }

        if (aw9523_->read_outputs(&actual_p0_output, &actual_p1_output) == ESP_OK) {
            ESP_LOGI(TAG, "芯片当前输出状态: P0=0x%02X, P1=0x%02X", actual_p0_output, actual_p1_output);
        }

        // 配置 INTN 引脚
        if (AW9523_INT_GPIO != GPIO_NUM_NC) {
            gpio_config_t io_cfg = {};
            io_cfg.pin_bit_mask = 1ULL << AW9523_INT_GPIO;
            io_cfg.mode = GPIO_MODE_INPUT;
            io_cfg.pull_up_en = GPIO_PULLUP_ENABLE;  // INTN 低有效，上拉
            io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
            io_cfg.intr_type = GPIO_INTR_NEGEDGE;    // 下降沿
            ESP_ERROR_CHECK(gpio_config(&io_cfg));
            aw_int_queue_ = xQueueCreate(4, sizeof(uint32_t));
            esp_err_t isr_ret = gpio_install_isr_service(0);
            if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
                ESP_ERROR_CHECK(isr_ret);
            }
            ESP_ERROR_CHECK(gpio_isr_handler_add(AW9523_INT_GPIO, AwGpioIsrThunk, this));
            // 创建去抖任务
            xTaskCreate(AwButtonTask, "aw_btn", 2048, this, 5, &aw_btn_task_);
        }

        // 默认输出关断（已在 Init 内处理），可按需再次写入
        ESP_LOGI(TAG, "AW9523B IO扩展芯片初始化完成");
        ESP_LOGI(TAG, "功能映射: P0_0/1/7=按键输入, P0_2-6=LED输出, P1=电机/加热器控制");
    }

    static void IRAM_ATTR AwGpioIsrThunk(void* arg) {
        auto* self = static_cast<LichuangC3DevBoard*>(arg);
        uint32_t gpio = AW9523_INT_GPIO;
        if (self->aw_int_queue_) {
            xQueueSendFromISR(self->aw_int_queue_, &gpio, nullptr);
        }
    }

    static void AwButtonTask(void* arg) {
        auto* self = static_cast<LichuangC3DevBoard*>(arg);
        uint32_t dummy;
        const uint8_t debounce_need = 3; // 3*10ms = 30ms 去抖
        while (true) {
            // 等待中断或超时轮询
            if (xQueueReceive(self->aw_int_queue_, &dummy, pdMS_TO_TICKS(10)) == pdTRUE) {
                // 收到中断，立即读取
            }
            uint8_t p0, p1;
            if (self->aw9523_ && self->aw9523_->read_inputs(&p0, &p1) == ESP_OK) {
                // 仅关心 P0 的 0/1/7 位（按钮），低有效或高有效需按硬件定义，这里假设高电平=按下为1
                uint8_t mask = (1u << P0_BTN_SUCK_BIT) | (1u << P0_BTN_ON_BIT) | (1u << P0_BTN_VOL_BIT);
                uint8_t now = p0 & mask;
                if (now == self->p0_input_last_) {
                    if (self->debounce_cnt_ < debounce_need) self->debounce_cnt_++;
                } else {
                    self->debounce_cnt_ = 0;
                }
                self->p0_input_last_ = now;
                if (self->debounce_cnt_ >= debounce_need && now != self->p0_input_stable_) {
                    // 状态稳定变化 -> 触发事件（此处示例打印）
                    ESP_LOGI(TAG, "Buttons changed: old=0x%02x new=0x%02x", self->p0_input_stable_, now);
                    self->p0_input_stable_ = now;
                    // TODO: 分发到应用或封装成 Button 事件
                }
            }
        }
    }

    void I2cDetect() {
        uint8_t address;
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address = i + j;
                esp_err_t ret = i2c_master_probe(codec_i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }


    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
    }

public:
    LichuangC3DevBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        // 无显示屏，跳过 SPI 初始化
        // InitializeSpi();
        InitializeSt7789Display();
        InitializeAw9523();
        InitializeButtons();
        InitializeIot();
        GetBacklight()->SetBrightness(100);
        led_ = new SingleLed(WS2812_GPIO);
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_,
            I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    //重写基类board的GetLed方法
    virtual Led* GetLed() override {
        return led_;
    }


};

DECLARE_BOARD(LichuangC3DevBoard);
