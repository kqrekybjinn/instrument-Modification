idf_component_register(
    SRCS
        "src/app_controller.cpp"
        "src/gui_app.cpp"
        "src/lv_font_modi_16_cjk.c"
        "src/app_control_service.cpp"
        "src/app_gui_service.cpp"
        "src/app_input_service.cpp"
        "src/app_sensor_service.cpp"
        "src/app_usb_host_service.cpp"
        "src/app_usb_log_service.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES bsp
    PRIV_REQUIRES usb esp_timer lvgl lvgl__lvgl
)
