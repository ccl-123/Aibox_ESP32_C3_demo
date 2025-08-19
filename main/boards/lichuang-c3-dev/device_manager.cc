#include "device_manager.h"
#include "application.h"
#include "board.h"
#include <esp_log.h>
#include <esp_system.h>

#define TAG "DeviceManager"

DeviceManager::DeviceManager(Aw9523* aw9523) : aw9523_(aw9523) {
    ESP_LOGI(TAG, "开始初始化设备管理器...");
    
    if (!aw9523_) {
        ESP_LOGE(TAG, "AW9523指针为空！");
        return;
    }
    
    ESP_LOGI(TAG, "正在创建Settings对象...");
    settings_ = new Settings("device", true);
    if (!settings_) {
        ESP_LOGE(TAG, "Settings初始化失败！");
        return;
    }
    ESP_LOGI(TAG, "Settings对象创建成功");
    
    ESP_LOGI(TAG, "正在加载设置...");
    LoadSettings();
    ESP_LOGI(TAG, "设置加载完成");
    
    // 创建PWM定时器 (10ms周期)
    pwm_timer_ = xTimerCreate("pwm_timer", pdMS_TO_TICKS(10), pdTRUE, this, PwmTimerCallback);
    if (pwm_timer_ == NULL) {
        ESP_LOGE(TAG, "PWM定时器创建失败！");
        return;
    }
    
    if (xTimerStart(pwm_timer_, 0) != pdPASS) {
        ESP_LOGE(TAG, "PWM定时器启动失败！");
        return;
    }
    
    ESP_LOGI(TAG, "设备管理器初始化完成✓");
}

DeviceManager::~DeviceManager() {
    if (pwm_timer_) {
        xTimerDelete(pwm_timer_, 0);
    }
    delete settings_;
}

void DeviceManager::SetMotorLevel(MotorType motor, uint8_t level) {
    if (level < 1 || level > 8) return;
    
    switch (motor) {
        case MOTOR_ROCK:
            rock_level_ = level;
            ESP_LOGI(TAG, "震动档位设置为: %d", level);
            break;
        case MOTOR_SUCK:
            suck_level_ = level;
            ESP_LOGI(TAG, "夹吸档位设置为: %d", level);
            break;
        case HEATER:
            heater_level_ = level;
            ESP_LOGI(TAG, "加热档位设置为: %d", level);
            break;
    }
    SaveSettings();
}

uint8_t DeviceManager::GetMotorLevel(MotorType motor) {
    switch (motor) {
        case MOTOR_ROCK: return rock_level_;
        case MOTOR_SUCK: return suck_level_;
        case HEATER: return heater_level_;
        default: return 1;
    }
}

void DeviceManager::ToggleMotor(MotorType motor) {
    bool* running_state;
    uint8_t current_level;
    
    switch (motor) {
        case MOTOR_ROCK:
            running_state = &rock_running_;
            current_level = rock_level_;
            break;
        case MOTOR_SUCK:
            running_state = &suck_running_;
            current_level = suck_level_;
            break;
        case HEATER:
            running_state = &heater_running_;
            current_level = heater_level_;
            break;
        default:
            return;
    }
    
    if (*running_state) {
        // 如果已开启，切换到下一档位
        current_level++;
        if (current_level > 8) {
            current_level = 1;
        }
        SetMotorLevel(motor, current_level);
    } else {
        // 如果未开启，开启电机
        *running_state = true;
        ESP_LOGI(TAG, "启动电机 %d, 档位: %d", motor, current_level);
    }
}

void DeviceManager::StopMotor(MotorType motor) {
    switch (motor) {
        case MOTOR_ROCK:
            rock_running_ = false;
            aw9523_->digital_write(1, 0, false);
            ESP_LOGI(TAG, "停止震动");
            break;
        case MOTOR_SUCK:
            suck_running_ = false;
            aw9523_->digital_write(1, 1, false);
            ESP_LOGI(TAG, "停止夹吸");
            break;
        case HEATER:
            heater_running_ = false;
            aw9523_->digital_write(1, 3, false);
            ESP_LOGI(TAG, "停止加热");
            break;
    }
}

void DeviceManager::StopAllMotors() {
    StopMotor(MOTOR_ROCK);
    StopMotor(MOTOR_SUCK);
    StopMotor(HEATER);
}

void DeviceManager::SetVolume(uint8_t volume) {
    if (volume < 60) volume = 60;
    if (volume > 100) volume = 100;
    
    volume_level_ = volume;
    
    // 尝试设置系统音量（如果音频系统已初始化）
    try {
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec) {
            codec->SetOutputVolume(volume);
            ESP_LOGI(TAG, "音量设置为: %d", volume);
        } else {
            ESP_LOGW(TAG, "音频编解码器未准备就绪，音量将在稍后应用: %d", volume);
        }
    } catch (...) {
        ESP_LOGW(TAG, "音频编解码器访问失败，音量将在稍后应用: %d", volume);
    }
    
    SaveSettings();
}

uint8_t DeviceManager::GetVolume() {
    return volume_level_;
}

void DeviceManager::NextVolumeLevel() {
    uint8_t new_volume = volume_level_ + 10;
    if (new_volume > 100) {
        new_volume = 60; // 回到最小音量
    }
    SetVolume(new_volume);
}

void DeviceManager::SaveSettings() {
    settings_->SetInt("rock_level", rock_level_);
    settings_->SetInt("suck_level", suck_level_);
    settings_->SetInt("heater_level", heater_level_);
    settings_->SetInt("volume_level", volume_level_);
}

void DeviceManager::LoadSettings() {
    rock_level_ = settings_->GetInt("rock_level", 1);
    suck_level_ = settings_->GetInt("suck_level", 1);
    heater_level_ = settings_->GetInt("heater_level", 1);
    volume_level_ = settings_->GetInt("volume_level", 80);
    
    ESP_LOGI(TAG, "设置已加载 - 震动:%d 夹吸:%d 加热:%d 音量:%d", 
             rock_level_, suck_level_, heater_level_, volume_level_);
    
    // 注意：音量设置延后到音频系统完全初始化后
    ESP_LOGI(TAG, "音量设置将在音频系统初始化完成后应用");
}

void DeviceManager::PwmTimerCallback(TimerHandle_t timer) {
    auto* self = static_cast<DeviceManager*>(pvTimerGetTimerID(timer));
    self->UpdatePwmOutput();
}

void DeviceManager::UpdatePwmOutput() {
    pwm_counter_ = (pwm_counter_ + 1) % PWM_PERIOD;
    
    // 震动 PWM (每档15%)
    if (rock_running_) {
        uint32_t duty = rock_level_ * 15; // 15%, 30%, 45%... 120%(限制为100%)
        if (duty > 100) duty = 100;
        bool output = (pwm_counter_ * 100 / PWM_PERIOD) < duty;
        aw9523_->digital_write(1, 0, output);
    }
    
    // 夹吸 PWM (每档4%)
    if (suck_running_) {
        uint32_t duty = suck_level_ * 4; // 4%, 8%, 12%... 32%
        if (duty > 100) duty = 100;
        bool output = (pwm_counter_ * 100 / PWM_PERIOD) < duty;
        aw9523_->digital_write(1, 1, output);
    }
    
    // 加热 PWM (每档2%)
    if (heater_running_) {
        uint32_t duty = heater_level_ * 2; // 2%, 4%, 6%... 16%
        if (duty > 100) duty = 100;
        bool output = (pwm_counter_ * 100 / PWM_PERIOD) < duty;
        aw9523_->digital_write(1, 3, output);
    }
}

void DeviceManager::HandleButtonEvent(ButtonId button, ButtonEvent event) {
    switch (button) {
        case ButtonId::BUTTON_ROCK:  // P0_2 震动按键
            if (event == ButtonEvent::CLICK) {
                ESP_LOGI(TAG, "*****************震动按键单击 - 切换档位或启动******************");
                ToggleMotor(MOTOR_ROCK);
            } else if (event == ButtonEvent::LONG_PRESS) {
                ESP_LOGI(TAG, "**************震动按键长按 - 关闭震动****************");
                StopMotor(MOTOR_ROCK);
            }
            break;
            
        case ButtonId::BUTTON_SUCK:  // P0_0 夹吸加热按键
            if (event == ButtonEvent::CLICK) {
                ESP_LOGI(TAG, "******************夹吸按键单击 - 切换夹吸档位******************");
                ToggleMotor(MOTOR_SUCK);
            } else if (event == ButtonEvent::DOUBLE_CLICK) {
                ESP_LOGI(TAG, "*******************夹吸按键双击 - 切换加热档位******************");
                ToggleMotor(HEATER);
            } else if (event == ButtonEvent::LONG_PRESS) {
                ESP_LOGI(TAG, "*******************夹吸按键长按2秒 - 关闭夹吸和加热功能*************");
                StopMotor(MOTOR_SUCK);
                StopMotor(HEATER);
            }
            break;
            
        case ButtonId::BUTTON_ON:  // P0_1 开关机按键
            if (event == ButtonEvent::LONG_PRESS) {
                ESP_LOGI(TAG, "*****************开关机按键长按 - 关机******************");
                Shutdown();
            }
            break;
            
        case ButtonId::BUTTON_VOL:  // P0_3 音量按键
            if (event == ButtonEvent::CLICK) {
                ESP_LOGI(TAG, "********************音量按键单击 - 音量档位增加******************");
                NextVolumeLevel();
            }
            break;
    }
}

void DeviceManager::Shutdown() {
    ESP_LOGI(TAG, "正在关闭设备...");
    
    // 停止所有电机
    StopAllMotors();
    
    // 保存设置
    SaveSettings();
    
    // 延迟一点时间确保操作完成
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // 重启系统（ESP32的关机就是重启到深度睡眠）
    esp_restart();
}