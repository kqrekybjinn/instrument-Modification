# 日志为空原因与修复（2026-10-18）
只读检查F:/DEBUG与F:/LOGS：DEBUG/USB_TEST.TXT=19字节，内容usb_msc_write_test；LOGS空。未改动F盘。
sdkconfig启用CONFIG_FATFS_LFN_NONE。旧motor_log.csv的主名motor_log长9字符，不满足8.3，而usb_test为8字符；因此现配置下旧日志路径无法创建。这解释DEBUG正常而LOGS没有文件。
改用logs/motor.csv。旧代码忽略所有msc_write_file错误、失败后仍header_written=true；现仅成功才推进状态，失败记录错误（限频）并在后续采样重试。
不再wb截断旧日志；检查已有文件首字节，已有内容时直接追加，否则以append写表头。重新挂载重置日志阶段，重新记录当前状态。
底层检查mkdir、fflush、fsync和fclose结果；打开/写入失败记录路径和errno，不再错误报告ESP_OK。
测试实际UsbLogService::sample_once，模拟表头失败、数据失败、重试、Ready不重复写、运行/最终停止、重插保留旧内容与不重复表头。全部主机回归、ESP-IDF build和diff --check通过；未烧录，USB真实介质写入需新版固件实机确认。
备份 /home/amu/backups/modi-before-usb-log-fix-20261018/sources.tar。
