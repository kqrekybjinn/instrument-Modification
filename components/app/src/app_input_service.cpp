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
    bool stable[2] = {false, false};
    bool candidate[2] = {false, false};
    unsigned unchanged[2] = {0, 0};
    while (true) {
        const bool raw[2] = {key_pressed(KEY_START_PIN), key_pressed(KEY_STOP_PIN)};
        for (int i = 0; i < 2; ++i) {
            if (raw[i] != candidate[i]) { candidate[i] = raw[i]; unchanged[i] = 1; }
            else if (unchanged[i] < 3) ++unchanged[i];
            if (unchanged[i] >= 3 && stable[i] != candidate[i]) {
                stable[i] = candidate[i];
                if (stable[i]) control_.push_cmd(i == 0 ? CMD_STATE_TOGGLE : CMD_STOP);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_SCAN_MS));
    }
}
