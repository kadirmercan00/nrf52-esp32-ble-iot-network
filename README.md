<div align="center">

# 🌐 Industrial BLE Wireless Sensor Network & Edge Gateway

**Hybrid Multi-Platform WSN: Nordic nRF52840 (<15 µA Coin-Cell Nodes) & Espressif ESP32 (10-State Deterministic MQTT Gateway)**

[![Nordic nRF52](https://img.shields.io/badge/Platform-Nordic_nRF52840-00A9CE?logo=nordicsemiconductor&logoColor=white)](https://www.nordicsemi.com/)
[![Espressif ESP32](https://img.shields.io/badge/Platform-Espressif_ESP32-E7352C?logo=espressif&logoColor=white)](https://www.espressif.com/)
[![Embedded C++](https://img.shields.io/badge/Language-C%2B%2B17-00599C?logo=c%2B%2B&logoColor=white)](https://isocpp.org/)
[![BLE Protocol](https://img.shields.io/badge/Protocol-BLE_5.0_%7C_16--Byte_CRC--32-brightgreen)](protocol/packet_schema.h)
[![License: Proprietary Showcase](https://img.shields.io/badge/License-Proprietary_Showcase-red.svg)](LICENSE)

[System Architecture](docs/ARCHITECTURE.md) • [Binary Protocol Spec](protocol/packet_schema.h) • [nRF52 Node Firmware](firmware/nrf52840-sensor-node/) • [ESP32 Gateway Firmware](firmware/esp32-telemetry-gateway/) • [Python Sniffer](tools/)

</div>

---

## 📌 Architectural Overview

Industrial telemetry systems demand an asymmetric approach: **battery-operated field nodes** must maximize field longevity over multiple years on tiny power cells, while **central edge gateways** require deterministic reliability, robust packet verification, and high-throughput cloud connectivity.

This repository implements a production-grade dual-platform Wireless Sensor Network:
1. **Ultra-Low-Power Field Nodes (Nordic nRF52840):** Runs an event-driven state machine achieving **$<15\,\mu\text{A}$** average deep sleep current. Broadcasts 16-byte packed telemetry bursts over BLE 5.0 non-connectable advertising channels.
2. **Deterministic Edge Gateway (Espressif ESP32):** Ingests asynchronous BLE bursts, validates frames via hardware-grade **IEEE 802.3 CRC-32**, protects local actuation counts using **Non-Volatile Flash (NVS)**, and dispatches JSON telemetry to AWS IoT Core / MQTT brokers.
3. **Hardware RF Coexistence:** Firmware-level Time-Division Multiplexing (TDM) guarantees that 2.4 GHz Wi-Fi transmission bursts and NimBLE receiver windows never contend for the internal radio, preventing RF front-end desensitization.

---

## 🏗️ System Topology & Data Flow

```mermaid
graph LR
    subgraph Nodes ["Ultra-Low-Power Nodes (Nordic nRF52840)"]
        N1[CR2032 Coin Cell<br/>Iq < 15 µA] -->|BLE 5.0 Non-Connectable Adv<br/>16-Byte Packed Frame| GW
        N2[Industrial Hall Sensor<br/>Reed Switch Actuator] -->|BLE 5.0 Non-Connectable Adv<br/>16-Byte Packed Frame| GW
    end

    subgraph Gateway ["Industrial Edge Gateway (ESP32)"]
        GW[10-State Deterministic FSM]
        GW -->|1. CRC-32 Checksum Verification| GW
        GW -->|2. NVS Flash Event Persistence| GW
        GW -->|3. Wi-Fi / BLE RF Coexistence TDM| GW
    end

    subgraph Cloud ["Enterprise Ingestion"]
        GW -->|MQTTS / TLS 1.2 (Port 8883)| BROKER[AWS IoT Core / MQTT Broker]
        BROKER --> DASH[Real-Time Monitoring Portal]
    end
```

---

## 📡 16-Byte Compact Binary Protocol

To minimize RF airtime and maximize coin-cell longevity, telemetry is serialized into an atomic **16-byte packed C struct**:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Magic 1 (0xA5)   |   Magic 2 (0x5A)   |    Device ID (1-255)  |   Group ID (1-16) |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Boot Counter     |   Sequence ID      |   State Flags (Bits)  |   Level (0-100%)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  Actuation / Pulse Counter (32-bit uint)                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  Standard IEEE 802.3 CRC-32 Checksum                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

* **Frame Synchronization:** Delimited by fixed magic bytes `0xA5 0x5A`.
* **State Flags:** Encodes present state, prior state, and low-battery warning flags.
* **Integrity:** The trailing 4-byte CRC-32 guarantees corrupt RF frames are dropped before cloud dispatch.

---

## 📁 Repository Structure

```
nrf52-esp32-ble-iot-network/
├── firmware/
│   ├── nrf52840-sensor-node/        # Nordic nRF52840 firmware (<15µA deep sleep)
│   │   ├── src/main.cpp
│   │   ├── include/node_config.h
│   │   └── platformio.ini
│   │
│   └── esp32-telemetry-gateway/     # ESP32 10-State FSM edge gateway
│       ├── src/main.cpp
│       ├── include/config.example.h
│       └── platformio.ini
│
├── protocol/
│   └── packet_schema.h              # Single source of truth binary frame definition
│
├── tools/
│   └── ble_packet_sniffer.py        # Python Bleak sniffer & CRC-32 validator
│
├── docs/
│   └── ARCHITECTURE.md              # Deep-dive RF coexistence & power profiles
│
├── .github/workflows/
│   └── firmware-build.yml           # Automated multi-target compile CI
│
├── LICENSE                          # MIT License
└── README.md
```

---

## ⚙️ Compilation & Quick Start

### 1. Build nRF52840 Node Firmware
```bash
cd firmware/nrf52840-sensor-node
pio run
```

### 2. Build ESP32 Gateway Firmware
```bash
cd firmware/esp32-telemetry-gateway
pio run
```

### 3. Run Python Packet Sniffer & Validator
```bash
python tools/ble_packet_sniffer.py
```

---

## 📄 License & Terms of Evaluation

This project is licensed under the **Proprietary Source-Available Showcase License** - see the [LICENSE](LICENSE) file for full details.  
*Evaluation, code audit, and engineering review are permitted; unauthorized commercial deployment, compilation into industrial products, and reproduction are strictly prohibited.*

## 👨‍💻 Engineering Attribution

Engineered by **[Kadir Mercan](https://www.linkedin.com/in/kadir-mercan-/)**  
*Senior Embedded Systems, Hardware & IoT Engineer*
