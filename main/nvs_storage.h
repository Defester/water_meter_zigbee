#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "water_meter.h"

/**
 * @brief Load counters (liters) from NVS.
 * Channels without a stored value keep whatever is already in @p liters.
 */
esp_err_t nvs_storage_load(uint64_t liters[WATER_CHANNEL_COUNT]);

/** @brief Store counters (liters) to NVS. */
esp_err_t nvs_storage_save(const uint64_t liters[WATER_CHANNEL_COUNT]);
