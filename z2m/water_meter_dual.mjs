// Zigbee2MQTT external definition for the dual water meter (NathanDIY / WaterMeterDual).
//
// Install (Z2M 2.x): copy this file to <z2m data dir>/external_converters/ and restart Zigbee2MQTT.
// The firmware reports CurrentSummationDelivered (seMetering 0x0702) in RAW LITERS on
// endpoint 1 (cold, HVS) and endpoint 2 (hot, GVS); "scale: 1000" turns it into m3.
//
// CurrentSummationDelivered itself cannot be written: ZBOSS answers a Write Attribute
// request for it with NOT_AUTHORIZED regardless of the device's declared access flags
// (an anti-tamper rule baked into the Smart Energy metering cluster implementation).
// The firmware instead exposes a second, non-standard attribute (0xF000, in the ZCL8
// manufacturer-extension range 0xF000-0xFFFE) on the same cluster, write-only in
// practice, used only to transfer the calibration reading of a freshly installed
// mechanical meter into the running counter.
import {Zcl} from "zigbee-herdsman";
import * as m from "zigbee-herdsman-converters/lib/modernExtend";

export default {
    zigbeeModel: ["WaterMeterDual"],
    model: "WaterMeterDual",
    vendor: "NathanDIY",
    description: "Dual water meter (cold + hot, reed switch, 10 L/pulse) on M5Stack NanoH2",
    extend: [
        m.deviceEndpoints({endpoints: {cold: 1, hot: 2}}),
        m.deviceAddCustomCluster("seMeteringCalibration", {
            ID: Zcl.Clusters.seMetering.ID,
            attributes: {
                setVolume: {ID: 0xf000, type: Zcl.DataType.UINT48},
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
            cluster: "seMeteringCalibration",
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
