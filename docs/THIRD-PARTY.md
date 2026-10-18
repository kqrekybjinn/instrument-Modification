# 第三方组件与许可

工程原有 MIT 许可保留于 LICENSE；不改变原代码权利声明。

| 组件 | 用途 | 来源与许可 |
| --- | --- | --- |
| ESP-IDF v5.3 | 芯片 SDK、FreeRTOS、驱动与构建 | https://github.com/espressif/esp-idf ，各文件遵循 SDK 原许可 |
| LVGL 8 | 图形界面 | https://github.com/lvgl/lvgl ，MIT，精确版本见 dependencies.lock |
| espressif/usb_host_msc | USB 大容量存储 | https://components.espressif.com/components/espressif/usb_host_msc ，许可随组件下载 |
| 文泉驿微米黑字体子集 | 中文界面字形 | 见 FONT-LICENSE.txt，生成字库保留原声明 |

managed_components 是可重新下载的第三方依赖目录，不作为本项目原创代码上传或纳入业务源程序材料。
sdkconfig、依赖清单和锁文件保留以支持构建复现。第三方原始声明不得从分发中删除。
