#include "app_gui_service.hpp"

#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"

#include "bsp_lcd.h"
#include "bsp_touch_ft6336u.h"

static const char *TAG = "APP";

// Provide defaults when sdkconfig does not define panel resolution
#ifndef CONFIG_LCD_H_RES
#define CONFIG_LCD_H_RES 480
#endif
#ifndef CONFIG_LCD_V_RES
#define CONFIG_LCD_V_RES 272
#endif

GuiService::GuiService(gui_event_cb_t cb) : event_cb_(cb) {}

void GuiService::start_task(int core_id)
{
    xTaskCreatePinnedToCore(task_trampoline, "gui", 10240, this, 5, NULL, core_id);
}

void GuiService::task_trampoline(void *arg)
{
    static_cast<GuiService *>(arg)->task_loop();
}

void GuiService::lvgl_no_touch(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    (void)indev_drv;
    data->state = LV_INDEV_STATE_REL;
}

void GuiService::lvgl_tick_task(void *arg)
{
    (void)arg;
    lv_tick_inc(2);
}

void GuiService::task_loop()
{
    ESP_LOGI(TAG, "GUI task start");

    lvgl_lock_ = xSemaphoreCreateRecursiveMutex();
    if (!lvgl_lock_) {
        ESP_LOGE(TAG, "LVGL mutex create failed");
        vTaskDelete(NULL);
        return;
    }

    lv_init();

    esp_err_t lcd_err = bsp_lcd_init();
    if (lcd_err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_lcd_init failed: %s", esp_err_to_name(lcd_err));
    } else {
        // Clear once; UI will render the rest.
        (void)bsp_lcd_fill(0x0000);
    }

    const int lv_hor = CONFIG_LCD_V_RES;
    const int lv_ver = CONFIG_LCD_H_RES;

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[CONFIG_LCD_V_RES * 20];
    static lv_color_t buf2[CONFIG_LCD_V_RES * 20];
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, CONFIG_LCD_V_RES * 20);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.hor_res = lv_hor;
    disp_drv.ver_res = lv_ver;
    disp_drv.flush_cb = bsp_lcd_flush;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;

    esp_err_t touch_err = bsp_touch_init((uint16_t)lv_hor, (uint16_t)lv_ver);
    if (touch_err != ESP_OK) {
        ESP_LOGW(TAG, "Touch init failed (%s). Using no-touch stub.", esp_err_to_name(touch_err));
        indev_drv.read_cb = lvgl_no_touch;
    } else {
        indev_drv.read_cb = bsp_touch_read;
    }
    lv_indev_drv_register(&indev_drv);

    if (gui_app_init(lvgl_lock_, event_cb_) != ESP_OK) {
        ESP_LOGE(TAG, "GUI init failed");
    }

    esp_timer_create_args_t tick_args{};
    tick_args.callback = &lvgl_tick_task;
    tick_args.name = "lv_tick";

    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2000));

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (lvgl_lock_ && xSemaphoreTakeRecursive(lvgl_lock_, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGiveRecursive(lvgl_lock_);
        }
    }
}
