// Zigbee2MQTT external definition for the dual water meter (NathanDIY / WaterMeterDual).
//
// Install (Z2M 2.x): copy this file to <z2m data dir>/external_converters/ and restart Zigbee2MQTT.
// The firmware reports CurrentSummationDelivered (seMetering 0x0702) in RAW LITERS on
// endpoint 1 (cold, HVS) and endpoint 2 (hot, GVS); "scale: 1000" turns it into m3.
import * as m from "zigbee-herdsman-converters/lib/modernExtend";

export default {
    zigbeeModel: ["WaterMeterDual"],
    model: "WaterMeterDual",
    vendor: "NathanDIY",
    description: "Dual water meter (cold + hot, reed switch, 10 L/pulse) on M5Stack NanoH2",
    extend: [
        m.deviceEndpoints({endpoints: {cold: 1, hot: 2}}),
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
    ],
    meta: {multiEndpoint: true},
};
