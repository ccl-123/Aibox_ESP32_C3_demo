#include "button_state_machine.h"
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "ButtonStateMachine"

ButtonStateMachine::ButtonStateMachine() {
    ESP_LOGI(TAG, "按键状态机初始化");
}

ButtonStateMachine::~ButtonStateMachine() {
}

void ButtonStateMachine::SetCallback(ButtonCallback callback) {
    callback_ = callback;
}

uint32_t ButtonStateMachine::GetTimeMs() {
    return esp_timer_get_time() / 1000;
}

void ButtonStateMachine::ProcessButtonStates(uint8_t button_states) {
    // 添加初始化保护：设备启动后5秒内不处理按键事件，避免误触发
    // static uint32_t init_time = GetTimeMs();
    // uint32_t current_time = GetTimeMs();
    // if (current_time - init_time < 1300) {
    //     // 初始化期间，只更新状态，不处理事件
    //     static bool init_warning_shown = false;
    //     if (!init_warning_shown) {
    //         ESP_LOGI(TAG, "按键系统初始化保护期：1.3秒内忽略按键事件");
    //         init_warning_shown = true;
    //     }
    //     return;
    // }
    
    // 处理4个按键的状态变化
    // 根据用户最终确认：按键按下时被拉高，未按下时为低电平
    // 初始状态应该是0x00（所有按键未按下，低电平）
    static uint8_t last_states = 0x00;  // 正确的初始状态：所有按键未按下（低电平）
    
    if (button_states != last_states) {
        ESP_LOGI(TAG, "按键状态变化: 0x%02X -> 0x%02X", last_states, button_states);
        last_states = button_states;
    }
    
    for (int i = 0; i < 4; i++) {
        ButtonId button_id = static_cast<ButtonId>(i);
        
        // 最终确认的硬件逻辑（用户最终澄清）：
        // - 按键未按下时：P0_x = 低电平 (0) ← 默认状态
        // - 按键按下时：  P0_x = 高电平 (1) ← 硬件电路拉高
        // 因此：pressed = (bit == 1) 表示按键被按下
        bool pressed = (button_states & (1 << i)) != 0;  // 高电平有效：bit=1时按键被按下
        
        // 保留原来的极性切换代码以备用
        #ifdef BUTTON_ACTIVE_HIGH_OVERRIDE
        pressed = !pressed;  // 低电平有效
        #endif
        
        ProcessSingleButton(button_id, pressed);
    }
}

void ButtonStateMachine::ProcessSingleButton(ButtonId button_id, bool pressed) {
    int idx = static_cast<int>(button_id);
    ButtonInfo& btn = buttons_[idx];
    uint32_t current_time = GetTimeMs();
    
    // 检测按键状态变化
    if (pressed != btn.last_pressed) {
        if (pressed) {
            // 按键按下
            ESP_LOGI(TAG, "按键%d按下", idx);
            btn.current_pressed = true;
            btn.press_time = current_time;
            btn.long_press_fired = false;
            btn.state = ButtonState::PRESSED;
            TriggerEvent(button_id, ButtonEvent::PRESS);
        } else {
            // 按键释放
            ESP_LOGI(TAG, "按键%d释放", idx);
            btn.current_pressed = false;
            btn.release_time = current_time;
            
            if (btn.state == ButtonState::PRESSED && !btn.long_press_fired) {
                // 短按释放，可能是单击或双击的第一次
                if (btn.waiting_double_click) {
                    // 这是双击
                    ESP_LOGI(TAG, "按键%d双击检测", idx);
                    btn.waiting_double_click = false;
                    btn.state = ButtonState::IDLE;
                    TriggerEvent(button_id, ButtonEvent::DOUBLE_CLICK);
                } else {
                    // 等待可能的双击
                    btn.waiting_double_click = true;
                    btn.state = ButtonState::RELEASED;
                }
            } else if (btn.state == ButtonState::LONG_PRESSING) {
                // 长按后释放
                btn.state = ButtonState::IDLE;
            }
            
            TriggerEvent(button_id, ButtonEvent::RELEASE);
        }
        btn.last_pressed = pressed;
    }
}

void ButtonStateMachine::ProcessTimer() {
    uint32_t current_time = GetTimeMs();
    
    for (int i = 0; i < 4; i++) {
        ButtonInfo& btn = buttons_[i];
        ButtonId button_id = static_cast<ButtonId>(i);
        
        switch (btn.state) {
            case ButtonState::PRESSED:
                // 检查长按
                if (btn.current_pressed && 
                    (current_time - btn.press_time) >= LONG_PRESS_TIME && 
                    !btn.long_press_fired) {
                    ESP_LOGI(TAG, "按键%d长按检测", i);
                    btn.long_press_fired = true;
                    btn.state = ButtonState::LONG_PRESSING;
                    TriggerEvent(button_id, ButtonEvent::LONG_PRESS);
                }
                break;
                
            case ButtonState::RELEASED:
                // 检查双击超时
                if ((current_time - btn.release_time) >= DOUBLE_CLICK_TIME) {
                    if (btn.waiting_double_click) {
                        // 双击超时，确认为单击
                        ESP_LOGI(TAG, "按键%d单击检测", i);
                        btn.waiting_double_click = false;
                        btn.state = ButtonState::IDLE;
                        TriggerEvent(button_id, ButtonEvent::CLICK);
                    }
                }
                break;
                
            default:
                break;
        }
    }
}

void ButtonStateMachine::TriggerEvent(ButtonId button_id, ButtonEvent event) {
    if (callback_) {
        callback_(button_id, event);
    }
}
