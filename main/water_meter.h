/*
 * Zigbee dual water meter (cold + hot) for M5Stack NanoH2 (ESP32-H2), End Device.
 * Based on the Espressif HA_on_off_light example.
 */
#pragma once

#include <stdint.h>
#include "sdkconfig.h"
#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_metering.h"

/* Channels / endpoints */
#define WATER_CHANNEL_COUNT     2
#define WATER_CH_COLD           0
#define WATER_CH_HOT            1
#define WATER_ENDPOINT_COLD     1
#define WATER_ENDPOINT_HOT      2
#define WATER_ENDPOINT(ch)      ((ch) == WATER_CH_COLD ? WATER_ENDPOINT_COLD : WATER_ENDPOINT_HOT)

/* Pulse weight and persistence policy (see Kconfig "Water meter") */
#define WATER_LITERS_PER_PULSE  CONFIG_WATER_LITERS_PER_PULSE
#define WATER_DEBOUNCE_US       (CONFIG_WATER_DEBOUNCE_MS * 1000LL)

/*
 * The ZCL attribute CurrentSummationDelivered holds RAW LITERS.
 * Real value = raw * Multiplier / Divisor = raw * 1 / 1000  [m3].
 * SummationFormatting: bit7 = suppress leading zeros, bits6..3 = digits left of the
 * decimal point, bits2..0 = digits right of the decimal point -> 6 left, 3 right.
 */
#define WATER_UNIT_OF_MEASURE   ESP_ZB_ZCL_METERING_UNIT_M3_M3H_BINARY
#define WATER_MULTIPLIER        1
#define WATER_DIVISOR           1000
#define WATER_SUMMATION_FORMAT  ((6 << 3) | 3)

/*
 * Setting the counter to the reading on a freshly installed meter's dial (its calibration
 * volume) is a vendor-specific action, and CurrentSummationDelivered is read-only by spec,
 * so the value is carried by one attribute in a private cluster of its own, using a
 * cluster ID from the manufacturer-specific range (0xFC00-0xFFFF, reserved by the ZCL
 * spec for exactly this) rather than by making a standard metering attribute writable.
 */
#define WATER_CLUSTER_CALIBRATION_ID 0xFC00
#define WATER_ATTR_SET_VOLUME_ID     0x0000

/* Zigbee configuration */
#define INSTALLCODE_POLICY_ENABLE   false
#define ED_AGING_TIMEOUT            ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ED_KEEP_ALIVE               3000 /* ms */
#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

/* Basic cluster identification (ZCL string: first byte is the length) */
#define WATER_MANUFACTURER_NAME "\x09""NathanDIY"
#define WATER_MODEL_IDENTIFIER  "\x0e""WaterMeterDual"

/* NanoH2 BOOT button: hold for WATER_FACTORY_RESET_HOLD_MS to leave the Zigbee network */
#define WATER_BUTTON_GPIO            9
#define WATER_FACTORY_RESET_HOLD_MS  5000

#define ESP_ZB_ZED_CONFIG()                                         \
    {                                                               \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,                       \
        .install_code_policy = INSTALLCODE_POLICY_ENABLE,           \
        .nwk_cfg.zed_cfg = {                                        \
            .ed_timeout = ED_AGING_TIMEOUT,                         \
            .keep_alive = ED_KEEP_ALIVE,                            \
        },                                                          \
    }

#define ESP_ZB_DEFAULT_RADIO_CONFIG()                           \
    {                                                           \
        .radio_mode = ZB_RADIO_MODE_NATIVE,                     \
    }

#define ESP_ZB_DEFAULT_HOST_CONFIG()                            \
    {                                                           \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,   \
    }
