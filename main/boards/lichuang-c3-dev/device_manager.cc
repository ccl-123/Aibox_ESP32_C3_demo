#include "device_manager.h"
#include "application.h"
#include "board.h"
#include <esp_log.h>
#include <esp_system.h>
#include <inttypes.h>

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
    
    // 创建夹吸定时器 (单次触发，动态时间)
    suck_timer_ = xTimerCreate("suck_timer", pdMS_TO_TICKS(MOTOR_SUCK_LEVEL1_TIME_MS), pdFALSE, this, SuckTimerCallback);
    if (suck_timer_ == NULL) {
        ESP_LOGE(TAG, "夹吸定时器创建失败！");
        return;
    }
    
    // 创建放气定时器 (单次触发，1.5秒)
    loose_timer_ = xTimerCreate("loose_timer", pdMS_TO_TICKS(MOTOR_LOOSE_DURATION_MS), pdFALSE, this, LooseTimerCallback);
    if (loose_timer_ == NULL) {
        ESP_LOGE(TAG, "放气定时器创建失败！");
        return;
    }
    
    // 创建加热定时器 (单次触发，10分钟)
    heater_timer_ = xTimerCreate("heater_timer", pdMS_TO_TICKS(HEATER_DURATION_MS), pdFALSE, this, HeaterTimerCallback);
    if (heater_timer_ == NULL) {
        ESP_LOGE(TAG, "加热定时器创建失败！");
        return;
    }
    
    ESP_LOGI(TAG, "设备管理器初始化完成✓");
}

DeviceManager::~DeviceManager() {
    if (pwm_timer_) {
        xTimerDelete(pwm_timer_, 0);
    }
    if (suck_timer_) {
        xTimerDelete(suck_timer_, 0);
    }
    if (loose_timer_) {
        xTimerDelete(loose_timer_, 0);
    }
    if (heater_timer_) {
        xTimerDelete(heater_timer_, 0);
    }
    delete settings_;
}

void DeviceManager::SetMotorLevel(MotorType motor, uint8_t level) {
    if (level < 1 || level > 3) return;  // 修改为3档
    
    switch (motor) {
        case MOTOR_ROCK:
            rock_level_ = level;
            ESP_LOGI(TAG, "震动档位设置为: %d", level);
            break;
        case MOTOR_SUCK:
            suck_level_ = level;
            ESP_LOGI(TAG, "夹吸档位设置为: %d", level);
            break;
        case MOTOR_LOOSE:
            // 放气功能固定占空比，不支持档位调节
            ESP_LOGW(TAG, "放气功能不支持档位调节，固定使用 %d%% 占空比", MOTOR_LOOSE_PWM_DUTY);
            return; // 不保存设置
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
        case MOTOR_LOOSE: return 1; // 放气固定返回1（不使用档位）
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
        if (current_level > 3) {  // 修改为3档
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
        case MOTOR_LOOSE:
            loose_running_ = false;
            aw9523_->digital_write(1, 2, false);
            ESP_LOGI(TAG, "停止放气");
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
    StopMotor(MOTOR_LOOSE);
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
    
    // 夹吸 PWM (固定80%占空比)
    if (suck_running_) {
        uint32_t duty = MOTOR_SUCK_PWM_DUTY; // 固定80%占空比
        bool output = (pwm_counter_ * 100 / PWM_PERIOD) < duty;
        aw9523_->digital_write(1, 1, output);
    }
    
    // 放气 PWM (固定80%占空比)
    if (loose_running_) {
        uint32_t duty = MOTOR_LOOSE_PWM_DUTY; // 固定80%占空比
        bool output = (pwm_counter_ * 100 / PWM_PERIOD) < duty;
        aw9523_->digital_write(1, 2, output);
    }
    
    // 加热 PWM (按档位：70%/85%/100%)
    if (heater_running_) {
        uint32_t duty;
        switch (heater_level_) {
            case 1: duty = HEATER_LEVEL1_DUTY; break; // 70%
            case 2: duty = HEATER_LEVEL2_DUTY; break; // 85%
            case 3: duty = HEATER_LEVEL3_DUTY; break; // 100%
            default: duty = HEATER_LEVEL1_DUTY; break;
        }
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
                // 新逻辑：启动夹吸循环序列
                if (suck_sequence_running_) {
                    // 如果正在运行循环序列，切换到下一档位
                    suck_level_++;
                    if (suck_level_ > 3) {
                        suck_level_ = 1;
                    }
                    ESP_LOGI(TAG, "夹吸档位切换为: %d", suck_level_);
                    SaveSettings();
                } else {
                    suck_level_ = GetMotorLevel(MOTOR_SUCK); // 获取上次档位
                    ESP_LOGI(TAG, "启动夹吸档位: %d", suck_level_);
                }
                StartSuckSequence(suck_level_);
            } else if (event == ButtonEvent::DOUBLE_CLICK) {
                ESP_LOGI(TAG, "*******************夹吸按键双击 - 切换加热档位******************");
                // 新逻辑：启动加热序列（10分钟自动停止）
                if (heater_running_) {
                    // 如果正在运行，切换到下一档位
                    heater_level_++;
                    if (heater_level_ > 3) {
                        heater_level_ = 1;
                    }
                    ESP_LOGI(TAG, "加热档位切换为: %d", heater_level_);
                    SaveSettings();
                } else {
                    heater_level_ = GetMotorLevel(HEATER); // 获取上次档位
                    ESP_LOGI(TAG, "启动加热档位: %d", heater_level_);
                }
                StartHeaterSequence(heater_level_);
            } else if (event == ButtonEvent::LONG_PRESS) {
                ESP_LOGI(TAG, "*******************夹吸按键长按2秒 - 关闭所有功能*************");
                StopSuckSequence();   // 停止夹吸序列
                StopHeaterSequence(); // 停止加热序列
                StopLooseMotor();     // 停止放气
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
    
    // 停止所有序列和电机
    StopSuckSequence();   // 停止夹吸序列
    StopHeaterSequence(); // 停止加热序列
    StopLooseMotor();     // 停止放气
    StopAllMotors();      // 停止其他电机
    
    // 保存设置
    SaveSettings();
    
    // 延迟一点时间确保操作完成
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // 重启系统（ESP32的关机就是重启到深度睡眠）
    esp_restart();
}

void DeviceManager::StartLooseMotor() {
    ESP_LOGI(TAG, "🌬️ 开始放气 - PWM占空比: %d%%, 持续时间: %.1f秒", 
             MOTOR_LOOSE_PWM_DUTY, MOTOR_LOOSE_DURATION_MS / 1000.0f);
    
    // 如果放气已经在运行，先停止之前的定时器
    if (loose_running_) {
        ESP_LOGI(TAG, "停止上一次放气操作");
        xTimerStop(loose_timer_, 0);
        StopLooseMotor();
    }
    
    // 启动放气电机
    loose_running_ = true;
    
    // 启动5秒定时器
    if (xTimerStart(loose_timer_, 0) != pdPASS) {
        ESP_LOGE(TAG, "放气定时器启动失败！");
        StopLooseMotor(); // 如果定时器启动失败，立即停止
        return;
    }
    
    ESP_LOGI(TAG, "放气电机已启动，将在 %.1f 秒后自动停止", MOTOR_LOOSE_DURATION_MS / 1000.0f);
}

void DeviceManager::StopLooseMotor() {
    if (loose_running_) {
        ESP_LOGI(TAG, "🌬️ 停止放气");
        loose_running_ = false;
        aw9523_->digital_write(1, 2, false); // 立即关闭P1_2
        
        // 停止定时器（如果还在运行）
        xTimerStop(loose_timer_, 0);
    }
}

void DeviceManager::LooseTimerCallback(TimerHandle_t timer) {
    auto* self = static_cast<DeviceManager*>(pvTimerGetTimerID(timer));
    ESP_LOGI(TAG, "🌬️ 放气定时器到期 - 1.5秒放气完成");
    
    // 停止放气
    self->StopLooseMotor();
    
    // 检查夹吸序列是否还在运行，如果是则重新启动夹吸（形成循环）
    if (self->suck_sequence_running_) {
        ESP_LOGI(TAG, "🔄 放气完成，重新启动夹吸循环 - 档位: %d", self->current_suck_level_);
        
        // 根据当前档位确定夹吸时间
        uint32_t suck_time_ms;
        switch (self->current_suck_level_) {
            case 1: suck_time_ms = MOTOR_SUCK_LEVEL1_TIME_MS; break; // 3秒
            case 2: suck_time_ms = MOTOR_SUCK_LEVEL2_TIME_MS; break; // 3.5秒
            case 3: suck_time_ms = MOTOR_SUCK_LEVEL3_TIME_MS; break; // 4秒
            default: suck_time_ms = MOTOR_SUCK_LEVEL1_TIME_MS; break;
        }
        
        // 启动夹吸电机
        self->suck_running_ = true;
        
        // 重新配置并启动夹吸定时器
        xTimerChangePeriod(self->suck_timer_, pdMS_TO_TICKS(suck_time_ms), 0);
        if (xTimerStart(self->suck_timer_, 0) != pdPASS) {
            ESP_LOGE(TAG, "夹吸循环重启失败！停止序列");
            self->StopSuckSequence();
            return;
        }
        
        ESP_LOGI(TAG, "夹吸循环重启成功，将在 %.1f 秒后切换到放气", suck_time_ms / 1000.0f);
    } else {
        ESP_LOGI(TAG, "🔄 夹吸序列已停止，不再循环");
    }
}

void DeviceManager::StartSuckSequence(uint8_t level) {
    ESP_LOGI(TAG, "🔧 开始夹吸循环序列 - 档位: %d", level);
    
    // 停止之前的序列
    StopSuckSequence();
    
    // 设置序列状态
    suck_sequence_running_ = true;
    current_suck_level_ = level;
    
    // 根据档位确定时间
    uint32_t suck_time_ms;
    switch (level) {
        case 1: suck_time_ms = MOTOR_SUCK_LEVEL1_TIME_MS; break; // 3秒
        case 2: suck_time_ms = MOTOR_SUCK_LEVEL2_TIME_MS; break; // 3.5秒
        case 3: suck_time_ms = MOTOR_SUCK_LEVEL3_TIME_MS; break; // 4秒
        default: suck_time_ms = MOTOR_SUCK_LEVEL1_TIME_MS; break;
    }
    
    ESP_LOGI(TAG, "夹吸档位 %d: 80%% PWM 持续 %.1f秒，然后放气 1.5秒，循环执行", 
             level, suck_time_ms / 1000.0f);
    
    // 启动夹吸电机
    suck_running_ = true;
    
    // 重新配置并启动定时器
    xTimerChangePeriod(suck_timer_, pdMS_TO_TICKS(suck_time_ms), 0);
    if (xTimerStart(suck_timer_, 0) != pdPASS) {
        ESP_LOGE(TAG, "夹吸定时器启动失败！");
        StopSuckSequence();
        return;
    }
    
    ESP_LOGI(TAG, "夹吸循环已启动，将在 %.1f 秒后自动切换到放气", suck_time_ms / 1000.0f);
}

void DeviceManager::StopSuckSequence() {
    if (suck_sequence_running_ || suck_running_) {
        ESP_LOGI(TAG, "🔧 停止夹吸循环序列");
        
        // 停止序列状态
        suck_sequence_running_ = false;
        suck_running_ = false;
        
        // 立即关闭夹吸电机
        aw9523_->digital_write(1, 1, false);
        
        // 停止夹吸定时器
        xTimerStop(suck_timer_, 0);
        
        ESP_LOGI(TAG, "夹吸循环序列已完全停止");
    }
}

void DeviceManager::SuckTimerCallback(TimerHandle_t timer) {
    auto* self = static_cast<DeviceManager*>(pvTimerGetTimerID(timer));
    ESP_LOGI(TAG, "🔧 夹吸定时器到期 - 开始切换到放气");
    
    // 停止夹吸
    self->suck_running_ = false;
    self->aw9523_->digital_write(1, 1, false);
    
    // 立即启动放气
    self->StartLooseMotor();
}

void DeviceManager::StartHeaterSequence(uint8_t level) {
    ESP_LOGI(TAG, "🔥 开始加热序列 - 档位: %d", level);
    
    // 停止之前的序列
    StopHeaterSequence();
    
    // 根据档位确定占空比
    uint32_t duty;
    switch (level) {
        case 1: duty = HEATER_LEVEL1_DUTY; break; // 70%
        case 2: duty = HEATER_LEVEL2_DUTY; break; // 85%
        case 3: duty = HEATER_LEVEL3_DUTY; break; // 100%
        default: duty = HEATER_LEVEL1_DUTY; break;
    }
    
    ESP_LOGI(TAG, "加热档位 %d: %" PRIu32 "%% PWM 持续 10分钟", level, duty);
    
    // 启动加热
    heater_running_ = true;
    
    // 启动10分钟定时器
    if (xTimerStart(heater_timer_, 0) != pdPASS) {
        ESP_LOGE(TAG, "加热定时器启动失败！");
        StopHeaterSequence();
        return;
    }
    
    ESP_LOGI(TAG, "加热已启动，将在 10 分钟后自动停止");
}

void DeviceManager::StopHeaterSequence() {
    if (heater_running_) {
        ESP_LOGI(TAG, "🔥 停止加热序列");
        heater_running_ = false;
        aw9523_->digital_write(1, 3, false); // 立即关闭P1_3
        
        // 停止定时器
        xTimerStop(heater_timer_, 0);
    }
}

void DeviceManager::HeaterTimerCallback(TimerHandle_t timer) {
    auto* self = static_cast<DeviceManager*>(pvTimerGetTimerID(timer));
    ESP_LOGI(TAG, "🔥 加热定时器到期 - 10分钟加热完成");
    self->StopHeaterSequence();
}