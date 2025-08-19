#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include "aw9523.h"
#include "settings.h"
#include "button_state_machine.h"

class DeviceManager {
public:
    enum MotorType {
        MOTOR_ROCK = 0,    // P1_0 震动
        MOTOR_SUCK = 1,    // P1_1 夹吸 
        HEATER = 3         // P1_3 加热
    };

    DeviceManager(Aw9523* aw9523);
    ~DeviceManager();
    
    // 档位控制 (1-8档)
    void SetMotorLevel(MotorType motor, uint8_t level);
    uint8_t GetMotorLevel(MotorType motor);
    void ToggleMotor(MotorType motor);
    void StopMotor(MotorType motor);
    void StopAllMotors();
    
    // 音量控制 (60-100)
    void SetVolume(uint8_t volume);
    uint8_t GetVolume();
    void NextVolumeLevel();
    
    // 设置保存和恢复
    void SaveSettings();
    void LoadSettings();
    
    // 按键事件处理
    void HandleButtonEvent(ButtonId button, ButtonEvent event);
    
    // 关机处理
    void Shutdown();
    
private:
    Aw9523* aw9523_;
    Settings* settings_;
    
    // 当前档位状态
    uint8_t rock_level_ = 1;     // 震动档位 (1-8)
    uint8_t suck_level_ = 1;     // 夹吸档位 (1-8)  
    uint8_t heater_level_ = 1;   // 加热档位 (1-8)
    uint8_t volume_level_ = 80;  // 音量档位 (60-100)
    
    // 运行状态
    bool rock_running_ = false;
    bool suck_running_ = false;
    bool heater_running_ = false;
    
    // PWM 相关
    TimerHandle_t pwm_timer_;
    static void PwmTimerCallback(TimerHandle_t timer);
    void UpdatePwmOutput();
    
    // PWM 状态
    uint32_t pwm_counter_ = 0;
    static const uint32_t PWM_PERIOD = 100; // PWM周期 100*10ms = 1秒
};

#endif