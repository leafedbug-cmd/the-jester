<div align="center">
  <img src="https://github.com/user-attachments/assets/1b4ca968-6d71-41bb-a723-fbf235024d5b" alt="logo" width="200" height="auto" />
  
  <h1>The Jester</h1>
  <p>A fully open-source Bluetooth and BLE jammer based on the [RF-Clown project by cifertech](https://github.com/cifertech/RF-Clown). This implementation uses dual nRF24L01 modules with ESP32 for simultaneous BLE and Bluetooth operations, featuring serial command control and LED status indication.</p>
</div>

## ⚠️ Disclaimer

This project is intended for educational purposes, security research, and testing in controlled environments only. Use responsibly and in accordance with local laws and regulations.

## 🌟 Features

- **Dual nRF24L01 Configuration**: Utilizes both HSPI and VSPI on the ESP32 for simultaneous BLE and Bluetooth operations
- **Serial Command Interface**: Control modes and settings via serial commands
- **LED Status Indication**: Visual feedback showing current operating mode
- **Persistent Settings**: Default mode is saved and restored on startup
- **Multiple Operating Modes**: OFF, BLUETOOTH, BLE, and BOTH modes
- **Open-Source**: Fully transparent codebase for educational purposes

## 🔧 Hardware Requirements

- 1x ESP32 development board (tested with ESP32-DOIT-DEVKIT-V1)
- 2x nRF24L01+ modules
- 2x 10-100uF capacitors (any voltage above 5V)
- 1x LED
- 1x 4k7 resistor (for LED)

### Pin Configuration

| Component | ESP32 Pin |
|-----------|-----------|
| nRF24L01 1 (VSPI) | GPIO 22 (CE)<br>GPIO 21 (CSN)<br>GPIO 18 (SCK)<br>GPIO 23 (MOSI)<br>GPIO 19 (MISO) |
| nRF24L01 2 (HSPI) | GPIO 16 (CE)<br>GPIO 15 (CSN)<br>GPIO 14 (SCK)<br>GPIO 13 (MOSI)<br>GPIO 12 (MISO) |
| LED | GPIO 27 | - |

## 🔌 Hardware Setup

| Breadboard Setup | Perfboard Setup |
|------------------|-----------------|
| <img src="https://github.com/user-attachments/assets/2dc4e8e1-258e-46e0-a4a5-dc6de75b1984" alt="Breadboard Setup" width="300"> | <img src="https://github.com/user-attachments/assets/537aacd4-988e-48e8-81e3-0481a83e1596" alt="Perfboard Setup" width="300"> |

## 📋 Operating Modes

The device supports four operating modes:

1. **OFF** - Device is inactive, LED is off
2. **BLUETOOTH** - Jams Bluetooth Classic signals only
3. **BLE** - Jams Bluetooth Low Energy signals only  
4. **BOTH** - Alternates between Bluetooth and BLE jamming

### Default Mode on Startup

The device loads its default mode from non-volatile storage on startup. If no default has been set previously, it defaults to **OFF** mode. The current mode is displayed in the serial output and indicated by LED blinking patterns.

## 💡 LED Status Indication

The LED provides visual feedback about the current operating mode:

| Mode | LED Pattern | Description |
|------|-------------|-------------|
| **OFF** | Solid OFF | LED remains off |
| **BLUETOOTH** | 1 blink per second | Single blink every 1000ms |
| **BLE** | 2 blinks per cycle | Double blink every 300ms |
| **BOTH** | 3 blinks per cycle | Triple blink every 200ms |

The LED cycles repeat continuously while in active modes, with a 1-second pause between cycles.

## 🔌 Serial Commands

Connect to the device via serial at **115200 baud** to send commands. All commands are case-insensitive.

### Available Commands

#### Mode Control Commands
- `mode:off` - Set current mode to OFF
- `mode:bluetooth` - Set current mode to BLUETOOTH
- `mode:ble` - Set current mode to BLE  
- `mode:both` - Set current mode to BOTH

#### Default Mode Commands
- `default:off` - Set default startup mode to OFF
- `default:bluetooth` - Set default startup mode to BLUETOOTH
- `default:ble` - Set default startup mode to BLE
- `default:both` - Set default startup mode to BOTH

#### Help Command
- `help` - Display available commands

### Command Examples

```
mode:ble          # Switch to BLE jamming mode
default:bluetooth # Set Bluetooth as default startup mode
help              # Show available commands
```

## 🚀 Installation & Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) installed
- ESP32 board support package
- nRF24 library (automatically installed via PlatformIO)

### Building and Uploading

1. Clone this repository:
   ```bash
   git clone <your-repo-url>
   cd the-jester
   ```

2. Connect your ESP32 to your computer via USB

3. Build and upload using PlatformIO:
   ```bash
   pio run --target upload
   ```

4. Monitor serial output:
   ```bash
   pio device monitor
   ```

### Arduino IDE Alternative

If using Arduino IDE instead of PlatformIO:

1. Install ESP32 board package (version 1.0.5 or later)
2. Install RF24 library via Library Manager
3. Open `src/main.cpp` in Arduino IDE
4. Select your ESP32 board and upload

## 🔒 Security & Legal Notice

This device is designed for:
- Educational purposes
- Security research and testing
- Penetration testing in authorized environments
- Understanding RF communication vulnerabilities

**Important**: Always ensure you have proper authorization before testing in any environment. Unauthorized jamming may violate local laws and regulations.

## 🤝 Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues for bugs and feature requests.

---

**Remember**: Use this device responsibly and only in authorized testing environments!
