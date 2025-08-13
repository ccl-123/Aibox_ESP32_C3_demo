#ifndef LICHUANG_C3_DEV_BOARD_H
#define LICHUANG_C3_DEV_BOARD_H

#include "board.h"

// 前向声明
class LichuangC3DevBoard;

// 声明LichuangC3DevBoard类，用于在application.cc中进行类型转换
class LichuangC3DevBoard : public Board {
public:
    // AW9523功能测试函数
    void TestAw9523();
};

#endif // LICHUANG_C3_DEV_BOARD_H
