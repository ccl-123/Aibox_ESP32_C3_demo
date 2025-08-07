#include "esp32_s3_szp.h"

static const char *TAG = "esp32_s3_szp";
#include "boards/common/i2c_device.h"
// 在源文件中定义
#include <iostream>
#include <chrono>
#include <iostream>
#include <chrono>

using namespace std;
using namespace std::chrono;

class JumpDebouncer {
private:
    milliseconds debounce_time;
    int last_stable_number;  // 记录最后一个稳定的数字
    int candidate_number;    // 记录可能的新值
    steady_clock::time_point last_time;

public:
    JumpDebouncer(int debounce_ms) : debounce_time(debounce_ms), last_stable_number(0), candidate_number(-1) {
        last_time = steady_clock::now();
    }

    int processNumber(int number) {
        auto now = steady_clock::now();
        auto duration = duration_cast<milliseconds>(now - last_time);

        if (number != last_stable_number) {  // 发生跳变
            if (number != candidate_number) {
                // 发现新候选值，重置时间
                candidate_number = number;
                last_time = now;
            } else if (duration.count() > debounce_time.count()) {
                // 新值稳定超过窗口时间，接受跳变
                last_stable_number = number;
            }
        }
        return  last_stable_number;
    }

};
JumpDebouncer  kJumpDebouncer(600); 
namespace {

uint8_t qmi8658_device_addr = 0x6A;  // 默认地址
}
I2cDevice *i2c_device;
/******************************************************************************/
/***************************  I2C ↓ *******************************************/
// esp_err_t bsp_i2c_init(void)
// {
//     i2c_config_t i2c_conf = {
//         .mode = I2C_MODE_MASTER,
//         .sda_io_num = BSP_I2C_SDA,
//         .scl_io_num = BSP_I2C_SCL,
//         .sda_pullup_en = GPIO_PULLUP_ENABLE,
//         .scl_pullup_en = GPIO_PULLUP_ENABLE,
//         .master.clk_speed = BSP_I2C_FREQ_HZ
//     };
//     i2c_param_config(BSP_I2C_NUM, &i2c_conf);

//     return i2c_driver_install(BSP_I2C_NUM, i2c_conf.mode, 0, 0, 0);
// }
/***************************  I2C ↑ *******************************************/
/*******************************************************************************/

/*******************************************************************************/
/***************************  姿态传感器 QMI8658 ↓ ****************************/

// 读取QMI8658寄存器的值
esp_err_t qmi8658_register_read(uint8_t reg_addr, uint8_t *data, size_t len) {
  i2c_device->ReadRegs(reg_addr, data, len);
  return 0;
  //   return i2c_master_write_read_device(BSP_I2C_NUM, QMI8658_SENSOR_ADDR,
  //                                       &reg_addr, 1, data, len,
  //                                       1000 / portTICK_PERIOD_MS);
}

// 给QMI8658的寄存器写值
esp_err_t qmi8658_register_write_byte(uint8_t reg_addr, uint8_t data) {
  //
  i2c_device->WriteReg(reg_addr, data);
//   uint8_t write_buf[2] = {reg_addr, data};
  return 0;
  //   return i2c_master_write_to_device(BSP_I2C_NUM, QMI8658_SENSOR_ADDR,
  //   write_buf,
  //                                     sizeof(write_buf),
  //                                     1000 / portTICK_PERIOD_MS);
}

// 初始化qmi8658
void qmi8658_init( I2cDevice *i2c) {
  uint8_t id = 0;  // 芯片的ID号
  i2c_device = i2c;
  qmi8658_register_read(QMI8658_WHO_AM_I, &id, 1);  // 读芯片的ID号
  while (id != 0x05)                                // 判断读到的ID号是否是0x05
  {
    vTaskDelay(1000 / portTICK_PERIOD_MS);            // 延时1秒
    qmi8658_register_read(QMI8658_WHO_AM_I, &id, 1);  // 读取ID号
  }
  ESP_LOGI(TAG, "QMI8658 OK!");  // 打印信息

  qmi8658_register_write_byte(QMI8658_RESET, 0xb0);  // 复位
  vTaskDelay(10 / portTICK_PERIOD_MS);               // 延时10ms
  // qmi8658_register_write_byte(QMI8658_CTRL1, 0x40); // CTRL1 设置地址自动增加
  // qmi8658_register_write_byte(QMI8658_CTRL7, 0x03); // CTRL7
  // 允许加速度和陀螺仪 qmi8658_register_write_byte(QMI8658_CTRL2, 0x95); //
  // CTRL2 设置ACC 4g 250Hz qmi8658_register_write_byte(QMI8658_CTRL3, 0xd5); //
  // CTRL3 设置GRY 512dps 250Hz
  qmi8658_register_write_byte(QMI8658_CTRL1, 0x60);  // CTRL1 设置地址自动增加
  qmi8658_register_write_byte(QMI8658_CTRL7, 0x03);  // CTRL7 允许加速度和陀螺仪
  qmi8658_register_write_byte(QMI8658_CTRL2, 0x15);  // CTRL2 设置ACC 4g 250Hz
  qmi8658_register_write_byte(QMI8658_CTRL3,
                              0x00);  // CTRL3 设置GRY 512dps 250Hz
}

// 读取加速度和陀螺仪寄存器值
void qmi8658_Read_AccAndGry(t_sQMI8658 *p) {
  uint8_t status, data_ready = 0;
  int16_t buf[6];

  qmi8658_register_read(QMI8658_STATUS0, &status, 1);  // 读状态寄存器
  if (status & 0x03)  // 判断加速度和陀螺仪数据是否可读
    data_ready = 1;
  if (data_ready == 1) {  // 如果数据可读
    data_ready = 0;
    qmi8658_register_read(QMI8658_AX_L, (uint8_t *)buf,
                          12);  // 读加速度和陀螺仪值
    p->acc_x = buf[0];
    p->acc_y = buf[1];
    p->acc_z = buf[2];
    p->gyr_x = buf[3];
    p->gyr_y = buf[4];
    p->gyr_z = buf[5];
  }
}

motion_level_t qmi8658_detect_motion(t_sQMI8658 *p) {
  // 数据有效性检查
  if (abs(p->acc_x) > 32768 || abs(p->acc_y) > 32768 || abs(p->acc_z) > 32768) {
    return MOTION_LEVEL_IDLE;
  }

  // 使用定点数计算代替浮点数计算
  static const int32_t LSB_TO_G_FIXED = 8;  // 1/8192 in Q16.16
  static const int32_t ALPHA_FIXED = 52429; // 0.8 in Q16.16 (0.8*65536)
  static const int32_t ONE_FIXED = 65536;   // 1.0 in Q16.16

  // 更好的初始化历史值 - 首次运行时使用当前值初始化
  static int32_t last_acc_x_fixed = 0;
  static int32_t last_acc_y_fixed = 0;
  static int32_t last_acc_z_fixed = 0;
  static bool first_run = true;

  // 当前加速度转换为定点数表示 (g单位)
  int32_t curr_acc_x_fixed = (p->acc_x * LSB_TO_G_FIXED);
  int32_t curr_acc_y_fixed = (p->acc_y * LSB_TO_G_FIXED);
  int32_t curr_acc_z_fixed = (p->acc_z * LSB_TO_G_FIXED);

  // 首次运行时，初始化历史值为当前值
  if (first_run) {
    last_acc_x_fixed = curr_acc_x_fixed;
    last_acc_y_fixed = curr_acc_y_fixed;
    last_acc_z_fixed = curr_acc_z_fixed;
    first_run = false;
    return MOTION_LEVEL_IDLE; // 首次运行返回静止状态
  }

  // 计算变化量 - 使用前一次的原始值，而不是滤波后的值
  int32_t delta_x_fixed = abs(curr_acc_x_fixed - last_acc_x_fixed);
  int32_t delta_y_fixed = abs(curr_acc_y_fixed - last_acc_y_fixed);
  int32_t delta_z_fixed = abs(curr_acc_z_fixed - last_acc_z_fixed);

  // 更新历史值 - 直接使用当前值，不使用滤波
  last_acc_x_fixed = curr_acc_x_fixed;
  last_acc_y_fixed = curr_acc_y_fixed;
  last_acc_z_fixed = curr_acc_z_fixed;

  // 计算总变化量
  int32_t total_delta_fixed = delta_x_fixed + delta_y_fixed + delta_z_fixed;

  // 阈值检查
  motion_level_t motion_level;
  if (total_delta_fixed < 3277) {
    motion_level = MOTION_LEVEL_IDLE;      // 静止
  } else if (total_delta_fixed < 13107) {
    motion_level = MOTION_LEVEL_SLIGHT;    // 轻微运动
  } else if (total_delta_fixed < 26214) {
    motion_level = MOTION_LEVEL_MODERATE;  // 中等运动
  } else {
    motion_level = MOTION_LEVEL_INTENSE;   // 剧烈运动
  }

  return motion_level;
}

// 使用示例函数
t_sQMI8658 qmi8658_motion_demo(void) {
  t_sQMI8658 imu_data;
  motion_level_t motion;

  // 读取IMU数据
  qmi8658_Read_AccAndGry(&imu_data);

  // 检测运动强度
  motion = qmi8658_detect_motion(&imu_data);

  // ESP_LOGW(TAG, "before Motion%d: ", motion);
  motion =
      static_cast<motion_level_t>(kJumpDebouncer.processNumber(int(motion)));
  // ESP_LOGW(TAG, "after Motion%d: ", motion);
  std::vector<bool> continue_state_;
  // 打印运动强度
  switch (motion) {
    case MOTION_LEVEL_IDLE:
      // ESP_LOGW(TAG, "Motion: IDLE");
      break;
    case MOTION_LEVEL_SLIGHT:
      // ESP_LOGW(TAG, "Motion: SLIGHT");
      break;
    case MOTION_LEVEL_MODERATE:
      // ESP_LOGW(TAG, "Motion: MODERATE");
      break;
    case MOTION_LEVEL_INTENSE:
      // ESP_LOGW(TAG, "Motion: INTENSE");
      break;
  }
  imu_data.motion = int(motion);
  return imu_data;
}

// 获取XYZ轴的倾角值
void qmi8658_fetch_angleFromAcc(t_sQMI8658 *p) {
  float temp;

  qmi8658_Read_AccAndGry(p);  // 读取加速度和陀螺仪的寄存器值
  // 根据寄存器值 计算倾角值 并把弧度转换成角度
  temp = (float)p->acc_x / sqrt(((float)p->acc_y * (float)p->acc_y +
                                 (float)p->acc_z * (float)p->acc_z));
  p->AngleX = atan(temp) * 57.29578f;  // 180/π=57.29578
  temp = (float)p->acc_y / sqrt(((float)p->acc_x * (float)p->acc_x +
                                 (float)p->acc_z * (float)p->acc_z));
  p->AngleY = atan(temp) * 57.29578f;  // 180/π=57.29578
  temp = sqrt(((float)p->acc_x * (float)p->acc_x +
               (float)p->acc_y * (float)p->acc_y)) /
         (float)p->acc_z;
  p->AngleZ = atan(temp) * 57.29578f;  // 180/π=57.29578
}

/***************************  姿态传感器 QMI8658 ↑ ****************************/
/*******************************************************************************/
