#include "wifi_board.h"
#include "audio_codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "aw9523.h"
#include "iot/thing_manager.h"
#include "button_state_machine.h"
#include "device_manager.h"

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
    
    // 按键状态机和设备管理器
    ButtonStateMachine* button_state_machine_ = nullptr;
    DeviceManager* device_manager_ = nullptr;
    
    // 按键状态机定时器
    TimerHandle_t button_timer_ = nullptr;

    // 按钮位定义（端口/位）
    static constexpr uint8_t P0_BTN_SUCK_BIT = 0; // P0_0 夹吸加热按键
    static constexpr uint8_t P0_BTN_ON_BIT   = 1; // P0_1 开关机按键
    static constexpr uint8_t P0_BTN_ROCK_BIT = 2; // P0_2 震动按键
    static constexpr uint8_t P0_BTN_VOL_BIT  = 3; // P0_3 音量按键

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

        // 实例化与初始化 AW9523
        aw9523_ = new Aw9523(codec_i2c_bus_, AW9523_I2C_ADDR);
        ESP_ERROR_CHECK(aw9523_->Init(AW9523_RST_GPIO, AW9523_CONFIG_P0, AW9523_CONFIG_P1, true));

        // 配置中断掩码
        aw9523_->set_int_mask(0, AW9523_INTMASK_P0);
        aw9523_->set_int_mask(1, AW9523_INTMASK_P1);

        // 配置 INTN 引脚
        if (AW9523_INT_GPIO != GPIO_NUM_NC) {
            gpio_config_t io_cfg = {};
            io_cfg.pin_bit_mask = 1ULL << AW9523_INT_GPIO;
            io_cfg.mode = GPIO_MODE_INPUT;
            io_cfg.pull_up_en = GPIO_PULLUP_ENABLE;     
            io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
            io_cfg.intr_type = GPIO_INTR_NEGEDGE;
            ESP_ERROR_CHECK(gpio_config(&io_cfg));
            
            aw_int_queue_ = xQueueCreate(4, sizeof(uint32_t));
            esp_err_t isr_ret = gpio_install_isr_service(0);
            if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
                ESP_ERROR_CHECK(isr_ret);
            }
            ESP_ERROR_CHECK(gpio_isr_handler_add(AW9523_INT_GPIO, AwGpioIsrThunk, this));
            
            // 清除中断标志
            uint8_t dummy_p0, dummy_p1;
            aw9523_->read_inputs(&dummy_p0, &dummy_p1);
        }
        
        // 初始化设备管理器
        device_manager_ = new DeviceManager(aw9523_);
        
        // 初始化按键状态机
        button_state_machine_ = new ButtonStateMachine();
        button_state_machine_->SetCallback([this](ButtonId button, ButtonEvent event) {
            if (device_manager_) {
                device_manager_->HandleButtonEvent(button, event);
            }
        });
        
        // 创建按键状态机定时器（10ms周期）
        button_timer_ = xTimerCreate("button_timer", pdMS_TO_TICKS(10), pdTRUE, this, ButtonTimerCallback);
        xTimerStart(button_timer_, 0);
        
        // 创建按键监控任务
            xTaskCreate(AwButtonTask, "aw_btn", 2048, this, 5, &aw_btn_task_);
        
        ESP_LOGI(TAG, "AW9523B初始化完成");
        
        // 🔧 详细硬件诊断
        ESP_LOGI(TAG, "=== 开始按键硬件诊断 ===");
        
        // 1. 检查GPIO11初始状态
        if (AW9523_INT_GPIO != GPIO_NUM_NC) {
            int gpio_level = gpio_get_level(AW9523_INT_GPIO);
            ESP_LOGI(TAG, "1️⃣ GPIO11(INTN)初始电平: %d (正常应为1)", gpio_level);
            if (gpio_level == 0) {
                ESP_LOGW(TAG, "⚠️  警告: GPIO11初始为低电平，可能有中断待处理");
            }
        }
        
        // 2. 读取所有关键寄存器状态
        if (aw9523_) {
            uint8_t p0_input, p1_input, p0_config, p1_config;
            uint8_t p0_intmask, p1_intmask;
            
            ESP_LOGI(TAG, "2️⃣ 读取AW9523B寄存器状态:");
            
            if (aw9523_->read_inputs(&p0_input, &p1_input) == ESP_OK) {
                ESP_LOGI(TAG, "   INPUT: P0=0x%02X P1=0x%02X", p0_input, p1_input);
                ESP_LOGI(TAG, "   按键位状态: P0_0=%d P0_1=%d P0_2=%d P0_3=%d", 
                        (p0_input>>0)&1, (p0_input>>1)&1, (p0_input>>2)&1, (p0_input>>3)&1);
            }
            
            if (aw9523_->read_config(&p0_config, &p1_config) == ESP_OK) {
                ESP_LOGI(TAG, "   CONFIG: P0=0x%02X P1=0x%02X", p0_config, p1_config);
            }
            
            if (aw9523_->read_int_mask(&p0_intmask, &p1_intmask) == ESP_OK) {
                ESP_LOGI(TAG, "   INTMASK: P0=0x%02X P1=0x%02X", p0_intmask, p1_intmask);
            }
        }
        
        // 3. 启动实时监控任务
        ESP_LOGI(TAG, "3️⃣ 启动按键实时监控...");
        ESP_LOGI(TAG, "   请现在按下单个按键，观察寄存器变化：");
        
        xTaskCreate([](void* arg) {
            auto* self = static_cast<LichuangC3DevBoard*>(arg);
            uint8_t last_p0 = 0xFF;
            
            for (int i = 0; i < 100; i++) { // 监控10秒
                if (self->aw9523_) {
                    uint8_t p0, p1;
                    esp_err_t result = self->aw9523_->read_inputs(&p0, &p1);
                    
                    if (result == ESP_OK) {
                        if (p0 != last_p0) {
                            ESP_LOGI(TAG, "🔔 检测到P0变化: 0x%02X -> 0x%02X (GPIO11=%d)", 
                                    last_p0, p0, gpio_get_level(AW9523_INT_GPIO));
                            ESP_LOGI(TAG, "   各按键状态: P0_0=%d P0_1=%d P0_2=%d P0_3=%d", 
                                    (p0>>0)&1, (p0>>1)&1, (p0>>2)&1, (p0>>3)&1);
                            last_p0 = p0;
                        }
                    } else {
                        ESP_LOGE(TAG, "❌ I2C读取失败: %s", esp_err_to_name(result));
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            ESP_LOGI(TAG, "实时监控结束");
            vTaskDelete(NULL);
        }, "btn_monitor", 3072, this, 6, NULL);
        
        ESP_LOGI(TAG, "=== 硬件诊断启动完成 ===");
    }
    




    static void IRAM_ATTR AwGpioIsrThunk(void* arg) {
        auto* self = static_cast<LichuangC3DevBoard*>(arg);
        uint32_t gpio = AW9523_INT_GPIO;
        if (self->aw_int_queue_) {
            xQueueSendFromISR(self->aw_int_queue_, &gpio, nullptr);
        }
    }
    
    static void ButtonTimerCallback(TimerHandle_t timer) {
        auto* self = static_cast<LichuangC3DevBoard*>(pvTimerGetTimerID(timer));
        if (self->button_state_machine_) {
            self->button_state_machine_->ProcessTimer();
        }
    }

    static void AwButtonTask(void* arg) {
        auto* self = static_cast<LichuangC3DevBoard*>(arg);
        uint32_t dummy;
        uint8_t last_button_states = 0x00;
        
        while (true) {
            bool has_interrupt = false;
            
            // 等待中断信号
            if (self->aw_int_queue_ != nullptr) {
                if (xQueueReceive(self->aw_int_queue_, &dummy, pdMS_TO_TICKS(100)) == pdTRUE) {
                    has_interrupt = true;
                    
                    // ✅ 只有真正收到中断时才打印
                    int gpio_level = gpio_get_level(AW9523_INT_GPIO);
                    ESP_LOGI(TAG, "🔔 真实中断！GPIO11电平=%d", gpio_level);
                    
                    // 清除中断标志
                    uint8_t clear_p0, clear_p1;
                    if (self->aw9523_) {
                        self->aw9523_->read_inputs(&clear_p0, &clear_p1);
                        ESP_LOGI(TAG, "清除中断后: P0=0x%02X P1=0x%02X, GPIO11=%d", 
                                clear_p0, clear_p1, gpio_get_level(AW9523_INT_GPIO));
                    }
                } else {
                    // 超时 - 正常情况
                    static int normal_count = 0;
                    normal_count++;
                    if (normal_count % 600 == 0) { // 每60秒打印一次正常状态
                        ESP_LOGI(TAG, "按键监控正常运行 (60秒无中断)");
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            
            // 读取按键状态
            uint8_t p0, p1;
            if (self->aw9523_ && self->aw9523_->read_inputs(&p0, &p1) == ESP_OK) {
                uint8_t button_states = p0 & 0x0F;
                
                // 检测状态变化
                if (button_states != last_button_states || has_interrupt) {
                    // 防抖确认
                            vTaskDelay(pdMS_TO_TICKS(5));
                            uint8_t p0_confirm, p1_confirm;
                            if (self->aw9523_->read_inputs(&p0_confirm, &p1_confirm) == ESP_OK) {
                                uint8_t button_states_confirm = p0_confirm & 0x0F;
                                
                        if (button_states == button_states_confirm) {
                            // 更新状态
                    if (button_states != last_button_states) {
                        last_button_states = button_states;
                }
                
                // 传递给按键状态机处理
                if (self->button_state_machine_) {
                    self->button_state_machine_->ProcessButtonStates(button_states);
                            }
                        }
                    }
                }
            }
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
        InitializeSt7789Display();
        InitializeAw9523();
        InitializeButtons();
        InitializeIot();
        GetBacklight()->SetBrightness(100);
        led_ = new SingleLed(WS2812_GPIO);
    }
    
    ~LichuangC3DevBoard() {
        // 清理资源
        if (button_timer_) {
            xTimerDelete(button_timer_, 0);
        }
        if (device_manager_) {
            delete device_manager_;
        }
        if (button_state_machine_) {
            delete button_state_machine_;
        }
        if (aw9523_) {
            delete aw9523_;
        }
        if (led_) {
            delete led_;
        }
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
