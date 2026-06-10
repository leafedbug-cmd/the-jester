#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include <esp_bt.h>
#include <esp_wifi.h>

#include "esp32_nrf24_jammer/esp32_nrf24_jammer.h"

namespace {
constexpr int8_t  kLedPin        = 48;
constexpr uint8_t kBootButtonPin = 0;
constexpr uint8_t kModeCount     = 7;

// Shared SPI bus — one wire from each of these pins fans out to all radios.
constexpr uint8_t kRadioSckPin  = 12;
constexpr uint8_t kRadioMosiPin = 11;
constexpr uint8_t kRadioMisoPin = 13;

// Per-radio chip enable / chip select (unique to each module).
constexpr uint8_t kRadio1CePin  = 4;
constexpr uint8_t kRadio1CsnPin = 5;
constexpr uint8_t kRadio2CePin  = 6;
constexpr uint8_t kRadio2CsnPin = 7;
constexpr uint8_t kRadio3CePin  = 8;
constexpr uint8_t kRadio3CsnPin = 9;

SPIClass gRadioSpi(FSPI);
Preferences gPreferences;

NRF24RadioConfig makeRadioConfig(SPIClass& spi, uint8_t cePin, uint8_t csnPin) {
    NRF24RadioConfig config;
    config.spi     = &spi;
    config.cePin   = cePin;
    config.csnPin  = csnPin;
    config.sckPin  = kRadioSckPin;
    config.mosiPin = kRadioMosiPin;
    config.misoPin = kRadioMisoPin;
    return config;
}

NRF24RadioConfig gRadioConfigs[kNRF24RadioCount] = {
    makeRadioConfig(gRadioSpi, kRadio1CePin, kRadio1CsnPin),
    makeRadioConfig(gRadioSpi, kRadio2CePin, kRadio2CsnPin),
    makeRadioConfig(gRadioSpi, kRadio3CePin, kRadio3CsnPin),
};
ESP32NRF24Jammer gJammer(kLedPin, gRadioConfigs);

// RGB party state for ALL mode
uint8_t gPartyHue = 0;

void hsvToRgb(uint8_t h, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint8_t region = h / 43;
    uint8_t rem    = (h - region * 43) * 6;
    uint8_t q      = 255 - rem;
    uint8_t t      = rem;
    switch (region) {
        case 0: r = 255; g = t;   b = 0;   break;
        case 1: r = q;   g = 255; b = 0;   break;
        case 2: r = 0;   g = 255; b = t;   break;
        case 3: r = 0;   g = q;   b = 255; break;
        case 4: r = t;   g = 0;   b = 255; break;
        default:r = 255; g = 0;   b = q;   break;
    }
}

void setLedForMode(JammerMode mode) {
    switch (mode) {
        case JammerMode::Bluetooth: neopixelWrite(kLedPin,   0,   0, 255); break; // blue
        case JammerMode::Ble:       neopixelWrite(kLedPin, 255,  20, 147); break; // pink
        case JammerMode::Wifi:      neopixelWrite(kLedPin,   0, 255,   0); break; // green
        case JammerMode::WifiLock:  neopixelWrite(kLedPin,   0, 255, 255); break; // cyan
        case JammerMode::Adaptive:  neopixelWrite(kLedPin, 255, 255, 255); break; // white
        case JammerMode::All: {
            uint8_t r, g, b;
            hsvToRgb(gPartyHue, r, g, b);
            neopixelWrite(kLedPin, r, g, b);
            break;
        }
        case JammerMode::Off:
            // pulsing red handled in updateOffPulse()
            break;
    }
}

void updateOffPulse() {
    static unsigned long lastPulseMs = 0;
    static uint8_t brightness = 0;
    static int8_t direction = 4;
    if (millis() - lastPulseMs < 16) return;
    lastPulseMs = millis();
    neopixelWrite(kLedPin, brightness, 0, 0);
    brightness = static_cast<uint8_t>(brightness + direction);
    if (brightness >= 200) direction = -4;
    if (brightness == 0)   direction =  4;
}

void savePreferredMode(JammerMode mode) {
    gPreferences.begin("the-jester", false);
    gPreferences.putUChar("default_mode", static_cast<uint8_t>(mode));
    gPreferences.end();
}

JammerMode loadPreferredMode() {
    gPreferences.begin("the-jester", true);
    const uint8_t stored = gPreferences.getUChar("default_mode", static_cast<uint8_t>(JammerMode::Bluetooth));
    gPreferences.end();
    if (stored >= kModeCount) {
        return JammerMode::Bluetooth;
    }
    return static_cast<JammerMode>(stored);
}
}

void setup() {
    Serial.begin(921600);
    delay(1500);

    Serial.println("\n\n===== THE JESTER / SPARKLE IOT S3N16R8 =====");
    Serial.printf("Flash Size: %u MB\n", ESP.getFlashChipSize() / 1024 / 1024);
    Serial.printf("Free Heap: %u KB\n", ESP.getFreeHeap() / 1024);
    Serial.printf("PSRAM Size: %u KB\n", ESP.getPsramSize() / 1024);

    esp_bt_controller_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_wifi_disconnect();

    randomSeed(static_cast<uint32_t>(esp_random()));

    pinMode(kBootButtonPin, INPUT_PULLUP);

    for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
        Serial.printf("[APP] Probing nRF24L01+ radio %u...\n", i + 1);
        if (gJammer.beginRadio(i)) {
            Serial.printf("[APP] Radio %u OK\n", i + 1);
        } else {
            Serial.printf("[APP] Radio %u FAILED\n", i + 1);
        }
    }

    // Keep working radios keyed even if a presence read-back flaps mid-run.
    gJammer.setTrustMode(true);

    // Green blink once per radio detected at boot, as a count indicator
    // (3 blinks = all three up, 2 = two up, etc.).
    const uint8_t readyRadios = gJammer.getReadyRadioCount();
    if (readyRadios > 0) {
        Serial.printf("[APP] %u radio(s) enumerated - green confirmation blink\n", readyRadios);
        neopixelWrite(kLedPin, 0, 0, 0);
        delay(400);  // dark lead-in so the blinks read as distinct from boot
        for (uint8_t blink = 0; blink < readyRadios; ++blink) {
            neopixelWrite(kLedPin, 0, 255, 0);
            delay(350);
            neopixelWrite(kLedPin, 0, 0, 0);
            delay(300);
        }
        delay(250);  // settle before the steady mode color takes over
    }

    gJammer.setMode(JammerMode::Bluetooth);
    setLedForMode(JammerMode::Bluetooth);
    Serial.printf("[APP] Boot mode: BLUETOOTH | Radios ready: %u\n", gJammer.getReadyRadioCount());
}

void loop() {
    static unsigned long lastButtonMs    = 0;
    static unsigned long lastHeartbeatMs = 0;
    static unsigned long lastPartyMs     = 0;

    // BOOT button cycles modes
    if (digitalRead(kBootButtonPin) == LOW && (millis() - lastButtonMs) > 200) {
        lastButtonMs = millis();
        const JammerMode next = static_cast<JammerMode>((static_cast<uint8_t>(gJammer.getMode()) + 1) % kModeCount);
        gJammer.setMode(next);
        setLedForMode(next);
        savePreferredMode(next);
        Serial.printf("[APP] Mode -> %s\n", jammerModeName(next));
    }

    // Animate LED for special modes
    const JammerMode mode = gJammer.getMode();
    if (mode == JammerMode::Off) {
        updateOffPulse();
    } else if (mode == JammerMode::All && (millis() - lastPartyMs) > 20) {
        lastPartyMs = millis();
        gPartyHue += 3;
        setLedForMode(JammerMode::All);
    }

    gJammer.update();

    if ((millis() - lastHeartbeatMs) > 30000) {
        lastHeartbeatMs = millis();
        Serial.printf("[APP] Heap=%uKB R1=%s R2=%s R3=%s Mode=%s\n",
            ESP.getFreeHeap() / 1024,
            gJammer.isRadioReady(0) ? "OK" : "OFF",
            gJammer.isRadioReady(1) ? "OK" : "OFF",
            gJammer.isRadioReady(2) ? "OK" : "OFF",
            jammerModeName(gJammer.getMode()));
    }

    delay(1);
}
