#include "app_input_service.hpp"

#include "freertos/task.h"

#include "driver/gpio.h"

extern "C" {
#include "bsp_pinmap.h"
#include "gui_app.h"
}

#define KEY_START_PIN  BSP_KEY_START_GPIO
#define KEY_STOP_PIN   BSP_KEY_STOP_GPIO

InputService::InputService(ControlService &control) : control_(control) {}

void InputService::start_task(int core_id)
{
    xTaskCreatePinnedToCore(task_trampoline, "keys", 3072, this, 5, NULL, core_id);
}

void InputService::task_trampoline(void *arg)
{
    static_cast<InputService *>(arg)->task_loop();
}

void InputService::keys_init()
{
    uint64_t mask = BIT64(KEY_START_PIN) | BIT64(KEY_STOP_PIN);

    gpio_config_t cfg{};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
}

bool InputService::key_pressed(gpio_num_t pin)
{
    return gpio_get_level(pin) == 0;
}

void InputService::task_loop()
{
    keys_init();
    bool prev_start = false;
    bool prev_stop = false;

    while (1) {
        bool s = key_pressed(KEY_START_PIN);
        bool p = key_pressed(KEY_STOP_PIN);

        if (s && !prev_start) {
            control_state_t st = control_.snapshot();
            if (st.mode == MODE_COUNT) {
                // Count mode A key: pause/resume/start-forward
                control_.push_cmd(CMD_STATE_TOGGLE);
            } else {
                // Timer mode: keep UI and physical key behavior consistent
                gui_app_sync_button(GUI_EVENT_STATE_TOGGLE);
            }
        }
        if (p && !prev_stop) {
            control_state_t st = control_.snapshot();
            if (st.mode == MODE_COUNT) {
                // Count mode B key: stop forward/pause and enter reverse flow
                control_.push_cmd(CMD_COUNT_ABORT);
            } else {
                // Stop key in timer mode: cycle speed level
                gui_app_sync_button(GUI_EVENT_SPEED_STEP);
            }
        }
        prev_start = s;
        prev_stop = p;

        vTaskDelay(pdMS_TO_TICKS(KEY_SCAN_MS));
    }
}
