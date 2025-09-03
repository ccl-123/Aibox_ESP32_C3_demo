#include "wifi_board.h"
#include "audio_codecs/es8311_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "iot/thing_manager.h"
#include "display/display.h"  // 使用 NoDisplay

#include <esp_log.h>
#include <esp_sleep.h>
#include <driver/i2c_master.h>
#include <wifi_station.h>
#include "led/single_led.h"

#define TAG "LichuangC3DevBoard"

class LichuangC3DevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button boot_button_;
    Button power_button_;  // GPIO18电源按键
    Display* display_ = nullptr; // 使用通用 Display 指针，实际为 NoDisplay
    SingleLed* led_;

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
        // 未使用屏幕，SPI 不初始化，节省内存与 DMA 资源
    }

    void InitializePowerButton() {
        // GPIO18专门配置：配置为普通GPIO输入
        gpio_config_t gpio_conf = {};
        
        // 配置GPIO18为输入模式
        gpio_conf.pin_bit_mask = (1ULL << POWER_BUTTON_GPIO);
        gpio_conf.mode = GPIO_MODE_INPUT;
        
        // 按键按下时为高电平，所以配置下拉电阻，默认状态为低电平
        gpio_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
        
        // 启用中断（上升沿和下降沿都触发，由Button库处理）
        gpio_conf.intr_type = GPIO_INTR_ANYEDGE;
        
        // 应用配置
        ESP_ERROR_CHECK(gpio_config(&gpio_conf));
        
        // GPIO18是普通GPIO，无特殊复用功能冲突
        ESP_LOGI(TAG, "GPIO18 configured as power button input with pull-down");
    }

    void InitializeButtons() {
        // 原有的BOOT按键功能
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
        
        // 添加按键事件调试输出
        power_button_.OnPressDown([this]() {
            ESP_LOGI(TAG, "[BUTTON-DEBUG] Power button PRESS DOWN detected");
        });
        
        power_button_.OnPressUp([this]() {
            ESP_LOGI(TAG, "[BUTTON-DEBUG] Power button PRESS UP detected");
        });
        
        // GPIO18电源按键功能
        // 单击：设备开机（软件层面） - 与深度睡眠关机相反的处理
        power_button_.OnClick([this]() {
            ESP_LOGI(TAG, "Power button single click - Device wake up/power on");
            auto& app = Application::GetInstance();
            
            // 如果设备处于关机状态或初始状态，则开机（软件唤醒）
            if (app.GetDeviceState() == kDeviceStateUnknown || 
                app.GetDeviceState() == kDeviceStateStarting) {
                
                ESP_LOGI(TAG, "Device waking up from shutdown state...");
                
                // 重新启用音频外设
                auto codec = GetAudioCodec();
                if (codec) {
                    codec->EnableInput(true);
                    codec->EnableOutput(true);
                }
                
                // 设置设备为空闲状态，让系统重新初始化
                app.SetDeviceState(kDeviceStateIdle);
                
                ESP_LOGI(TAG, "Device successfully woken up and ready");
            } else {
                // 如果已经开机，则执行正常的按键功能
                app.ToggleChatState();
            }
        });
        
        // 长按3秒：设备深度睡眠关机
        power_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "Power button long press - Device deep sleep shutdown");
            auto& app = Application::GetInstance();
            
            // 播放关机提示音（如果有的话）
            // app.PlaySound(Lang::Sounds::P3_SHUTDOWN);
            
            // 设置设备为未知状态，表示关机
            ESP_LOGI(TAG, "Device entering deep sleep...");
            app.SetDeviceState(kDeviceStateUnknown);
            
            // 关闭所有外设
            auto codec = GetAudioCodec();
            if (codec) {
                codec->EnableInput(false);
                codec->EnableOutput(false);
            }
            
            // 短暂延迟，让系统完成清理工作
            vTaskDelay(pdMS_TO_TICKS(500));
            
            ESP_LOGI(TAG, "Entering deep sleep mode - device will be powered off");
            
            // 禁用所有唤醒源，进入深度睡眠
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
            esp_deep_sleep_start();
        });
        
        // 三击：进入配网模式（AP模式）
        power_button_.OnMultipleClick([this]() {
            ESP_LOGI(TAG, "🔥🔥🔥 Power button TRIPLE CLICK detected - Enter WiFi configuration mode 🔥🔥🔥");
            auto& app = Application::GetInstance();
            
            // 重置WiFi配置，进入AP模式
            ResetWifiConfiguration();
            
            // 播放配网提示音（如果有的话）
            // app.PlaySound(Lang::Sounds::P3_WIFI_CONFIG);
            
            // 设置设备状态为WiFi配置状态
            app.SetDeviceState(kDeviceStateWifiConfiguring);
            
            ESP_LOGI(TAG, "WiFi configuration mode activated successfully");
        }, 3);  // 三击
        
        // 添加双击检测作为对比测试
        power_button_.OnDoubleClick([this]() {
            ESP_LOGI(TAG, "🚀🚀 Power button DOUBLE CLICK detected (test) 🚀🚀");
        });
    }

    void InitializeSt7789Display() {
        // 不使用屏幕，改为 NoDisplay，避免 LVGL 初始化
        display_ = new NoDisplay();
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
    }

public:
    LichuangC3DevBoard() : 
        boot_button_(BOOT_BUTTON_GPIO),
        power_button_(POWER_BUTTON_GPIO, true, 3000, 180) {  // active_high=true, long_press=3000ms, short_press=180ms
        // 首先初始化GPIO18的硬件配置
        InitializePowerButton();
        
        InitializeI2c();
        // 不初始化 SPI / LCD，改为 NoDisplay
        InitializeSt7789Display();
        InitializeButtons();
        InitializeIot();
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
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;  // 不使用屏幕，避免GPIO冲突
    }

    //重写基类board的GetLed方法
    virtual Led* GetLed() override {
        return led_;
    }
};

DECLARE_BOARD(LichuangC3DevBoard);
