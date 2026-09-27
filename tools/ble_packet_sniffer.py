#!/usr/bin/env python3
"""
Industrial BLE Telemetry Sniffer & Frame Validator
Scans for custom 16-byte broadcast packets, validates IEEE 802.3 CRC-32,
and outputs decoded telemetry in real time.
"""

import sys
import struct
import zlib
import argparse
import asyncio
from datetime import datetime

try:
    from bleak import BleakScanner
except ImportError:
    BleakScanner = None

MAGIC_HEADER = bytes([0xA5, 0x5A])

def calculate_crc32(payload_without_crc: bytes) -> int:
    """Calculates IEEE 802.3 CRC-32 checksum."""
    return zlib.crc32(payload_without_crc) & 0xFFFFFFFF

def decode_sensor_packet(raw_bytes: bytes):
    """
    Parses packed 16-byte struct:
    uint8_t magic[2], deviceId, groupId, bootCount, seqId, stateBits, level
    uint32_t count, uint32_t crc32
    """
    if len(raw_bytes) != 16:
        return None

    if raw_bytes[:2] != MAGIC_HEADER:
        return None

    magic1, magic2, dev_id, grp_id, boot_cnt, seq_id, state_bits, level, count, received_crc = struct.unpack(
        "<2BBBBBBBII", raw_bytes
    )

    expected_crc = calculate_crc32(raw_bytes[:12])
    crc_valid = (received_crc == expected_crc)

    curr_state = state_bits & 0x03
    prev_state = (state_bits >> 2) & 0x03
    low_bat = bool(state_bits & 0x10)

    return {
        "timestamp": datetime.utcnow().isoformat() + "Z",
        "device_id": dev_id,
        "group_id": grp_id,
        "boot_count": boot_cnt,
        "seq_id": seq_id,
        "current_state": "FULL" if curr_state == 1 else "EMPTY",
        "previous_state": "FULL" if prev_state == 1 else "EMPTY",
        "low_battery": low_bat,
        "level_pct": level,
        "actuation_count": count,
        "crc_valid": crc_valid
    }

async def scan_ble():
    if BleakScanner is None:
        print("[!] 'bleak' library not installed. Simulating frame reception...")
        mock_packets()
        return

    print("[*] Starting BLE Scanner for Custom Sensor Frames...")

    def callback(device, advertising_data):
        for mfg_id, data in advertising_data.manufacturer_data.items():
            parsed = decode_sensor_packet(data)
            if parsed:
                status = "VALID" if parsed["crc_valid"] else "CORRUPTED"
                print(f"[{status}] Dev #{parsed['device_id']:02d} | Group {parsed['group_id']} | "
                      f"Level: {parsed['level_pct']}% | Count: {parsed['actuation_count']} | RSSI: {advertising_data.rssi} dBm")

    scanner = BleakScanner(detection_callback=callback)
    await scanner.start()
    try:
        while True:
            await asyncio.sleep(1)
    except KeyboardInterrupt:
        await scanner.stop()
        print("\n[*] Sniffer stopped.")

def mock_packets():
    """Generates synthetic valid and corrupted test frames."""
    for i in range(5):
        # Build 12-byte header & payload
        payload = struct.pack("<2BBBBBBBI", 0xA5, 0x5A, i + 1, 1, 12, 100 + i, 0x01, 85, 1420 + i * 5)
        crc = calculate_crc32(payload)
        packet = payload + struct.pack("<I", crc)
        
        parsed = decode_sensor_packet(packet)
        print(f"[MOCK] Dev #{parsed['device_id']:02d} -> Level: {parsed['level_pct']}%, Count: {parsed['actuation_count']} (CRC: {parsed['crc_valid']})")

if __name__ == '__main__':
    asyncio.run(scan_ble())
