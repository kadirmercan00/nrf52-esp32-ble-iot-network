/**
 * @file protocol.h
 * @brief Unified Industrial BLE Telemetry Binary Protocol Schema
 * @details Shared binary frame format for Nordic nRF52 nodes, ESP32 gateways, and Python sniffers.
 * @author Kadir Mercan (Mercan Studio)
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTOCOL_MAGIC_BYTE_1   0xA5
#define PROTOCOL_MAGIC_BYTE_2   0x5A
#define SENSOR_PACKET_SIZE      16

// 128-bit BLE Custom Service & Characteristic UUIDs
#define SENSOR_SERVICE_UUID     "12345678-1234-1234-1234-1234567890ab"
#define SENSOR_DATA_CHAR_UUID   "12345678-1234-1234-1234-1234567890ac"

typedef enum {
    NODE_STATE_EMPTY   = 0x00,
    NODE_STATE_FULL    = 0x01,
    NODE_STATE_WARNING = 0x02,
    NODE_STATE_FAULT   = 0x03
} NodeOperationalState_t;

/**
 * @brief 16-Byte Compact Sensor Payload (IEEE 802.3 CRC-32 protected)
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic[2];    ///< Frame delimiter: 0xA5, 0x5A
    uint8_t  deviceId;    ///< Node ID (1-255)
    uint8_t  groupId;     ///< Sector/Facility Group (1-16)
    uint8_t  bootCount;   ///< Incremental brownout/boot counter
    uint8_t  seqId;       ///< Rolling sequence counter (0-255)
    uint8_t  stateBits;   ///< bit0-1: current state, bit2-3: prev state, bit4: low battery
    uint8_t  level;       ///< Measured level/percentage (0-100%)
    uint32_t count;       ///< Total actuation / event count (monotonically increasing)
    uint32_t crc32;       ///< Standard IEEE 802.3 CRC-32 checksum
} SensorPacket_t;

#ifdef __cplusplus
}
#endif

#endif // PROTOCOL_H
