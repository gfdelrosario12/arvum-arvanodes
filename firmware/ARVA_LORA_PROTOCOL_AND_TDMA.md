# ARVACORE LORA PROTOCOL AND TDMA SCHEDULING SPECIFICATION
**Version:** 1.0.0  
**Target Architecture:** ESP32 Field Nodes (ARVANODE-001 .. 006) + Semtech SX1278 SPI Transceiver + ArvaGate (LilyGO A-7670G R2)  
**Standard Frequency:** 433.00 MHz (Sub-GHz ISM Band, Philippines NTC Compliant)  
**Radio Modulation:** LoRa Chirp Spread Spectrum (CSS)  

---

## 1. Physical Layer & Radio Parameters

| Parameter | Configuration Value | Justification / Trade-off |
| :--- | :--- | :--- |
| **Carrier Frequency** | `433.000 MHz` | Penetrates dense wet rice crop canopy and earthen dikes (*pilapil*). |
| **Bandwidth (BW)** | `125 kHz` | Standard balance between link budget sensitivity and airtime. |
| **Spreading Factor (SF)** | `SF8` | Time-on-Air (ToA) ~148 ms; Link budget ~-129 dBm sensitivity. |
| **Coding Rate (CR)** | `4/5` | Forward error correction with low overhead. |
| **Preamble Length** | `8 symbols` | Fast synchronization without excessive battery consumption. |
| **Transmit Power** | `+17 dBm (50 mW)` to `+20 dBm` | Maximum range across 1.1-hectare rice farm with 3dBi omni whip. |
| **Sync Word** | `0x12` (Private Network) | Prevents cross-talk with public LoRaWAN networks. |

---

## 2. Hardware SPI Pinout Mapping (ESP32 Dev Module to SX1278)

```
SX1278 Pin       ESP32 Pin      Function / Role
------------------------------------------------------------------
VCC              3.3V           CRITICAL: Must NEVER connect to 5V
GND              GND            Common ground reference
SCK              GPIO 18        SPI Clock
MISO             GPIO 19        SPI Master In Slave Out
MOSI             GPIO 23        SPI Master Out Slave In
NSS / CS         GPIO 5         SPI Chip Select
RST              GPIO 14        Hardware Reset
DIO0             GPIO 2         Interrupt on Packet Received / Done
```

*Note on GPIO Conflict Resolution:*  
In standard Wi-Fi prototypes, GPIO 19 was used for `BTN_ACTIVATE` and GPIO 5 was used for `ULTRASONIC_TRIG`. When the SX1278 daughterboard is soldered:
- `ULTRASONIC_TRIG` relocates to **GPIO 13** (or secondary hardware timer pin).
- `BTN_ACTIVATE` relocates to **GPIO 12** (pulled down during boot to ensure clean strapping).
- Ultrasonic Echo remains on **GPIO 18** or shifts to **GPIO 15** via 1k/2k resistive divider.

---

## 3. TDMA Transmission Schedule (60-Second Epoch)

To prevent RF collisions on the single 433MHz carrier without multi-channel gateways, nodes transmit sequentially during assigned time-slots:

| Node ID | Assigned Parcel | Hydraulic Position | TDMA Slot Window | Random Jitter Window |
| :--- | :--- | :--- | :--- | :--- |
| **ARVANODE-001** | Pitak 1 | Upper Inflow | `00:00 - 00:05` | $\pm 400\text{ ms}$ |
| **ARVANODE-002** | Pitak 2 | North Ridge | `00:10 - 00:15` | $\pm 400\text{ ms}$ |
| **ARVANODE-003** | Pitak 3 | Benchmark Center | `00:20 - 00:25` | $\pm 400\text{ ms}$ |
| **ARVANODE-004** | Pitak 4 | Clay Low Center | `00:30 - 00:35` | $\pm 400\text{ ms}$ |
| **ARVANODE-005** | Pitak 5 | Distal Margin | `00:40 - 00:45` | $\pm 400\text{ ms}$ |
| **ARVANODE-006** | Pitak 6 | Tail Drainage | `00:50 - 00:55` | $\pm 400\text{ ms}$ |
| **GUARD BAND** | All | System Sync | `00:55 - 01:00` | Gateway Beacon / Drift Sync |

---

## 4. Binary Telemetry Frame Format (32 Bytes Compact)

```
Byte Offset   Field Name          Type / Encoding     Description
---------------------------------------------------------------------------------------------------
0x00 - 0x01   Protocol Header     uint16_t (0xA5C0)   Magic identifier ('ARVA_V1')
0x02          Node Index          uint8_t (1..6)      1 = ARVANODE-001, etc.
0x03          Message Sequence    uint8_t (0..255)    Rolling frame counter
0x04 - 0x07   Timestamp           uint32_t (epoch s)  Node UTC or relative uptime seconds
0x08 - 0x09   Water Level         int16_t (mm)        Signed distance relative to datum (e.g. -150 mm)
0x0A - 0x0B   Soil Moisture       uint16_t (0.1%)     0 to 1000 representing 0.0% to 100.0%
0x0C - 0x0D   Battery Voltage     uint16_t (mV)       3000 to 4200 mV
0x0E - 0x0F   Ambient Temp        int16_t (0.1 C)     e.g. 295 = 29.5 deg C
0x10          Status Bitmask      uint8_t             Bit 0: Sensor Err, Bit 1: Low Batt, Bit 2: Flooded
0x11 - 0x1E   HMAC-SHA256 Trunc   uint8_t[14]         First 14 bytes of HMAC-SHA256(Key, Payload)
0x1F          CRC8                uint8_t             Polynomial 0x07 checksum
```

---

## 5. Offline Flash Buffering (LittleFS)

When the radio link suffers temporary jamming or deep desaturation:
1. The node commits the un-transmitted binary telemetry frame to `/spool/unsent.bin`.
2. A maximum spool size of 512 frames (~16 KB flash) is maintained.
3. During the subsequent TDMA slot, if an ACK is received from the ArvaGate, the node bursts up to 2 historical frames alongside the current telemetry frame until the spool is empty.
