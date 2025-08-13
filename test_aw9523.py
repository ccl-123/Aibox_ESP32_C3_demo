#!/usr/bin/env python3
"""
AW9523B IO扩展板功能测试脚本
通过串口发送测试命令来验证AW9523功能
"""

import serial
import time
import sys

def send_test_commands(port='/dev/ttyUSB0', baudrate=115200):
    """发送测试命令到ESP32"""
    try:
        ser = serial.Serial(port, baudrate, timeout=1)
        print(f"连接到 {port}, 波特率 {baudrate}")
        
        # 等待设备稳定
        time.sleep(2)
        
        # 测试命令列表
        test_commands = [
            # I2C扫描命令（如果有的话）
            "i2c_scan\n",
            
            # AW9523寄存器读取测试
            "aw9523_test\n",
            
            # LED测试命令
            "led_test\n",
            
            # 按钮测试命令
            "button_test\n"
        ]
        
        for cmd in test_commands:
            print(f"发送命令: {cmd.strip()}")
            ser.write(cmd.encode())
            time.sleep(1)
            
            # 读取响应
            response = ""
            start_time = time.time()
            while time.time() - start_time < 3:  # 3秒超时
                if ser.in_waiting > 0:
                    data = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
                    response += data
                    print(data, end='')
                time.sleep(0.1)
            
            if response:
                print(f"\n--- 命令 {cmd.strip()} 响应结束 ---\n")
            else:
                print(f"命令 {cmd.strip()} 无响应\n")
        
        ser.close()
        
    except Exception as e:
        print(f"测试失败: {e}")

if __name__ == "__main__":
    port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
    send_test_commands(port)
