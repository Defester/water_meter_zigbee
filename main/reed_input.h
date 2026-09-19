#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/**
 * @brief Configure reed switch GPIOs (pull-up, falling-edge interrupt).
 *
 * The ISR applies a per-channel time debounce and posts the channel index
 * (uint8_t, WATER_CH_*) to @p queue. It never touches the Zigbee API.
 */
esp_err_t reed_input_init(QueueHandle_t queue);
