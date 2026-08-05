# Hardware Components List

This document lists the hardware components used in the ESP32-S3-based IoT Water Monitoring System.

| Sr. No. | Component Name            | Specification / Description                     |
|--------:|---------------------------|-------------------------------------------------|
| 1       | ESP32-S3 Development Board | ESP32-S3 dev board (e.g. BITSKY ESP32-S3)      |
| 2       | Soil Moisture Sensor      | Analog soil moisture sensor module              |
| 3       | DHT Sensor                | DHT11 (Temperature & Humidity Sensor)           |
| 4       | Relay Module              | 5V Single Channel Relay Module                  |
| 5       | Solenoid Valve            | 12V DC Solenoid Valve                           |
| 6       | DC Adapter                | 12V DC Power Adapter                            |
| 7       | DC-DC Converter           | 12V to 5V Buck Converter                        |
| 8       | Connecting Wires          | Male-to-Male / Male-to-Female Jumper Wires      |
| 9       | Breadboard                | Standard Breadboard                             |

All components are selected to ensure low cost, easy availability, and suitability for educational and academic purposes.

## Board Selection Notes

The project was migrated from the **ESP32-C3 Super Mini** to the **ESP32-S3** for the following reasons:

1. **More GPIO pins available** — the S3 has 45 GPIOs vs the C3's 22, leaving room for future expansion (additional sensors, multi-zone valves, status LEDs, flow meters, etc.)
2. **No strapping pin conflict** — GPIO 8 (used for the relay) is a strapping pin on the C3 but is a normal GPIO on the S3.
3. **Native USB support** — the S3 has a USB-OTG controller, useful for future features like USB CDC serial without occupying UART0.
4. **More RAM and flash** — better headroom for the larger firmware that now includes WiFiManager, HTTPS (TLS stack), and Basic Auth.

All three pins used by this project (GPIO 1, 4, 8) are valid and safe on the ESP32-S3.
See [`pin-connections.md`](pin-connections.md) for full wiring details.
