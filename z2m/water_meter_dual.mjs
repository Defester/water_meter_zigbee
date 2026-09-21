// Zigbee2MQTT external definition for the dual water meter (NathanDIY / WaterMeterDual).
//
// Install (Z2M 2.x): copy this file to <z2m data dir>/external_converters/ and restart Zigbee2MQTT.
// The firmware reports CurrentSummationDelivered (seMetering 0x0702) in RAW LITERS on
// endpoint 1 (cold, HVS) and endpoint 2 (hot, GVS); "scale: 1000" turns it into m3.
//
// CurrentSummationDelivered is read-only, as the ZCL spec defines it. Setting the counter
// to the reading of a freshly installed mechanical meter (its calibration volume) is a
// vendor-specific action, so the firmware exposes it through a private cluster of its own
// (0xFC00, in the ZCL manufacturer-specific cluster range 0xFC00-0xFFFF) holding a single
// write-only-in-practice attribute, rather than by making a standard, spec-read-only
// metering attribute writable.
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
                // write: true is mandatory - without it zigbee-herdsman refuses the write
                // locally, before anything is transmitted, with
                // "Status 'NOT_AUTHORIZED' <name> (<id>) is not writable"
                // (see processAttributeWrite() in its src/zspec/zcl/utils.ts).
                setVolume: {ID: 0x0000, type: Zcl.DataType.UINT48, write: true},
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
            // No valueMin/valueMax on purpose: declaring both makes the Z2M frontend render
            // a slider instead of an input box, and a slider spanning a million steps both
            // fires a write per intermediate value while dragging and, on release, sent a
            // payload the backend rejected with
            // "'calibrate_volume' is not a number, got object (undefined)".
            // A meter reading is typed in, not dragged to.
            //
            // STATE_SET rather than a write-only SET: a settable field that also carries a
            // state is the ordinary, well-trodden path in Z2M, and the attribute is
            // readable on the device anyway.
            access: "STATE_SET",
            endpointNames: ["cold", "hot"],
        }),
    ],
    meta: {multiEndpoint: true},
};
