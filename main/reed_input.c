#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "reed_input.h"
#include "water_meter.h"

static const char *TAG = "REED";

static const gpio_num_t s_pins[WATER_CHANNEL_COUNT] = {
    [WATER_CH_COLD] = CONFIG_WATER_COLD_GPIO,
    [WATER_CH_HOT] = CONFIG_WATER_HOT_GPIO,
};
static int64_t s_last_accepted_us[WATER_CHANNEL_COUNT];
static QueueHandle_t s_queue;

static void IRAM_ATTR reed_isr(void *arg)
{
    uint8_t ch = (uint8_t)(uintptr_t)arg;
    int64_t now = esp_timer_get_time();

    /* software debounce: drop edges closer than WATER_DEBOUNCE_US to the last accepted one */
    if (now - s_last_accepted_us[ch] < WATER_DEBOUNCE_US) {
        return;
    }
    s_last_accepted_us[ch] = now;

    BaseType_t higher_woken = pdFALSE;
    xQueueSendFromISR(s_queue, &ch, &higher_woken);
    if (higher_woken) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t reed_input_init(QueueHandle_t queue)
{
    ESP_RETURN_ON_FALSE(queue, ESP_ERR_INVALID_ARG, TAG, "queue is NULL");
    s_queue = queue;

    /* allow the very first pulse right after boot */
    for (int ch = 0; ch < WATER_CHANNEL_COUNT; ch++) {
        s_last_accepted_us[ch] = -2 * WATER_DEBOUNCE_US;
    }

    esp_err_t err = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "isr service install failed");

    for (int ch = 0; ch < WATER_CHANNEL_COUNT; ch++) {
        gpio_config_t cfg = {
            .pin_bit_mask = BIT64(s_pins[ch]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config(%d) failed", s_pins[ch]);
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(s_pins[ch], reed_isr, (void *)(uintptr_t)ch), TAG,
                            "isr handler add(%d) failed", s_pins[ch]);
        ESP_LOGI(TAG, "%s water reed switch on GPIO%d", ch == WATER_CH_COLD ? "Cold" : "Hot", s_pins[ch]);
    }
    return ESP_OK;
}
