# MODI 总线电机控制与人机交互软件 v1.0

基于 ESP32-S3、M0603A 总线电机和 480×320 触摸屏的嵌入式控制软件。
从任意起始角度统计电机轴相对圈数，支持自由运行、指定圈数运行、暂停继续和回本次起点。

## 功能

- 自由模式持续计圈；定圈模式目标为 1–9999 整数圈。
- 设定转速 50 / 100 / 200 / 300 / 380 RPM，显示实际相对圈数至两位小数。
- 启动和继续从 30 RPM 平滑加速，接近目标自动减速；手动暂停和停止立即协议刹车。
- 触摸与实体按键共用命令队列：GPIO12 启动/暂停/继续，GPIO15 停止。
- 新运行释放电机并准备新任务；回位使用保留起点，途中停止后可继续回位。
- U 盘追加写入 `LOGS/MOTOR.CSV`，包含原始反馈、任务起点、阶段和最终读数。

## 构建和测试

要求 ESP-IDF v5.3、ESP32-S3 工具链、Python 3；主机测试另需 C++17 编译器和 CMake。
在已加载 ESP-IDF 环境的终端运行：

```sh
idf.py build
sh tests/run_host_tests.sh
# 连接设备后，替换实际串口：
idf.py -p /dev/ttyACM0 flash monitor
```

目标芯片和板级参数已在 sdkconfig 中配置。首次构建通过组件管理器下载依赖，
版本由 dependencies.lock 锁定；主机 HMI 测试需要先完成依赖下载。

## 目录

| 路径 | 内容 |
| --- | --- |
| main/ | 程序入口与 NVS 初始化 |
| components/app/ | 任务状态机、位置跟踪、速度控制、HMI、输入和日志服务 |
| components/bsp/ | 电机、LCD、触摸、USB 和 TF 卡驱动 |
| tests/ | 算术、状态机、日志回放、故障注入和实际 LVGL 交互测试 |
| artifacts/diagnostics/ | 回归测试使用的实机原始日志 |
| docs/ | 使用说明、设计说明、协议及版本资料 |
| tools/ | 源程序整理工具 |

## 文档

- [使用说明书](docs/USER-MANUAL.md)
- [软件设计说明书](docs/SOFTWARE-DESIGN.md)
- [版本记录及验证](docs/RELEASE-NOTES.md)
- [软著材料整理说明](docs/COPYRIGHT-MATERIALS.md)
- [M0603A 协议参考](docs/MOTOR-PROTOCOL.md)
- [第三方组件与许可](docs/THIRD-PARTY.md)

v1.0 已通过软件回归与固件编译。任意起点、正反向、各档速度和停止误差 ±0.5 圈属于实机验收项目，
本版本发布不表示这些机械验收已完成。传感器数值仍为占位解析，不能作为标定测量值。

工程沿用 [MIT 许可](LICENSE)，第三方依赖与字体遵循各自许可。
