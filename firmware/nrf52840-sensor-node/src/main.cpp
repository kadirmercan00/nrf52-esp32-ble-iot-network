/**
 * @file main.cpp
 * @brief Ultra-Low Power BLE Sensor Node Firmware for Nordic nRF52840 SoC
 * @author Kadir Mercan (Mercan Studio)
 *
 * ARCHITECTURAL SPECIFICATION:
 * - Target Platform: Nordic Semiconductor nRF52840 (ARM Cortex-M4F @ 64 MHz)
 * - BLE Stack: Adafruit Bluefruit52Lib (Nordic SoftDevice S140 v6.1.1)
 * - File System: Internal LittleFS (Wear-leveling persistent pulse storage)
 * - Power Profile: Event-driven execution utilizing FreeRTOS sleep hooks (<18 uA quiescent)
 * - Telemetry Frame: 16-Byte packed binary layout with IEEE 802.3 CRC-32 integrity
 * - Refill Detection: Asynchronous lid microswitch timer with debounced state transition
 */

#include <Arduino.h>
#include <bluefruit.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

#include "node_config.h"
#include "protocol.h"

using namespace Adafruit_LittleFS_Namespace;

// ==================== PERSISTENT STORAGE (LittleFS) ====================
#define FLASH_MAGIC_KEY     0xBEEF
#define FLASH_STORE_PATH    "/sensor_store.dat"

typedef struct __attribute__((packed)) {
    uint16_t magic;         ///< Validation key (0xBEEF)
    uint8_t  deviceId;      ///< Persistent node identifier
    uint8_t  bootCount;     ///< Cumulative boot sequence counter
    uint32_t savedCount;    ///< Persisted pulse/actuation count
    uint8_t  lastState;     ///< Last recorded capacity state
    uint8_t  reserved[3];   ///< Future word alignment padding
} FlashRecord_t;

static FlashRecord_t flashRecord;
static File          flashFile(InternalFS);

// ==================== BLE SERVICES & CHARACTERISTICS ====================
static BLEService        telemetryService(SENSOR_SERVICE_UUID);
static BLECharacteristic telemetryCharacteristic(SENSOR_DATA_CHAR_UUID);

// ==================== OPERATIONAL RUNTIME STATE ====================
static volatile uint32_t isrEncoderCount  = 0;
static volatile bool     isrPulseOccurred = false;
static uint32_t          lastTransmittedCount = 0;

static bool     lidPrevOpenState  = false;
static uint32_t lidOpenTimestamp  = 0;

static NodeState_t currentNodeState = NODE_STATE_FULL;
static NodeState_t prevNodeState    = NODE_STATE_FULL;

static bool isBleConnected = false;
static uint8_t sequenceId  = 0;
static uint8_t levelRatio  = 100;

// ==================== IEEE 802.3 CRC-32 TABLE ====================
static uint32_t crc32_lookup[256];
static bool     crc32_ready = false;

static void initCRC32Table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : crc >> 1;
        }
        crc32_lookup[i] = crc;
    }
    crc32_ready = true;
}

static uint32_t calculateCRC32(const uint8_t *buffer, size_t length) {
    if (!crc32_ready) {
        initCRC32Table();
    }

    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc = crc32_lookup[(crc ^ buffer[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// ==================== FLASH STORAGE CONTROLLER ====================
static void persistToFlash(void) {
    flashRecord.savedCount = isrEncoderCount;
    flashRecord.lastState  = (uint8_t)currentNodeState;

    flashFile.open(FLASH_STORE_PATH, FILE_O_WRITE);
    if (flashFile) {
        flashFile.seek(0);
        flashFile.write((uint8_t *)&flashRecord, sizeof(FlashRecord_t));
        flashFile.close();
        Serial.printf("[NVM] Saved count: %u (Boot #%u)\n", 
                      flashRecord.savedCount, flashRecord.bootCount);
    } else {
        Serial.println("[NVM] ERROR: Flash sector write fault!");
    }
}

static void loadFromFlash(void) {
    InternalFS.begin();

    if (InternalFS.exists(FLASH_STORE_PATH)) {
        flashFile.open(FLASH_STORE_PATH, FILE_O_READ);
        if (flashFile) {
            flashFile.read(&flashRecord, sizeof(FlashRecord_t));
            flashFile.close();

            if (flashRecord.magic == FLASH_MAGIC_KEY) {
                isrEncoderCount   = flashRecord.savedCount;
                prevNodeState     = (NodeState_t)flashRecord.lastState;
                currentNodeState  = prevNodeState;
                flashRecord.bootCount++;

                Serial.println("[NVM] Cold-boot record recovered:");
                Serial.printf("      Boot Sequence: %u\n", flashRecord.bootCount);
                Serial.printf("      Restored Count: %u\n", flashRecord.savedCount);
                Serial.printf("      State: %s\n", (currentNodeState == NODE_STATE_FULL) ? "FULL" : "EMPTY");

                lastTransmittedCount = isrEncoderCount;
                persistToFlash();
                return;
            }
        }
    }

    Serial.println("[NVM] Initial factory provisioning: Writing defaults.");
    flashRecord.magic      = FLASH_MAGIC_KEY;
    flashRecord.deviceId   = CONFIG_NODE_ID;
    flashRecord.bootCount  = 1;
    flashRecord.savedCount = 0;
    flashRecord.lastState  = (uint8_t)NODE_STATE_FULL;
    persistToFlash();
}

// ==================== INTERRUPT SERVICE ROUTINES ====================
void handleEncoderPulseISR(void) {
    static uint32_t lastPulseTimeMs = 0;
    uint32_t nowMs = millis();

    // 50ms hardware switch bounce suppression window
    if (nowMs - lastPulseTimeMs > 50) {
        isrEncoderCount++;
        isrPulseOccurred = true;
        lastPulseTimeMs  = nowMs;
    }
}

// ==================== TELEMETRY BUILDER ====================
static void assembleAndQueueFrame(void) {
    SensorPacket_t frame;
    memset(&frame, 0, sizeof(SensorPacket_t));

    frame.magic[0]    = PROTOCOL_MAGIC_BYTE_1;
    frame.magic[1]    = PROTOCOL_MAGIC_BYTE_2;
    frame.deviceId    = CONFIG_NODE_ID;
    frame.groupId     = CONFIG_GROUP_ID;
    frame.bootCount   = flashRecord.bootCount;
    frame.seqId       = sequenceId++;
    frame.stateBits   = ((uint8_t)prevNodeState << 1) | ((uint8_t)currentNodeState & 0x01);
    frame.level       = levelRatio;
    frame.count       = isrEncoderCount;

    // Calculate CRC-32 over the preceding 12 bytes
    frame.crc32       = calculateCRC32((const uint8_t *)&frame, SENSOR_PACKET_SIZE - sizeof(uint32_t));

    // Update BLE characteristic value
    telemetryCharacteristic.write((const uint8_t *)&frame, SENSOR_PACKET_SIZE);

    Serial.printf("[TX] Frame #%u queued: Count=%u, Level=%u%%, CRC=0x%08X\n",
                  frame.seqId, frame.count, frame.level, frame.crc32);
}

// ==================== BLE CALLBACKS & ADVERTISING ====================
static void bleConnectCallback(uint16_t connHandle) {
    isBleConnected = true;
    BLEConnection* connection = Bluefruit.Connection(connHandle);
    char centralName[32] = { 0 };
    connection->getPeerName(centralName, sizeof(centralName));

    Serial.printf("[BLE] Link established with central: %s (Handle: %d)\n", centralName, connHandle);
}

static void bleDisconnectCallback(uint16_t connHandle, uint8_t reason) {
    (void)connHandle;
    (void)reason;
    isBleConnected = false;
    Serial.println("[BLE] Link disconnected. Returning to advertising mode.");
}

static void configureBleAdvertising(void) {
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(telemetryService);
    Bluefruit.ScanResponse.addName();

    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(BLE_ADV_FAST_INTERVAL_MS * 16 / 10, BLE_ADV_SLOW_INTERVAL_MS * 16 / 10);
    Bluefruit.Advertising.setFastTimeout(BLE_ADV_FAST_TIMEOUT_SEC);
    Bluefruit.Advertising.start(0); // Unlimited advertising
}

// ==================== SYSTEM ENTRY POINTS ====================
void setup(void) {
    Serial.begin(115200);

    Serial.println("\n=======================================================");
    Serial.println("  nRF52840 ULTRA-LOW-POWER TELEMETRY SENSOR NODE      ");
    Serial.println("=======================================================");
    Serial.println("  SoC         : Nordic Semiconductor nRF52840 (Cortex-M4F) ");
    Serial.println("  Storage     : Wear-Leveled LittleFS Internal Flash  ");
    Serial.println("  Protocol    : 16-Byte Packed Frame with IEEE 802.3 CRC ");
    Serial.println("  Author      : Kadir Mercan (Mercan Studio)          ");
    Serial.println("=======================================================\n");

    // Initialize CRC table
    initCRC32Table();

    // Mount and read LittleFS persistent records
    loadFromFlash();

    // Pin mode configurations
    pinMode(PIN_ENCODER_INPUT, INPUT_PULLUP);
    pinMode(PIN_LID_SWITCH, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_INPUT), handleEncoderPulseISR, FALLING);

    // Initialize Nordic Bluefruit SoftDevice
    Bluefruit.begin();
    Bluefruit.setTxPower(4); // +4 dBm for long range
    Bluefruit.setName(CONFIG_DEVICE_NAME);
    Bluefruit.Periph.setConnectCallback(bleConnectCallback);
    Bluefruit.Periph.setDisconnectCallback(bleDisconnectCallback);

    // Initialize Telemetry GATT Service
    telemetryService.begin();
    telemetryCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
    telemetryCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
    telemetryCharacteristic.setFixedLen(SENSOR_PACKET_SIZE);
    telemetryCharacteristic.begin();

    // Initial frame assembly
    assembleAndQueueFrame();

    // Start RF advertising
    configureBleAdvertising();
    Serial.println("[SYSTEM] Hardware initialization complete. Entering low-power scheduler.");
}

void loop(void) {
    // 1. Process Encoder Pulse State
    if (isrPulseOccurred) {
        isrPulseOccurred = false;

        // Calculate reservoir percentage based on max turn budget
        if (isrEncoderCount < MAX_RESERVOIR_TURNS) {
            levelRatio = (uint8_t)(((MAX_RESERVOIR_TURNS - isrEncoderCount) * 100) / MAX_RESERVOIR_TURNS);
        } else {
            levelRatio = 0;
            if (currentNodeState == NODE_STATE_FULL) {
                prevNodeState    = currentNodeState;
                currentNodeState = NODE_STATE_EMPTY;
                Serial.println("[STATE] Reservoir depleted: STATE_EMPTY transition triggered.");
            }
        }

        // Periodic frame update
        if ((isrEncoderCount - lastTransmittedCount) >= TRANSMIT_PULSE_INTERVAL) {
            lastTransmittedCount = isrEncoderCount;
            assembleAndQueueFrame();
        }

        // Periodic wear-leveling flash sync
        if (isrEncoderCount % FLASH_PERSIST_INTERVAL == 0) {
            persistToFlash();
        }
    }

    // 2. Process Service Door / Lid Microswitch
    bool lidCurrentlyOpen = (digitalRead(PIN_LID_SWITCH) == LOW);
    if (lidCurrentlyOpen && !lidPrevOpenState) {
        lidOpenTimestamp = millis();
        lidPrevOpenState = true;
        Serial.println("[LID] Service door opened.");
    } else if (!lidCurrentlyOpen && lidPrevOpenState) {
        uint32_t openDurationMs = millis() - lidOpenTimestamp;
        lidPrevOpenState = false;
        Serial.printf("[LID] Service door closed after %u ms.\n", openDurationMs);

        // Acknowledge refill if lid was open longer than minimum service threshold
        if (openDurationMs >= LID_OPEN_THRESHOLD_MS) {
            Serial.println("[STATE] Facility Refill Confirmed! Resetting operational reservoir.");
            isrEncoderCount      = 0;
            lastTransmittedCount = 0;
            levelRatio           = 100;
            prevNodeState        = currentNodeState;
            currentNodeState     = NODE_STATE_FULL;

            persistToFlash();
            assembleAndQueueFrame();
        }
    }

    // Yield CPU to FreeRTOS low-power tickless idle hook
    delay(20);
}
