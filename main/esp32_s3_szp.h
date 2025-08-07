#pragma once

#include "boards/common/i2c_device.h"
#include <stdio.h>
#include "esp_err.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "math.h"
#include "string"
/******************************************************************************/
/***************************  I2C ↓ *******************************************/
#define BSP_I2C_SDA           (GPIO_NUM_1)   // SDA引脚
#define BSP_I2C_SCL           (GPIO_NUM_2)   // SCL引脚

#define BSP_I2C_NUM           i2c_port_t(0)            // I2C外设
#define BSP_I2C_FREQ_HZ       100000         // 100kHz

esp_err_t bsp_i2c_init(void);   // 初始化I2C接口
/***************************  I2C ↑  *******************************************/
/*******************************************************************************/


/*******************************************************************************/
/***************************  姿态传感器 QMI8658 ↓   ****************************/
#define  QMI8658_SENSOR_ADDR       0x6A   // QMI8658 I2C地址

// QMI8658寄存器地址
enum qmi8658_reg
{
    QMI8658_WHO_AM_I,
    QMI8658_REVISION_ID,
    QMI8658_CTRL1,
    QMI8658_CTRL2,
    QMI8658_CTRL3,
    QMI8658_CTRL4,
    QMI8658_CTRL5,
    QMI8658_CTRL6,
    QMI8658_CTRL7,
    QMI8658_CTRL8,
    QMI8658_CTRL9,
    QMI8658_CATL1_L,
    QMI8658_CATL1_H,
    QMI8658_CATL2_L,
    QMI8658_CATL2_H,
    QMI8658_CATL3_L,
    QMI8658_CATL3_H,
    QMI8658_CATL4_L,
    QMI8658_CATL4_H,
    QMI8658_FIFO_WTM_TH,
    QMI8658_FIFO_CTRL,
    QMI8658_FIFO_SMPL_CNT,
    QMI8658_FIFO_STATUS,
    QMI8658_FIFO_DATA,
    QMI8658_STATUSINT = 45,
    QMI8658_STATUS0,
    QMI8658_STATUS1,
    QMI8658_TIMESTAMP_LOW,
    QMI8658_TIMESTAMP_MID,
    QMI8658_TIMESTAMP_HIGH,
    QMI8658_TEMP_L,
    QMI8658_TEMP_H,
    QMI8658_AX_L,
    QMI8658_AX_H,
    QMI8658_AY_L,
    QMI8658_AY_H,
    QMI8658_AZ_L,
    QMI8658_AZ_H,
    QMI8658_GX_L,
    QMI8658_GX_H,
    QMI8658_GY_L,
    QMI8658_GY_H,
    QMI8658_GZ_L,
    QMI8658_GZ_H,
    QMI8658_COD_STATUS = 70,
    QMI8658_dQW_L = 73,
    QMI8658_dQW_H,
    QMI8658_dQX_L,
    QMI8658_dQX_H,
    QMI8658_dQY_L,
    QMI8658_dQY_H,
    QMI8658_dQZ_L,
    QMI8658_dQZ_H,
    QMI8658_dVX_L,
    QMI8658_dVX_H,
    QMI8658_dVY_L,
    QMI8658_dVY_H,
    QMI8658_dVZ_L,
    QMI8658_dVZ_H,
    QMI8658_TAP_STATUS = 89,
    QMI8658_STEP_CNT_LOW,
    QMI8658_STEP_CNT_MIDL,
    QMI8658_STEP_CNT_HIGH,
    QMI8658_RESET = 96
};

// 倾角结构体
typedef struct {
  int16_t acc_x = 0;
  int16_t acc_y = 0;
  int16_t acc_z = 0;
  int16_t gyr_x = 0;
  int16_t gyr_y = 0;
  int16_t gyr_z = 0;
  float AngleX = 0.0;
  float AngleY = 0.0;
  float AngleZ = 0.0;
  int motion = 0;
  std::string ToString()const {
    return std::to_string(acc_x) + " " + std::to_string(acc_y) + " " +
           std::to_string(acc_z) + " " + std::to_string(gyr_x) + " " +
           std::to_string(gyr_y) + " " + std::to_string(gyr_z);
  }
} t_sQMI8658;

// 定义震动等级
typedef enum {
    MOTION_LEVEL_IDLE = 0,    // 静止
    MOTION_LEVEL_SLIGHT = 1,  // 轻微运动
    MOTION_LEVEL_MODERATE = 2,// 中等运动
    MOTION_LEVEL_INTENSE = 3  // 剧烈运动
} motion_level_t;

// 卡尔曼滤波器结构体
typedef struct {
    float x;     // 状态估计值
    float p;     // 估计误差协方差
    float q;     // 过程噪声协方差
    float r;     // 测量噪声协方差
    float k;     // 卡尔曼增益
} KalmanFilter;

// I2C 读写函数声明
esp_err_t qmi8658_register_write_byte(uint8_t reg_addr, uint8_t data);

// 初始化和检测函数声明
void qmi8658_init(I2cDevice *i2c);
void qmi8658_fetch_angleFromAcc(t_sQMI8658 *p);  // 获取倾角
t_sQMI8658 qmi8658_motion_demo(void);
/***************************  姿态传感器 QMI8658 ↑  ****************************/
/*******************************************************************************/


