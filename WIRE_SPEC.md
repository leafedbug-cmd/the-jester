# The Jester — Sparkle IoT S3N16R8 Wiring Spec

This branch targets the **Sparkle IoT ESP32-S3N16R8** (XH-S3E module, 16MB flash, 8MB PSRAM OPI) with two external `nRF24L01+ PA+LNA` breakout modules.

## NRF24L01+PA+LNA Breakout Header Layout

The breakout board has a 2×4 pin header:

```
[ GND ] [ VCC ]
[ CE  ] [ CSN ]
[ SCK ] [ MO  ]   <- MO = MOSI
[ MI  ] [ IRQ ]   <- MI = MISO
```

IRQ is not connected on either module.

---

## Radio 1 (Primary)

Used for spectrum scanning and TX in Analyze/Bluetooth/BLE modes.

| NRF24 Pin | ESP32 GPIO | Notes        |
|-----------|-----------|--------------|
| VCC       | 3.3V      | 3.3V only    |
| GND       | GND       |              |
| CE        | GPIO 4    |              |
| CSN       | GPIO 5    |              |
| SCK       | GPIO 6    | Shared bus   |
| MO (MOSI) | GPIO 7    | Shared bus   |
| MI (MISO) | GPIO 15   | Shared bus   |
| IRQ       | NC        | Leave open   |

## Radio 2 (Secondary)

Used for dual-radio TX in Bluetooth/BLE/Both modes.

| NRF24 Pin | ESP32 GPIO | Notes        |
|-----------|-----------|--------------|
| VCC       | 3.3V      | 3.3V only    |
| GND       | GND       |              |
| CE        | GPIO 16   |              |
| CSN       | GPIO 17   |              |
| SCK       | GPIO 6    | Shared bus   |
| MO (MOSI) | GPIO 7    | Shared bus   |
| MI (MISO) | GPIO 15   | Shared bus   |
| IRQ       | NC        | Leave open   |

---

## Onboard Peripherals

| Function    | GPIO   | Notes                        |
|-------------|--------|------------------------------|
| RGB WS2812  | GPIO 48 | Onboard LED, mode indicator  |
| BOOT button | GPIO 0 | Active LOW, internal pull-up  |

---

## Mode → LED Color

| Mode      | LED Color      |
|-----------|----------------|
| ANALYZE   | Blue           |
| BLUETOOTH | Red            |
| BLE       | Green          |
| BOTH      | Yellow/Orange  |

Press the **BOOT** button to cycle: ANALYZE → BLUETOOTH → BLE → BOTH → ANALYZE

---

## Wiring Overview

```
Sparkle IoT ESP32-S3N16R8
         ┌──────────────────────────────────┐
    3.3V ─┤──────────────────► Radio 1 VCC  │
     GND ─┤──────────────────► Radio 1 GND  │
   GPIO4 ─┤──────────────────► Radio 1 CE   │
   GPIO5 ─┤──────────────────► Radio 1 CSN  │
   GPIO6 ─┤──┬───────────────► Radio 1 SCK  │
   GPIO7 ─┤──┼───────────────► Radio 1 MO   │
  GPIO15 ─┤──┼───────────────► Radio 1 MI   │
    3.3V ─┤  │───────────────► Radio 2 VCC  │
     GND ─┤  │───────────────► Radio 2 GND  │
  GPIO16 ─┤──┼───────────────► Radio 2 CE   │
  GPIO17 ─┤──┼───────────────► Radio 2 CSN  │
          │  └───────────────► Radio 2 SCK  │
          │  └───────────────► Radio 2 MO   │
          │  └───────────────► Radio 2 MI   │
  GPIO48 ─┤  RGB WS2812 (onboard)           │
   GPIO0 ─┤  BOOT button (onboard)          │
         └──────────────────────────────────┘
```

SCK (GPIO 6), MOSI (GPIO 7), and MISO (GPIO 15) are shared between both modules — run one wire from each ESP32 pin to both radio boards.

---

## Power Notes

- Use **3.3V only** — do not connect VCC to 5V.
- Add a decoupling capacitor across VCC/GND at each module.
  - Minimum: 10 µF
  - Preferred: 47–220 µF
- Keep wires short.
- Keep antennas attached during use.
