// Zigbee2MQTT external definition for the dual water meter (NathanDIY / WaterMeterDual).
//
// Install (Z2M 2.x): copy this file to <z2m data dir>/external_converters/ and restart Zigbee2MQTT.
// The firmware reports CurrentSummationDelivered (seMetering 0x0702) in RAW LITERS on
// endpoint 1 (cold, HVS) and endpoint 2 (hot, GVS); "scale: 1000" turns it into m3.
//
// Writing to seMetering itself is rejected outright: ZBOSS answers ANY Write Attribute
// request targeting that cluster with NOT_AUTHORIZED, confirmed on hardware for both the
// standard CurrentSummationDelivered attribute and a custom one added inside the same
// cluster - the restriction is per-cluster, not per-attribute (an anti-tamper rule baked
// into the Smart Energy metering cluster implementation). The firmware instead exposes a
// private cluster of its own (0xFC00, in the ZCL manufacturer-specific cluster range
// 0xFC00-0xFFFF) with one write-only-in-practice attribute, used only to transfer the
// calibration reading of a freshly installed mechanical meter into the running counter.
import {Zcl} from "zigbee-herdsman";
import * as m from "zigbee-herdsman-converters/lib/modernExtend";

export default {
    zigbeeModel: ["WaterMeterDual"],
    model: "WaterMeterDual",
    vendor: "NathanDIY",
    description: "Dual water meter (cold + hot, reed switch, 10 L/pulse) on M5Stack NanoH2",
    extend: [
        m.deviceEndpoints({endpoints: {cold: 1, hot: 2}}),
        m.deviceAddCustomCluster("waterMeterCalibration", {
            ID: 0xfc00,
            attributes: {
                setVolume: {ID: 0x0000, type: Zcl.DataType.UINT48},
            },
            commands: {},
            commandsResponse: {},
        }),
        m.numeric({
            name: "volume",
            cluster: "seMetering",
            attribute: "currentSummDelivered",
            description: "Total water volume",
            unit: "m³",
            scale: 1000,
            precision: 3,
            access: "STATE_GET",
            endpointNames: ["cold", "hot"],
            reporting: {min: 10, max: 3600, change: 10},
        }),
        m.numeric({
            name: "calibrate_volume",
            cluster: "waterMeterCalibration",
            attribute: "setVolume",
            description: "Set to the reading on a freshly installed meter's dial to sync the counter",
            unit: "m³",
            scale: 1000,
            precision: 3,
            valueMin: 0,
            valueMax: 999999,
            valueStep: 0.001,
            access: "SET",
            endpointNames: ["cold", "hot"],
        }),
    ],
    meta: {multiEndpoint: true},
};
