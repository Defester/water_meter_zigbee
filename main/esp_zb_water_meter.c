/*
 * Zigbee dual water meter (cold + hot) for M5Stack NanoH2 (ESP32-H2), End Device.
 *
 * Based on the Espressif HA_on_off_light example (esp-zigbee-lib 1.6.x, esp_zb_* API).
 *
 * Data flow:
 *   reed switch --(GPIO ISR, debounce)--> queue --> pulse_task --> counters (RAM)
 *                                                             |--> NVS (every N pulses / every T seconds / on shutdown)
 *                                                             '--> ZCL Metering attribute + report (under Zigbee lock)
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_zigbee_core.h"

#include "nvs_storage.h"
#include "reed_input.h"
#include "water_meter.h"

#if !defined ZB_ED_ROLE
#error Define ZB_ED_ROLE in idf.py menuconfig to compile this (End Device) source code.
#endif

static const char *TAG = "WATER_METER";

/* ------------------------------------------------------------------------- */
/* Counters                                                                   */
/* ------------------------------------------------------------------------- */

static uint64_t s_liters[WATER_CHANNEL_COUNT]; /* accumulated volume, liters */
static uint32_t s_unsaved_pulses;              /* pulses since the last NVS save */
static int64_t s_last_save_us;
static SemaphoreHandle_t s_counter_mutex;
static QueueHandle_t s_pulse_queue;
static volatile bool s_zb_ready;         /* commissioning done, attributes can be touched */
static volatile bool s_zb_stack_running; /* stack main loop is alive (any signal received) */
/* backing storage for the write-only calibration attribute, see WATER_ATTR_SET_VOLUME_ID */
static esp_zb_uint48_t s_set_volume_attr[WATER_CHANNEL_COUNT];

static esp_zb_uint48_t to_u48(uint64_t v)
{
    esp_zb_uint48_t r = {
        .low = (uint32_t)(v & 0xFFFFFFFFu),
        .high = (uint16_t)((v >> 32) & 0xFFFFu),
    };
    return r;
}

static uint64_t counter_get(int ch)
{
    uint64_t v;
    xSemaphoreTake(s_counter_mutex, portMAX_DELAY);
    v = s_liters[ch];
    xSemaphoreGive(s_counter_mutex);
    return v;
}

static void counter_set(int ch, uint64_t liters)
{
    xSemaphoreTake(s_counter_mutex, portMAX_DELAY);
    s_liters[ch] = liters;
    xSemaphoreGive(s_counter_mutex);
}

static int channel_from_endpoint(uint8_t ep)
{
    for (int ch = 0; ch < WATER_CHANNEL_COUNT; ch++) {
        if (WATER_ENDPOINT(ch) == ep) {
            return ch;
        }
    }
    return -1;
}

/* Persist counters to NVS. Also used as shutdown handler, so it must not block forever. */
static void counters_save(void)
{
    uint64_t snapshot[WATER_CHANNEL_COUNT];
    bool locked = xSemaphoreTake(s_counter_mutex, pdMS_TO_TICKS(200)) == pdTRUE;

    memcpy(snapshot, s_liters, sizeof(snapshot));
    s_unsaved_pulses = 0;
    s_last_save_us = esp_timer_get_time();
    if (locked) {
        xSemaphoreGive(s_counter_mutex);
    }

    esp_err_t err = nvs_storage_save(snapshot);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Counters saved: cold=%llu L, hot=%llu L", snapshot[WATER_CH_COLD], snapshot[WATER_CH_HOT]);
    } else {
        ESP_LOGE(TAG, "Failed to save counters: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------------------- */
/* Zigbee attribute update / reporting                                        */
/* ------------------------------------------------------------------------- */

/*
 * Does the actual set-attribute-and-report work, without touching the Zigbee lock: the
 * SDK forbids acquiring it from inside a Zigbee callback (the stack already holds it
 * there), so zb_handle_attr_write() below calls this directly, while zb_publish() wraps
 * it with the lock for callers running in a FreeRTOS task of their own (pulse_task,
 * button_task, the signal handler).
 */
static void zb_publish_locked(int ch, bool send_report)
{
    uint8_t ep = WATER_ENDPOINT(ch);
    esp_zb_uint48_t value = to_u48(counter_get(ch));

    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(ep, ESP_ZB_ZCL_CLUSTER_ID_METERING, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                                              ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID, &value, false);
    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "EP%d: set attribute failed, status 0x%02x", ep, status);
    }
    if (send_report && esp_zb_bdb_dev_joined()) {
        /* one-shot report to the coordinator, works even before Z2M has configured a binding */
        esp_zb_zcl_report_attr_cmd_t cmd = {
            .zcl_basic_cmd = {
                .dst_addr_u.addr_short = 0x0000, /* coordinator */
                .dst_endpoint = 1,
                .src_endpoint = ep,
            },
            .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .clusterID = ESP_ZB_ZCL_CLUSTER_ID_METERING,
            .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
            .attributeID = ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        };
        esp_zb_zcl_report_attr_cmd_req(&cmd);
    }
}

static void zb_publish(int ch, bool send_report)
{
    esp_zb_lock_acquire(portMAX_DELAY);
    zb_publish_locked(ch, send_report);
    esp_zb_lock_release();
}

static void zb_publish_all(bool send_report)
{
    for (int ch = 0; ch < WATER_CHANNEL_COUNT; ch++) {
        zb_publish(ch, send_report);
    }
}

/* ------------------------------------------------------------------------- */
/* Pulse processing task                                                      */
/* ------------------------------------------------------------------------- */

static void pulse_task(void *pvParameters)
{
    while (true) {
        uint8_t ch;
        if (xQueueReceive(s_pulse_queue, &ch, pdMS_TO_TICKS(1000)) == pdTRUE && ch < WATER_CHANNEL_COUNT) {
            xSemaphoreTake(s_counter_mutex, portMAX_DELAY);
            s_liters[ch] += WATER_LITERS_PER_PULSE;
            s_unsaved_pulses++;
            uint64_t total = s_liters[ch];
            uint32_t unsaved = s_unsaved_pulses;
            xSemaphoreGive(s_counter_mutex);

            ESP_LOGI(TAG, "%s pulse: total %llu L (%llu.%03llu m3)", ch == WATER_CH_COLD ? "Cold" : "Hot", total, total / 1000,
                     total % 1000);

            if (s_zb_ready) {
                zb_publish(ch, true);
            }
            if (unsaved >= CONFIG_WATER_NVS_SAVE_EVERY_PULSES) {
                counters_save();
            }
        }

        /* time based flush, so a slow trickle is never lost for long */
        if (s_unsaved_pulses > 0 && (esp_timer_get_time() - s_last_save_us) >= (int64_t)CONFIG_WATER_NVS_SAVE_PERIOD_S * 1000000LL) {
            counters_save();
        }
    }
}

/* ------------------------------------------------------------------------- */
/* BOOT button: long press = leave the Zigbee network (counters are kept)     */
/* ------------------------------------------------------------------------- */

static void button_task(void *pvParameters)
{
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(WATER_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    /*
     * Hold time is measured against the real clock instead of counting 50 ms loop
     * iterations: a delayed iteration (flash write, Zigbee lock contention) would
     * otherwise let the counter step over the trigger value and never match it.
     */
    int64_t pressed_since_us = 0; /* 0 = button released */
    bool reset_done = false;      /* one-shot guard for the current press */

    while (true) {
        if (gpio_get_level(WATER_BUTTON_GPIO) == 0) {
            int64_t now = esp_timer_get_time();
            if (pressed_since_us == 0) {
                pressed_since_us = now;
            }
            if (!reset_done && (now - pressed_since_us) >= (int64_t)WATER_FACTORY_RESET_HOLD_MS * 1000LL) {
                reset_done = true;
                /*
                 * Gate on the stack main loop being alive, not on commissioning having
                 * succeeded: a device stuck retrying a rejoin is exactly when the user
                 * needs the reset, and taking the Zigbee lock is safe by then.
                 */
                if (s_zb_stack_running) {
                    ESP_LOGW(TAG, "BOOT held %d ms: Zigbee factory reset (water counters are kept)",
                             WATER_FACTORY_RESET_HOLD_MS);
                    counters_save();
                    esp_zb_lock_acquire(portMAX_DELAY);
                    esp_zb_factory_reset();
                    esp_zb_lock_release();
                } else {
                    ESP_LOGW(TAG, "BOOT held, but the Zigbee stack has not started yet: factory reset skipped");
                }
            }
        } else {
            pressed_since_us = 0;
            reset_done = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ------------------------------------------------------------------------- */
/* Zigbee commissioning                                                       */
/* ------------------------------------------------------------------------- */

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , TAG, "Failed to start Zigbee commissioning");
}

/* Consecutive failures of a rejoin attempt after reboot (see the handler below) */
#define WATER_REJOIN_ATTEMPTS_BEFORE_STEERING 5

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    static uint32_t s_rejoin_failures;

    /* any signal proves the stack main loop is running and its lock can be taken */
    s_zb_stack_running = true;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
            /* stack is up: from now on pulses update the attributes; push what was counted so far */
            s_zb_ready = true;
            s_rejoin_failures = 0;
            if (esp_zb_bdb_is_factory_new()) {
                zb_publish_all(false);
                ESP_LOGI(TAG, "Start network steering (enable Permit Join in Zigbee2MQTT)");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted, rejoined the network");
                zb_publish_all(true);
            }
        } else {
            /*
             * Parent unreachable, coordinator down, or the device was removed from the
             * network in Z2M: counting keeps working locally. Retrying INITIALIZATION
             * alone can never recover the last case, so fall back to steering after a
             * few attempts instead of looping on a rejoin that will always fail.
             */
            s_rejoin_failures++;
            if (s_rejoin_failures < WATER_REJOIN_ATTEMPTS_BEFORE_STEERING) {
                ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s), rejoin attempt %lu of %d",
                         esp_err_to_name(err_status), (unsigned long)s_rejoin_failures,
                         WATER_REJOIN_ATTEMPTS_BEFORE_STEERING);
                esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                       ESP_ZB_BDB_MODE_INITIALIZATION, 3000);
            } else {
                ESP_LOGW(TAG, "Rejoin failed %lu times, falling back to network steering",
                         (unsigned long)s_rejoin_failures);
                esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                       ESP_ZB_BDB_MODE_NETWORK_STEERING, 3000);
            }
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d, Short Address: 0x%04hx)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
            /* also covers joining through the rejoin fallback, where no successful
             * DEVICE_REBOOT signal ever arrived to raise the flag */
            s_zb_ready = true;
            s_rejoin_failures = 0;
            zb_publish_all(true);
        } else {
            ESP_LOGI(TAG, "Network steering was not successful (status: %s)", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Endpoint / cluster construction                                            */
/* ------------------------------------------------------------------------- */

static void add_water_meter_endpoint(esp_zb_ep_list_t *ep_list, int ch)
{
    uint8_t ep = WATER_ENDPOINT(ch);
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();

    /* Basic (mandatory) with manufacturer / model, so Zigbee2MQTT can recognise the device */
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)WATER_MANUFACTURER_NAME));
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)WATER_MODEL_IDENTIFIER));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(cluster_list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    /* Identify (mandatory for a valid HA endpoint) */
    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(cluster_list, esp_zb_identify_cluster_create(&identify_cfg),
                                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    /*
     * Metering, built attribute by attribute instead of esp_zb_metering_cluster_create():
     * the stack sizes its reporting table from attributes flagged "reportable" at device
     * registration time, and esp_zb_zcl_update_reporting_info() failed with ESP_ERR_NO_MEM
     * when CurrentSummationDelivered was not flagged.
     * value[m3] = raw[L] * multiplier / divisor, device type = Water.
     */
    esp_zb_attribute_list_t *metering = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_METERING);
    esp_zb_uint48_t summation = to_u48(counter_get(ch));
    uint8_t status = 0;
    uint8_t unit = WATER_UNIT_OF_MEASURE;
    uint8_t formatting = WATER_SUMMATION_FORMAT;
    uint8_t device_type = ESP_ZB_ZCL_METERING_WATER_METERING;
    esp_zb_uint24_t multiplier = {.low = WATER_MULTIPLIER, .high = 0};
    esp_zb_uint24_t divisor = {.low = WATER_DIVISOR, .high = 0};
    const uint8_t ro = ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY;
    const uint16_t mc = ESP_ZB_ZCL_CLUSTER_ID_METERING;

    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering, mc, ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U48, ro | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &summation));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering, mc, ESP_ZB_ZCL_ATTR_METERING_STATUS_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_8BITMAP, ro, &status));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering, mc, ESP_ZB_ZCL_ATTR_METERING_UNIT_OF_MEASURE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM, ro, &unit));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering, mc, ESP_ZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U24, ro, &multiplier));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering, mc, ESP_ZB_ZCL_ATTR_METERING_DIVISOR_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U24, ro, &divisor));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering, mc, ESP_ZB_ZCL_ATTR_METERING_SUMMATION_FORMATTING_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_8BITMAP, ro, &formatting));
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(metering, mc, ESP_ZB_ZCL_ATTR_METERING_METERING_DEVICE_TYPE_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_8BITMAP, ro, &device_type));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_metering_cluster(cluster_list, metering, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    /*
     * Calibration, in a private cluster of its own - receive-only in practice: a freshly
     * installed mechanical meter already shows the volume used to calibrate and test it,
     * and that reading has to reach the real counter without a reflash.
     * Putting this attribute inside the Metering cluster (0x0702) does not work: ZBOSS
     * answers ANY Write Attribute request targeting that cluster with NOT_AUTHORIZED,
     * confirmed on hardware for both the standard CurrentSummationDelivered attribute and
     * a custom one added alongside it - the restriction is per-cluster, not per-attribute,
     * most likely a blanket anti-tamper rule for the whole Smart Energy metering cluster.
     * A cluster ID outside the ZCL-defined range (0xFC00-0xFFFF is reserved for
     * manufacturer-specific clusters) carries no such restriction.
     */
    esp_zb_attribute_list_t *calib = esp_zb_zcl_attr_list_create(WATER_CLUSTER_CALIBRATION_ID);
    s_set_volume_attr[ch] = summation;
    ESP_ERROR_CHECK(esp_zb_cluster_add_attr(calib, WATER_CLUSTER_CALIBRATION_ID, WATER_ATTR_SET_VOLUME_ID,
                                            ESP_ZB_ZCL_ATTR_TYPE_U48, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                            &s_set_volume_attr[ch]));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(cluster_list, calib, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = ep,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_METER_INTERFACE_DEVICE_ID,
        .app_device_version = 0,
    };
    ESP_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_cfg));
}

/* Default reporting configuration: report on change of one pulse, plus a heartbeat */
static void setup_default_reporting(int ch)
{
    esp_zb_zcl_reporting_info_t info = {
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .ep = WATER_ENDPOINT(ch),
        .cluster_id = ESP_ZB_ZCL_CLUSTER_ID_METERING,
        .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .attr_id = ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID,
        .manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
        .dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .u.send_info.min_interval = 1,
        .u.send_info.max_interval = CONFIG_WATER_REPORT_MAX_INTERVAL_S,
        .u.send_info.def_min_interval = 1,
        .u.send_info.def_max_interval = CONFIG_WATER_REPORT_MAX_INTERVAL_S,
        .u.send_info.delta.u48.low = WATER_LITERS_PER_PULSE,
    };
    esp_err_t err = esp_zb_zcl_update_reporting_info(&info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "EP%d: default reporting setup failed: %s", info.ep, esp_err_to_name(err));
    }
}

/*
 * A write of the calibration attribute (WATER_ATTR_SET_VOLUME_ID, not the standard
 * CurrentSummationDelivered - see the comment at its registration above) syncs this
 * counter with the reading printed on the mechanical meter, so a replacement meter
 * that arrives with a non-zero calibration volume can be matched from Home Assistant
 * without reflashing. The stack has already stored the new value in the attribute;
 * mirror it into the counter and persist it right away, so a power cut cannot lose it.
 */
static esp_err_t zb_handle_attr_write(const esp_zb_zcl_set_attr_value_message_t *msg)
{
    ESP_RETURN_ON_FALSE(msg, ESP_ERR_INVALID_ARG, TAG, "empty set-attribute message");
    ESP_RETURN_ON_FALSE(msg->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG,
                        "set-attribute failed, status 0x%x", msg->info.status);

    if (msg->info.cluster != WATER_CLUSTER_CALIBRATION_ID || msg->attribute.id != WATER_ATTR_SET_VOLUME_ID) {
        return ESP_OK;
    }

    int ch = channel_from_endpoint(msg->info.dst_endpoint);
    if (ch < 0 || msg->attribute.data.type != ESP_ZB_ZCL_ATTR_TYPE_U48 || msg->attribute.data.value == NULL) {
        ESP_LOGW(TAG, "EP%d: ignoring calibration write (type 0x%x)", msg->info.dst_endpoint,
                 msg->attribute.data.type);
        return ESP_OK;
    }

    const esp_zb_uint48_t *v = (const esp_zb_uint48_t *)msg->attribute.data.value;
    uint64_t liters = ((uint64_t)v->high << 32) | v->low;

    counter_set(ch, liters);
    ESP_LOGW(TAG, "%s counter set over Zigbee to %llu L (%llu.%03llu m3)", ch == WATER_CH_COLD ? "Cold" : "Hot",
             liters, liters / 1000, liters % 1000);
    counters_save();
    /* already inside a Zigbee callback, so update+report without taking the lock again */
    zb_publish_locked(ch, true);
    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        return zb_handle_attr_write((const esp_zb_zcl_set_attr_value_message_t *)message);
    case ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID:
        break;
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        break;
    }
    return ESP_OK;
}

static void esp_zb_task(void *pvParameters)
{
    /* initialize Zigbee stack as End Device */
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    add_water_meter_endpoint(ep_list, WATER_CH_COLD);
    add_water_meter_endpoint(ep_list, WATER_CH_HOT);
    ESP_ERROR_CHECK(esp_zb_device_register(ep_list));
    for (int ch = 0; ch < WATER_CHANNEL_COUNT; ch++) {
        esp_zb_zcl_attr_t *attr = esp_zb_zcl_get_attribute(WATER_ENDPOINT(ch), ESP_ZB_ZCL_CLUSTER_ID_METERING,
                                                           ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                                           ESP_ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID);
        ESP_LOGI(TAG, "EP%d: CurrentSummationDelivered access flags 0x%02x", WATER_ENDPOINT(ch), attr ? attr->access : 0xFF);
        setup_default_reporting(ch);
    }

    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                                */
/* ------------------------------------------------------------------------- */

void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    /* counters: initial values from Kconfig, overridden by what is stored in NVS */
    s_liters[WATER_CH_COLD] = CONFIG_WATER_INITIAL_COLD_LITERS;
    s_liters[WATER_CH_HOT] = CONFIG_WATER_INITIAL_HOT_LITERS;
    ESP_ERROR_CHECK(nvs_storage_load(s_liters));
    ESP_LOGI(TAG, "Counters restored: cold=%llu L, hot=%llu L", s_liters[WATER_CH_COLD], s_liters[WATER_CH_HOT]);
    s_last_save_us = esp_timer_get_time();

    s_counter_mutex = xSemaphoreCreateMutex();
    s_pulse_queue = xQueueCreate(32, sizeof(uint8_t));
    ESP_ERROR_CHECK((s_counter_mutex && s_pulse_queue) ? ESP_OK : ESP_ERR_NO_MEM);

    /* flush counters on a controlled restart (esp_restart / Zigbee factory reset / OTA) */
    ESP_ERROR_CHECK(esp_register_shutdown_handler(counters_save));

    ESP_ERROR_CHECK(xTaskCreate(pulse_task, "pulse", 4096, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(reed_input_init(s_pulse_queue));
    ESP_ERROR_CHECK(xTaskCreate(button_task, "button", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_FAIL);

    xTaskCreate(esp_zb_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
