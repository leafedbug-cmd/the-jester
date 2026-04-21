# Display Spec — Sparkle IoT Branch

No display on this branch. The Sparkle IoT ESP32-S3N16R8 has no screen.

Mode feedback is provided via the onboard **WS2812 RGB LED** (GPIO 48):

| Mode      | LED Color     |
|-----------|---------------|
| ANALYZE   | Blue          |
| BLUETOOTH | Red           |
| BLE       | Green         |
| BOTH      | Yellow/Orange |

Mode switching is done via the onboard **BOOT button** (GPIO 0).
