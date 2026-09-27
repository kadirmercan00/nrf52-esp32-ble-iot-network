/**
 * @file config.example.h
 * @brief Gateway Configuration Template (Copy to config.h for local compilation)
 * @author Kadir Mercan (Mercan Studio)
 */

#ifndef CONFIG_H
#define CONFIG_H

// ==================== WI-FI SETTINGS ====================
#define GATEWAY_WIFI_SSID           "YOUR_FACILITY_WIFI_SSID"
#define GATEWAY_WIFI_PASSWORD       "YOUR_FACILITY_WIFI_PASS"
#define GATEWAY_WIFI_TIMEOUT_MS     10000

// ==================== MQTT BROKER SETTINGS ====================
#define GATEWAY_MQTT_SERVER         "mqtt.industrial-iot.internal"
#define GATEWAY_MQTT_PORT           1883
#define GATEWAY_MQTT_TOPIC          "telemetry/industrial/dispensers"
#define GATEWAY_MQTT_CLIENT_ID      "ESP32_Edge_Gateway_01"

// ==================== BLE SCAN & CONN SETTINGS ====================
#define GATEWAY_BLE_SCAN_TIME_SEC   5
#define GATEWAY_CONNECT_TIMEOUT_MS  5000
#define GATEWAY_MAX_CONNECT_RETRIES 3
#define GATEWAY_BASE_BACKOFF_MS     1000
#define GATEWAY_SCAN_INTERVAL_MS    10000

// ==================== DEVICE GROUP METADATA ====================
#define NUM_DEVICE_GROUPS 4

static const char *GROUP_UUIDS[] = {
    "",
    "872337DB-495D-4773-AD7C-41689A8B3D14", // Group 1: Liquid Sanitizer
    "6EF11313-12AB-491A-8EBC-9057FD1CA48C", // Group 2: Industrial Paper Rolls
    "175AFB38-8D2A-43E7-A060-9671C9B39FF9", // Group 3: Cotton Towel Rolls
    "A9B3442A-38FC-4BDC-AEFA-522E84E851CB"  // Group 4: Waste Level Receptacles
};

static const char *GROUP_PREFIXES[] = {
    "",
    "Dispenser_Sanitizer",
    "Dispenser_Paper",
    "Dispenser_Towel",
    "Dispenser_Waste"
};

#endif // CONFIG_H
