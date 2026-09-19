#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

#include "nvs_storage.h"

static const char *TAG = "NVS_STORE";
static const char *NVS_NAMESPACE = "water";
static const char *s_keys[WATER_CHANNEL_COUNT] = {"cold_l", "hot_l"};

esp_err_t nvs_storage_load(uint64_t liters[WATER_CHANNEL_COUNT])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No stored counters yet, using initial values");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open failed");

    for (int ch = 0; ch < WATER_CHANNEL_COUNT; ch++) {
        uint64_t v = 0;
        err = nvs_get_u64(h, s_keys[ch], &v);
        if (err == ESP_OK) {
            liters[ch] = v;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "read %s failed: %s", s_keys[ch], esp_err_to_name(err));
        }
    }
    nvs_close(h);
    return ESP_OK;
}

esp_err_t nvs_storage_save(const uint64_t liters[WATER_CHANNEL_COUNT])
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs_open failed");

    esp_err_t err = ESP_OK;
    for (int ch = 0; ch < WATER_CHANNEL_COUNT && err == ESP_OK; ch++) {
        err = nvs_set_u64(h, s_keys[ch], liters[ch]);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
