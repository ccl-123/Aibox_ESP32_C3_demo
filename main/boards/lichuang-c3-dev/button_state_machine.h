#ifndef BUTTON_STATE_MACHINE_H
#define BUTTON_STATE_MACHINE_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <functional>
#include <stdint.h>

enum class ButtonEvent {
    PRESS,      // 按键按下
    RELEASE,    // 按键释放
    CLICK,      // 单击
    DOUBLE_CLICK,  // 双击
    LONG_PRESS  // 长按(2秒)
};

enum class ButtonId {
    BUTTON_SUCK = 0,    // P0_0 夹吸加热按键
    BUTTON_ON = 1,      // P0_1 开关机按键
    BUTTON_ROCK = 2,    // P0_2 震动按键
    BUTTON_VOL = 3      // P0_3 音量按键
};

// 按键状态回调函数类型
using ButtonCallback = std::function<void(ButtonId button, ButtonEvent event)>;

class ButtonStateMachine {
public:
    ButtonStateMachine();
    ~ButtonStateMachine();
    
    // 设置按键事件回调
    void SetCallback(ButtonCallback callback);
    
    // 处理按键状态变化（从AW9523读取的按键状态）
    void ProcessButtonStates(uint8_t button_states);
    
    // 定时器处理函数（用于长按检测和双击超时）
    void ProcessTimer();

private:
    enum class ButtonState {
        IDLE,           // 空闲状态
        PRESSED,        // 按下状态
        RELEASED,       // 释放后等待双击
        LONG_PRESSING   // 长按状态
    };
    
    struct ButtonInfo {
        ButtonState state = ButtonState::IDLE;
        bool current_pressed = false;   // 当前按键状态
        bool last_pressed = false;      // 上次按键状态
        uint32_t press_time = 0;        // 按下时间戳
        uint32_t release_time = 0;      // 释放时间戳
        bool long_press_fired = false;  // 长按事件是否已触发
        bool waiting_double_click = false; // 是否正在等待双击
    };
    
    ButtonInfo buttons_[4];  // 4个按键的状态信息
    ButtonCallback callback_;
    
    // 时间常量（毫秒）
    static const uint32_t LONG_PRESS_TIME = 2000;      // 长按时间: 2秒
    static const uint32_t DOUBLE_CLICK_TIME = 400;     // 双击超时: 400ms
    static const uint32_t DEBOUNCE_TIME = 50;          // 去抖时间: 50ms
    
    // 获取当前时间戳（毫秒）
    uint32_t GetTimeMs();
    
    // 处理单个按键状态
    void ProcessSingleButton(ButtonId button_id, bool pressed);
    
    // 触发按键事件
    void TriggerEvent(ButtonId button_id, ButtonEvent event);
};

#endif // BUTTON_STATE_MACHINE_H
