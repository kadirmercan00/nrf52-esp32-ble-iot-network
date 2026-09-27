/**
 * @file node_config.h
 * @brief Hardware Pinout & Operating Parameters for nRF52840 Sensor Node
 * @author Kadir Mercan (Mercan Studio)
 */

#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

// ==================== NODE IDENTIFICATION ====================
#define CONFIG_NODE_ID              1
#define CONFIG_GROUP_ID             1
#define CONFIG_DEVICE_NAME          "SENSOR_NODE_001"

// ==================== GPIO HARDWARE PINOUT ====================
// Based on Seeed XIAO nRF52840 pin assignments
#define PIN_ENCODER_INPUT           2   ///< Pulse interrupt pin (Active Low, internal pullup)
#define PIN_LID_SWITCH              3   ///< Reed switch / microswitch for service door

// ==================== CALIBRATION & THRESHOLDS ====================
#define MAX_RESERVOIR_TURNS         800     ///< Total pulses representing 100% capacity
#define LID_OPEN_THRESHOLD_MS       5000    ///< Time lid must stay open to acknowledge refilling
#define TRANSMIT_PULSE_INTERVAL     10      ///< Transmit frame every 10 pulses or state change
#define FLASH_PERSIST_INTERVAL      100     ///< Save counter to LittleFS every 100 pulses (Wear leveling)

// ==================== BLE ADVERTISING PARAMETERS ====================
#define BLE_ADV_FAST_TIMEOUT_SEC    30      ///< Fast advertising interval on wake
#define BLE_ADV_FAST_INTERVAL_MS    100     ///< Fast interval: 100 ms (62.5 us units = 160)
#define BLE_ADV_SLOW_INTERVAL_MS    1000    ///< Low-power interval: 1000 ms

#endif // NODE_CONFIG_H
