# Modi - ESP32-S3 电机控制器

## 项目概述
Modi 是一个基于 **ESP32-S3-WROOM-1U (N16R8)** 的嵌入式电机控制系统，通过 UART 串口协议控制 **M0603A 电机驱动器**，配备触摸屏 GUI、物理按键、传感器采集、USB/TF 存储及日志记录功能。

## 技术栈
- **MCU**: ESP32-S3 (ESP-IDF, C/C++ 混合)
- **构建**: CMake + idf.py
- **GUI**: LVGL v8 (480x272 8080并口LCD, FT6336U 触摸)
- **电机协议**: M0603A 串口 10字节帧, CRC-8/MAXIM, UART1 38400bps
- **传感器**: RS485 半双工, UART2 115200bps
- **存储**: USB MSC (U盘) + TF卡 (SDMMC 1-bit)

## 项目结构
```
main/                          # 入口 (app_main.cpp -> AppController::start)
components/
  app/                         # 应用层
    src/
      app_controller.cpp       # 主控制器，初始化所有 Service
      app_control_service.cpp  # 电机状态机（正转/停止/反转回位）
      app_gui_service.cpp      # LVGL 初始化 + 渲染循环 (Core 1)
      app_input_service.cpp    # 物理按键扫描 (GPIO12=Start, GPIO15=Stop)
      app_sensor_service.cpp   # RS485 传感器数据采集
      app_usb_host_service.cpp # USB Host 库安装
      app_usb_log_service.cpp  # 运行时 CSV 日志写入 U盘
      gui_app.cpp              # LVGL UI 布局与刷新
      lv_font_modi_16_cjk.c   # 自定义中文字体
    include/
      app_types.hpp            # 共享类型定义（状态、命令、速度档位）
  bsp/                         # 板级支持包
    src/
      motor_driver.c           # M0603A UART 帧封装与发送
      bsp_lcd.c                # 8080 并口 LCD 驱动
      bsp_touch_ft6336u.c      # I2C 触摸驱动
      msc_driver.c             # USB MSC class driver
      tf_card.c                # TF/SD 卡挂载读写
    include/
      bsp_pinmap.h             # 全局引脚定义
      motor_driver.h           # 电机驱动 API
```

## 核心业务逻辑
电机运动状态机 (ControlService):
- **正转**: 按 Start -> 以当前档位 RPM 正转，计时器正计时
- **停止**: 按 Stop -> 刹车，记录正转时长和转速
- **反转回位**: 再按 Start -> 按比例时长反转，计时器倒计时，到时自动刹车归零
- **速度档位**: 停止时按 Speed 键循环切换 30/60/90 RPM

## 编码约定
- BSP 层用纯 C，应用层用 C++ (Service 类封装 FreeRTOS task)
- 每个 Service 类有 `start_task(core_id)` + `task_trampoline` + `task_loop` 模式
- GUI 运行在 Core 1，其余 Service 运行在 Core 0
- LVGL 操作需通过 recursive mutex 加锁
- 电机指令通过 motor_driver.h 的高层 API 发送，底层自动追加 CRC
