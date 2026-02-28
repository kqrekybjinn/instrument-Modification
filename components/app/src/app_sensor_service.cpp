#include "app_sensor_service.hpp"

#include "freertos/task.h"

#include "driver/uart.h"

#include "esp_check.h"
#include "esp_log.h"

extern "C" {
#include "bsp_pinmap.h"
#include "gui_app.h"
}

static const char *TAG = "APP";

#define SENSOR_UART_NUM BSP_SENSOR_UART_NUM
#define SENSOR_TX_PIN   BSP_SENSOR_UART_TX_GPIO
#define SENSOR_RX_PIN   BSP_SENSOR_UART_RX_GPIO
#define SENSOR_BAUD     115200

SensorService::SensorService(ControlService &control) : control_(control) {}

void SensorService::start_task(int core_id)
{
    xTaskCreatePinnedToCore(task_trampoline, "sensor", 5120, this, 4, NULL, core_id);
}

void SensorService::task_trampoline(void *arg)
{
    static_cast<SensorService *>(arg)->task_loop();
}

esp_err_t SensorService::sensor_uart_init()
{
    uart_config_t cfg{};
    cfg.baud_rate = SENSOR_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    ESP_RETURN_ON_ERROR(uart_param_config(SENSOR_UART_NUM, &cfg), TAG, "uart_param_config sensor");
    ESP_RETURN_ON_ERROR(uart_set_pin(SENSOR_UART_NUM, SENSOR_TX_PIN, SENSOR_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart_set_pin sensor");
    ESP_RETURN_ON_ERROR(uart_driver_install(SENSOR_UART_NUM, 512, 0, 0, NULL, 0), TAG, "uart_driver_install sensor");
    ESP_RETURN_ON_ERROR(uart_set_mode(SENSOR_UART_NUM, UART_MODE_RS485_HALF_DUPLEX), TAG, "uart_set_mode RS485");

    ESP_LOGI(TAG, "Sensor UART2 init (TX=%d RX=%d baud=%d)", SENSOR_TX_PIN, SENSOR_RX_PIN, SENSOR_BAUD);
    return ESP_OK;
}

int32_t SensorService::parse_sensor_value(const uint8_t *data, int len)
{
    if (len <= 0 || !data) {
        return 0;
    }
    int32_t acc = 0;
    for (int i = 0; i < len; i++) {
        acc += data[i];
    }
    return acc / len;
}

void SensorService::task_loop()
{
    if (sensor_uart_init() != ESP_OK) {
        ESP_LOGE(TAG, "Sensor UART init failed");
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[64];
    while (1) {
        int len = uart_read_bytes(SENSOR_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (len > 0) {
            int32_t val = parse_sensor_value(buf, len);
            control_.set_sensor_value(val);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
