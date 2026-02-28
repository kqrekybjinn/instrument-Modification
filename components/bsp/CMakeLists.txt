idf_component_register(
    SRCS
        "src/bsp_lcd.c"
        "src/bsp_touch_ft6336u.c"
        "src/motor_driver.c"
        "src/msc_driver.c"
        "src/tf_card.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl lvgl__lvgl esp_driver_gpio esp_driver_uart esp_driver_i2c
    PRIV_REQUIRES usb esp_lcd driver nvs_flash esp_timer fatfs sdmmc esp_driver_sdmmc
)
