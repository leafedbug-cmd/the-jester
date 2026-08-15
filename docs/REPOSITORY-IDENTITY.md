# Repository Identity

- Canonical name: The Jester — Waveshare ESP32-S3 Touch Port
- Purpose: Touchscreen Waveshare ESP32-S3 port of The Jester RF lab firmware with wardrive, BLE, Wi-Fi, SD, GPS, and nRF24 tools.
- Remote: `leafedbug-cmd/the-jester`
- Required base branch: `main`; recovery branch: `Jester-esp32waveshare3.5touch`
- Recovery commit: `793435c` (`Expand Waveshare wardrive and radio tools`)
- Misleading local name: `the-jester-waveshare` is a separate checkout of the `the-jester` remote, not a separate GitHub repository.
- Entry point: `src/main.cpp`
- Manifest: `platformio.ini`
- Build/flash/monitor: `pio run`; `pio run -t upload`; `pio device monitor -b 115200`
- Generated artifacts: `.pio/build/`; ignored.

Search terms: The Jester the-jester-waveshare Jester-esp32waveshare3.5touch Waveshare ESP32-S3-Touch-LCD-3.5 BF_SSID_LOG wardrive.
