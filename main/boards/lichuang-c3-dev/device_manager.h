#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include "aw9523.h"
#include "settings.h"
#include "button_state_machine.h"

// 夹吸和放气功能相关配置
#define MOTOR_SUCK_PWM_DUTY        80    // 夹吸PWM占空比(%)
#define MOTOR_LOOSE_PWM_DUTY       80    // 放气PWM占空比(%)
#define MOTOR_LOOSE_DURATION_MS    1500  // 放气持续时间(1.5秒)

// 夹吸档位时间配置(毫秒)
#define MOTOR_SUCK_LEVEL1_TIME_MS  3000  // 一档：3秒
#define MOTOR_SUCK_LEVEL2_TIME_MS  3500  // 二档：3.5秒  
#define MOTOR_SUCK_LEVEL3_TIME_MS  4000  // 三档：4秒

// 加热档位配置
#define HEATER_LEVEL1_DUTY         70    // 一档：70% PWM
#define HEATER_LEVEL2_DUTY         85    // 二档：85% PWM
#define HEATER_LEVEL3_DUTY         100   // 三档：100% PWM
#define HEATER_DURATION_MS         600000 // 加热持续时间(10分钟)

class DeviceManager {
public:
    enum MotorType {
        MOTOR_ROCK = 0,    // P1_0 震动
        MOTOR_SUCK = 1,    // P1_1 夹吸 
        MOTOR_LOOSE = 2,   // P1_2 放气
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
    bool loose_running_ = false;  // 放气运行状态
    bool heater_running_ = false;
    
    // PWM 相关
    TimerHandle_t pwm_timer_;
    static void PwmTimerCallback(TimerHandle_t timer);
    void UpdatePwmOutput();
    
    // 夹吸定时器相关
    TimerHandle_t suck_timer_;
    static void SuckTimerCallback(TimerHandle_t timer);
    void StartSuckSequence(uint8_t level);  // 开始夹吸序列(循环执行)
    void StopSuckSequence();                // 停止夹吸序列
    
    // 序列状态控制
    bool suck_sequence_running_ = false;    // 夹吸序列是否正在运行
    uint8_t current_suck_level_ = 1;        // 当前夹吸序列档位
    
    // 放气定时器相关  
    TimerHandle_t loose_timer_;
    static void LooseTimerCallback(TimerHandle_t timer);
    void StartLooseMotor();        // 开启放气
    void StopLooseMotor();         // 停止放气
    
    // 加热定时器相关
    TimerHandle_t heater_timer_;
    static void HeaterTimerCallback(TimerHandle_t timer);
    void StartHeaterSequence(uint8_t level); // 开始加热序列(10分钟自动停止)
    void StopHeaterSequence();               // 停止加热序列
    
    // PWM 状态
    uint32_t pwm_counter_ = 0;
    static const uint32_t PWM_PERIOD = 30; // PWM周期 100*10ms = 1秒
};

#endif