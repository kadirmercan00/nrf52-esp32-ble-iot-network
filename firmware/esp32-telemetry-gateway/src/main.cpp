/**
 * @file main.cpp
 * @brief ESP32 Robust Edge Gateway - Deterministic 10-State FSM with BLE/Wi-Fi Coexistence
 * @author Kadir Mercan (Mercan Studio)
 *
 * ARCHITECTURAL OVERVIEW:
 * - 10-State Finite State Machine (FSM) ensuring deterministic single-threaded execution.
 * - Hardware RF Coexistence: BLE and 2.4 GHz Wi-Fi are strictly mutually exclusive.
 *   Disabling BLE completely before initiating Wi-Fi eliminates RF amplifier saturation
 *   and carrier collisions on single-antenna ESP32 SoC designs.
 * - Packet Integrity: 16-byte packed frame with IEEE 802.3 CRC-32 validation.
 * - Non-Volatile Storage (NVS): Tracks cumulative pulse counts across node brownouts
 *   via dynamic delta calculation with boot counter indexing.
 * - Fault Tolerance: Exponential backoff reconnection, connection timeouts, and duplicate filter.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <time.h>
#include <vector>

// Load configuration with fallback to example header
#if __has_include("config.h")
    #include "config.h"
#else
    #include "config.example.h"
#endif

#include "protocol.h"

// ==================== FINITE STATE MACHINE DEFINITIONS ====================
typedef enum {
    STATE_IDLE,             ///< Initial power-on settling state
    STATE_BLE_INIT,         ///< Initialize NimBLE radio stack & scan parameters
    STATE_BLE_SCAN,         ///< Scan for registered peripheral advertisement UUIDs
    STATE_BLE_CONNECT,      ///< Establish connection with exponential backoff
    STATE_BLE_READ,         ///< Read 16-byte telemetry characteristic & validate CRC-32
    STATE_BLE_DISCONNECT,   ///< Terminate peripheral connection cleanly
    STATE_WIFI_INIT,        ///< Power down BLE stack, initialize Wi-Fi station mode & NTP
    STATE_MQTT_PUBLISH,     ///< Connect to broker and flush telemetry queue
    STATE_WIFI_OFF,         ///< Power down Wi-Fi station to prepare for BLE cycle
    STATE_WAIT              ///< Non-blocking interval/backoff delay state
} GatewayState_t;

static GatewayState_t currentFSMState = STATE_IDLE;
static GatewayState_t nextState       = STATE_IDLE;

// ==================== TELEMETRY DATA QUEUE ====================
typedef struct {
    SensorPacket_t packet;
    String         deviceAddress;
    unsigned long  timestamp;
} QueuedTelemetry_t;

static std::vector<QueuedTelemetry_t> dataQueue;
#define MAX_TELEMETRY_QUEUE_SIZE 16

// ==================== HARDWARE & STACK OBJECTS ====================
static NimBLEScan*   pBLEScan = nullptr;
static NimBLEClient* pClient  = nullptr;
static NimBLEAddress targetAddress;
static bool          targetFound = false;
static uint8_t       connectRetryCount = 0;

static WiFiClient   wifiClient;
static PubSubClient mqttClient(wifiClient);

// FSM Timers
static unsigned long stateStartTime = 0;
static unsigned long lastScanTime   = 0;
static unsigned long waitUntil      = 0;

// ==================== PERSISTENT STORAGE (NVS) ====================
static Preferences preferences;

typedef struct {
    uint32_t baseCount;     ///< Cumulative base count preserved across node resets
    uint32_t lastCount;     ///< Last recorded incremental counter
    uint8_t  lastBootCount; ///< Last known boot count of the peripheral node
} DevicePersistence_t;

static DevicePersistence_t deviceStorage[256];

// ==================== CRC-32 IMPLEMENTATION (IEEE 802.3) ====================
static uint32_t crc32_table[256];
static bool     crc32_initialized = false;

static void initCRC32Table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
        crc32_table[i] = crc;
    }
    crc32_initialized = true;
}

static uint32_t calculateCRC32(const uint8_t *data, size_t length) {
    if (!crc32_initialized) {
        initCRC32Table();
    }

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// ==================== PACKET VALIDATION ====================
static bool validatePacket(const SensorPacket_t *pkt) {
    // Check magic frame delimiter
    if (pkt->magic[0] != PROTOCOL_MAGIC_BYTE_1 || pkt->magic[1] != PROTOCOL_MAGIC_BYTE_2) {
        Serial.println("[SECURITY] Invalid frame magic delimiter rejected");
        return false;
    }

    // Verify 32-bit CRC integrity
    uint32_t calculatedCRC = calculateCRC32((const uint8_t *)pkt, SENSOR_PACKET_SIZE - sizeof(uint32_t));
    if (pkt->crc32 != calculatedCRC) {
        Serial.printf("[SECURITY] CRC-32 Mismatch! Expected: 0x%08X, Received: 0x%08X\n",
                      calculatedCRC, pkt->crc32);
        return false;
    }

    return true;
}

// ==================== TELEMETRY FORMATTER & INGESTION ====================
static String getISOTimestamp(void) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    gmtime_r(&now, &timeinfo);

    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    return String(buffer);
}

static bool publishTelemetry(const SensorPacket_t *pkt) {
    StaticJsonDocument<512> doc;

    uint8_t groupId = pkt->groupId;
    if (groupId < 1 || groupId > NUM_DEVICE_GROUPS) {
        groupId = 1;
    }

    doc["deviceGroupId"] = String("{") + GROUP_UUIDS[groupId] + "}";
    doc["deviceType"]    = GROUP_PREFIXES[groupId];
    doc["nodeId"]        = pkt->deviceId;
    doc["timestamp"]     = getISOTimestamp();

    JsonObject telemetry = doc.createNestedObject("telemetry");
    telemetry["levelPercent"]   = pkt->level;
    telemetry["pulseCount"]     = pkt->count;
    telemetry["currentState"]   = (pkt->stateBits & 0x01) ? "FULL" : "EMPTY";
    telemetry["previousState"]  = ((pkt->stateBits >> 1) & 0x01) ? "FULL" : "EMPTY";

    JsonObject diagnostics = doc.createNestedObject("diagnostics");
    diagnostics["bootCount"]    = pkt->bootCount;
    diagnostics["sequenceId"]   = pkt->seqId;

    char jsonBuffer[512];
    serializeJson(doc, jsonBuffer);

    if (mqttClient.publish(GATEWAY_MQTT_TOPIC, jsonBuffer)) {
        Serial.printf("[MQTT] Ingestion published (%u bytes): %s\n", strlen(jsonBuffer), jsonBuffer);
        return true;
    } else {
        Serial.printf("[MQTT] Publish failed, rc=%d\n", mqttClient.state());
        return false;
    }
}

// ==================== HARDWARE COEXISTENCE: BLE CONTROLLER ====================
static void initBLE(void) {
    Serial.println("[RADIO] Initializing BLE subsystem (Active Scan Mode)...");
    NimBLEDevice::init("ESP32_Edge_Gateway");

    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
}

static void deinitBLE(void) {
    if (pClient && pClient->isConnected()) {
        pClient->disconnect();
    }

    NimBLEDevice::deinit(true);
    pBLEScan = nullptr;
    pClient  = nullptr;

    Serial.println("[RADIO] BLE subsystem de-initialized. Radio isolated.");
    delay(50); // Transient RF decay time
}

static bool scanForPeripherals(void) {
    Serial.println("[BLE] Scanning for registered sensor nodes...");
    targetFound = false;

    pBLEScan->start(GATEWAY_BLE_SCAN_TIME_SEC);
    NimBLEScanResults results = pBLEScan->getResults();

    Serial.printf("[BLE] Scan complete. Found %d advertising devices.\n", results.getCount());

    for (int i = 0; i < results.getCount(); i++) {
        const NimBLEAdvertisedDevice *device = results.getDevice(i);
        if (!device) continue;

        if (device->isAdvertisingService(NimBLEUUID(SENSOR_SERVICE_UUID))) {
            targetAddress = device->getAddress();
            targetFound = true;
            Serial.printf("[BLE] Target node identified: %s [%s]\n",
                          device->getName().c_str(),
                          targetAddress.toString().c_str());
            break;
        }
    }

    pBLEScan->clearResults();
    return targetFound;
}

static bool connectToPeripheral(void) {
    Serial.printf("[BLE] Establishing connection to: %s\n", targetAddress.toString().c_str());

    pClient = NimBLEDevice::createClient();
    if (!pClient->connect(targetAddress, false)) {
        Serial.println("[BLE] Connection establishment failed.");
        NimBLEDevice::deleteClient(pClient);
        pClient = nullptr;
        return false;
    }

    Serial.println("[BLE] Link established successfully.");
    return true;
}

static void disconnectPeripheral(void) {
    if (pClient && pClient->isConnected()) {
        pClient->disconnect();
        Serial.println("[BLE] Peripheral link disconnected.");
    }
    if (pClient) {
        NimBLEDevice::deleteClient(pClient);
        pClient = nullptr;
    }
}

// ==================== NVS STORAGE HELPERS ====================
static void loadNVSStorage(void) {
    preferences.begin("edge_gateway", false);

    for (int i = 0; i < 16; i++) {
        char keyBase[16], keyLast[16];
        snprintf(keyBase, sizeof(keyBase), "base_%d", i);
        snprintf(keyLast, sizeof(keyLast), "last_%d", i);

        deviceStorage[i].baseCount = preferences.getUInt(keyBase, 0);
        deviceStorage[i].lastCount = preferences.getUInt(keyLast, 0);

        if (deviceStorage[i].baseCount > 0 || deviceStorage[i].lastCount > 0) {
            Serial.printf("[NVS] Node ID %d: Restored Base=%u, Last=%u\n",
                          i, deviceStorage[i].baseCount, deviceStorage[i].lastCount);
        }
    }
}

static void saveDevicePersistence(uint8_t devId) {
    if (devId > 250) return;

    char keyBase[16], keyLast[16];
    snprintf(keyBase, sizeof(keyBase), "base_%d", devId);
    snprintf(keyLast, sizeof(keyLast), "last_%d", devId);

    preferences.putUInt(keyBase, deviceStorage[devId].baseCount);
    preferences.putUInt(keyLast, deviceStorage[devId].lastCount);
    Serial.printf("[NVS] Node ID %d: Saved Base=%u, Last=%u\n",
                  devId, deviceStorage[devId].baseCount, deviceStorage[devId].lastCount);
}

// ==================== TELEMETRY INGESTION ENGINE ====================
static bool readAndIngestTelemetry(void) {
    if (!pClient || !pClient->isConnected()) return false;

    NimBLERemoteService *pSvc = pClient->getService(SENSOR_SERVICE_UUID);
    if (!pSvc) {
        Serial.println("[BLE] Service UUID resolution failed.");
        return false;
    }

    NimBLERemoteCharacteristic *pChr = pSvc->getCharacteristic(SENSOR_DATA_CHAR_UUID);
    if (!pChr || !pChr->canRead()) {
        Serial.println("[BLE] Data characteristic unreadable.");
        return false;
    }

    std::string value = pChr->readValue();
    if (value.length() != SENSOR_PACKET_SIZE) {
        Serial.printf("[BLE] Frame length error. Expected %u bytes, got %u\n",
                      SENSOR_PACKET_SIZE, value.length());
        return false;
    }

    SensorPacket_t pkt;
    memcpy(&pkt, value.data(), SENSOR_PACKET_SIZE);

    if (!validatePacket(&pkt)) {
        return false;
    }

    // Sequence deduplication filter
    static uint8_t lastSeqId[256];
    static bool seqInitialized = false;
    if (!seqInitialized) {
        memset(lastSeqId, 0xFF, sizeof(lastSeqId));
        seqInitialized = true;
    }

    uint8_t devId = pkt.deviceId;
    if (lastSeqId[devId] == pkt.seqId) {
        Serial.printf("[FILTER] Duplicate sequence ID %d dropped.\n", pkt.seqId);
        return false;
    }
    lastSeqId[devId] = pkt.seqId;

    // Node brownout & rollover compensation via NVS
    DevicePersistence_t &stored = deviceStorage[devId];
    if (pkt.count < stored.lastCount) {
        Serial.printf("[ANOMALY] Node %d reset detected! (Old Last: %u, New: %u)\n",
                      devId, stored.lastCount, pkt.count);
        stored.baseCount += stored.lastCount;
        saveDevicePersistence(devId);
    }

    if (stored.lastCount != pkt.count) {
        stored.lastCount = pkt.count;
        saveDevicePersistence(devId);
    }

    // Apply cumulative base offset to telemetry
    pkt.count += stored.baseCount;

    // Enqueue for Wi-Fi transmission
    if (dataQueue.size() < MAX_TELEMETRY_QUEUE_SIZE) {
        QueuedTelemetry_t q;
        q.packet        = pkt;
        q.deviceAddress = targetAddress.toString().c_str();
        q.timestamp     = millis();
        dataQueue.push_back(q);
        Serial.printf("[QUEUE] Ingested packet from node %d (Queue depth: %d)\n",
                      devId, dataQueue.size());
        return true;
    } else {
        Serial.println("[QUEUE] Telemetry buffer full! Dropping frame.");
        return false;
    }
}

// ==================== HARDWARE COEXISTENCE: WI-FI & MQTT ====================
static bool initWiFi(void) {
    Serial.println("[RADIO] Initializing Wi-Fi Station Subsystem...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(GATEWAY_WIFI_SSID, GATEWAY_WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < GATEWAY_WIFI_TIMEOUT_MS) {
        delay(250);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] Connected. IP: %s, RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());

        // Synchronize UTC clock via NTP
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        Serial.print("[NTP] Synchronizing UTC wall clock");
        time_t now = 0;
        int retries = 0;
        while (now < 1000000000 && retries++ < 30) {
            delay(100);
            time(&now);
            Serial.print(".");
        }
        Serial.printf(" OK [%s]\n", getISOTimestamp().c_str());

        mqttClient.setServer(GATEWAY_MQTT_SERVER, GATEWAY_MQTT_PORT);
        return true;
    } else {
        Serial.println("\n[WIFI] Station connection timed out.");
        return false;
    }
}

static bool connectMQTT(void) {
    if (mqttClient.connected()) return true;

    Serial.printf("[MQTT] Connecting to broker: %s:%d\n", GATEWAY_MQTT_SERVER, GATEWAY_MQTT_PORT);
    if (mqttClient.connect(GATEWAY_MQTT_CLIENT_ID)) {
        Serial.println("[MQTT] Session established.");
        return true;
    } else {
        Serial.printf("[MQTT] Connection failed, rc=%d\n", mqttClient.state());
        return false;
    }
    return false;
}

static void deinitWiFi(void) {
    if (mqttClient.connected()) {
        mqttClient.disconnect();
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[RADIO] Wi-Fi powered down. Radio returned to idle state.");
    delay(50);
}

// ==================== FSM CORE SCHEDULER ====================
static void executeFSM(void) {
    switch (currentFSMState) {

    case STATE_IDLE:
        Serial.println("\n[FSM] === SYSTEM INITIALIZED: STARTING CYCLE ===");
        currentFSMState = STATE_BLE_INIT;
        break;

    case STATE_BLE_INIT:
        initBLE();
        currentFSMState = STATE_BLE_SCAN;
        stateStartTime  = millis();
        break;

    case STATE_BLE_SCAN:
        if (scanForPeripherals()) {
            connectRetryCount = 0;
            currentFSMState   = STATE_BLE_CONNECT;
        } else {
            lastScanTime    = millis();
            currentFSMState = STATE_WAIT;
            waitUntil       = millis() + GATEWAY_SCAN_INTERVAL_MS;
            nextState       = STATE_BLE_SCAN;
        }
        break;

    case STATE_BLE_CONNECT:
        if (connectToPeripheral()) {
            currentFSMState = STATE_BLE_READ;
        } else {
            connectRetryCount++;
            if (connectRetryCount >= GATEWAY_MAX_CONNECT_RETRIES) {
                Serial.println("[FSM] Connection retry limit reached. Falling back to scan.");
                connectRetryCount = 0;
                currentFSMState   = STATE_BLE_SCAN;
            } else {
                uint32_t backoff = GATEWAY_BASE_BACKOFF_MS * (1 << (connectRetryCount - 1));
                Serial.printf("[FSM] Retrying link (%u/%u) with exponential backoff: %u ms\n",
                              connectRetryCount, GATEWAY_MAX_CONNECT_RETRIES, backoff);
                currentFSMState = STATE_WAIT;
                waitUntil       = millis() + backoff;
                nextState       = STATE_BLE_CONNECT;
            }
        }
        break;

    case STATE_BLE_READ:
        readAndIngestTelemetry();
        currentFSMState = STATE_BLE_DISCONNECT;
        break;

    case STATE_BLE_DISCONNECT:
        disconnectPeripheral();
        if (!dataQueue.empty()) {
            deinitBLE(); // Completely isolate BLE radio before engaging Wi-Fi
            currentFSMState = STATE_WIFI_INIT;
        } else {
            currentFSMState = STATE_BLE_SCAN;
        }
        break;

    case STATE_WIFI_INIT:
        if (initWiFi()) {
            currentFSMState = STATE_MQTT_PUBLISH;
        } else {
            Serial.println("[FSM] Wi-Fi init failed. Purging buffer and returning to BLE.");
            dataQueue.clear();
            deinitWiFi();
            currentFSMState = STATE_BLE_INIT;
        }
        break;

    case STATE_MQTT_PUBLISH:
        if (!mqttClient.connected()) {
            connectMQTT();
        }

        while (!dataQueue.empty() && mqttClient.connected()) {
            QueuedTelemetry_t &telemetry = dataQueue.front();
            publishTelemetry(&telemetry.packet);
            dataQueue.erase(dataQueue.begin());
            mqttClient.loop();
            delay(50); // Egress rate limiting
        }

        currentFSMState = STATE_WIFI_OFF;
        break;

    case STATE_WIFI_OFF:
        deinitWiFi();
        currentFSMState = STATE_BLE_INIT;
        break;

    case STATE_WAIT:
        if (millis() >= waitUntil) {
            currentFSMState = nextState;
        }
        break;
    }
}

// ==================== SYSTEM ENTRY POINTS ====================
void setup(void) {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n=======================================================");
    Serial.println("  ESP32 DETERMINISTIC EDGE GATEWAY - INDUSTRIAL FSM    ");
    Serial.println("=======================================================");
    Serial.println("  Architecture : Time-Multiplexed BLE / Wi-Fi Coexistence");
    Serial.println("  Integrity    : IEEE 802.3 CRC-32 Frame Verification   ");
    Serial.println("  Storage      : Non-Volatile Hardware Pulse Accounting ");
    Serial.println("  Author       : Kadir Mercan (Mercan Studio)           ");
    Serial.println("=======================================================\n");

    initCRC32Table();
    loadNVSStorage();

    currentFSMState = STATE_IDLE;
}

void loop(void) {
    executeFSM();

    if (mqttClient.connected()) {
        mqttClient.loop();
    }

    delay(10); // Yield to FreeRTOS watchdog & idle task
}
