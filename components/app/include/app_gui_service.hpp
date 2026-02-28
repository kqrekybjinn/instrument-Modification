#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lvgl.h"

extern "C" {
#include "gui_app.h"
}

class GuiService {
public:
    explicit GuiService(gui_event_cb_t cb);

    void start_task(int core_id);

private:
    static void task_trampoline(void *arg);
    void task_loop();

    static void lvgl_no_touch(lv_indev_drv_t *indev_drv, lv_indev_data_t *data);
    static void lvgl_tick_task(void *arg);

    SemaphoreHandle_t lvgl_lock_ = nullptr;
    gui_event_cb_t event_cb_ = nullptr;
};
