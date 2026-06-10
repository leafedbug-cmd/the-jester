# The Jester — Lonely Binary (Sparkle IoT S3N16R8) Wiring Spec

This branch targets the **Sparkle IoT ESP32-S3N16R8** (XH-S3E module, 16MB flash, 8MB PSRAM OPI) with **three** external `nRF24L01+ PA+LNA` breakout modules on a shared SPI bus.

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

## Shared SPI Bus (all three radios)

Run one wire from each ESP32 pin to the same pin on **all three** breakout boards.

| Signal    | ESP32 GPIO |
|-----------|-----------|
| SCK       | GPIO 12   |
| MO (MOSI) | GPIO 11   |
| MI (MISO) | GPIO 13   |

## Power (separate supply for radios)

The three PA+LNA modules run from an **external 3.3V supply**, NOT the ESP32's
3.3V rail.

| Signal | Source                                   |
|--------|------------------------------------------|
| VCC    | External 3.3V supply only (→ all 3 VCC)  |
| GND    | Common — external supply + ESP32 + all 3 |

- ESP32 is powered by **USB or 5V only**.
- **Do NOT** connect the external 3.3V to the ESP32 3.3V pin (back-feeding the
  rail makes two regulators fight and corrupts the SPI bus).
- The only wire between the external supply and the ESP32 is **ground**.
- Verify: with ESP32 USB/5V off and the nRF supply on, the ESP32 3.3V pin must
  read ~0V. If it reads 3.3V, the rail is being back-fed — remove that wire.

## Per-radio CE / CSN

Each module needs its own unique chip-enable and chip-select.

| Radio | CE       | CSN      |
|-------|----------|----------|
| 1     | GPIO 4   | GPIO 5   |
| 2     | GPIO 6   | GPIO 7   |
| 3     | GPIO 8   | GPIO 9   |

IRQ is left unconnected on all three modules.

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
         ┌────────────────────────────────────┐
  GPIO11 ─┤──┬──┬───────────────► SCK  (R1/R2/R3) │
  GPIO10 ─┤──┼──┼───────────────► MO   (R1/R2/R3) │
  GPIO15 ─┤──┼──┼───────────────► MI   (R1/R2/R3) │
    3.3V ─┤──┼──┼───────────────► VCC  (R1/R2/R3) │
     GND ─┤──┴──┴───────────────► GND  (R1/R2/R3) │
  GPIO12 ─┤────────────────────► Radio 1 CE      │
  GPIO13 ─┤────────────────────► Radio 1 CSN     │
  GPIO14 ─┤────────────────────► Radio 2 CE      │
   GPIO1 ─┤────────────────────► Radio 2 CSN     │
  GPIO40 ─┤────────────────────► Radio 3 CE      │
  GPIO39 ─┤────────────────────► Radio 3 CSN     │
  GPIO48 ─┤  RGB WS2812 (onboard)               │
   GPIO0 ─┤  BOOT button (onboard)              │
         └────────────────────────────────────┘
```

SCK (GPIO 11), MOSI (GPIO 10), and MISO (GPIO 15) are shared across all three modules — run one wire from each ESP32 pin to every radio board. Only CE/CSN are unique per radio.

---

## Power Notes

- Use **3.3V only** — do not connect VCC to 5V.
- Add a decoupling capacitor across VCC/GND at each module.
  - Minimum: 10 µF
  - Preferred: 47–220 µF
- Keep wires short.
- Keep antennas attached during use.
