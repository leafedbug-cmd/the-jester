#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include <esp_bt.h>
#include <esp_wifi.h>

#include "esp32_nrf24_jammer/esp32_nrf24_jammer.h"

namespace {
constexpr int8_t  kLedPin        = 48;
constexpr uint8_t kBootButtonPin = 0;
constexpr uint8_t kModeCount     = 5;

constexpr uint8_t kRadio1CePin  = 4;
constexpr uint8_t kRadio1CsnPin = 5;
constexpr uint8_t kRadioSckPin  = 6;
constexpr uint8_t kRadioMosiPin = 7;
constexpr uint8_t kRadioMisoPin = 15;

constexpr uint8_t kRadio2CePin  = 16;
constexpr uint8_t kRadio2CsnPin = 17;

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

NRF24RadioConfig gPrimaryRadioConfig   = makeRadioConfig(gRadioSpi, kRadio1CePin, kRadio1CsnPin);
NRF24RadioConfig gSecondaryRadioConfig = makeRadioConfig(gRadioSpi, kRadio2CePin, kRadio2CsnPin);
ESP32NRF24Jammer gJammer(kLedPin, gPrimaryRadioConfig, gSecondaryRadioConfig);

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

    Serial.println("[APP] Probing primary nRF24L01+ radio...");
    if (gJammer.beginPrimary()) {
        Serial.println("[APP] Primary radio OK");
    } else {
        Serial.println("[APP] Primary radio FAILED");
    }

    Serial.println("[APP] Probing secondary nRF24L01+ radio...");
    if (gJammer.beginSecondary()) {
        Serial.println("[APP] Secondary radio OK");
    } else {
        Serial.println("[APP] Secondary radio FAILED");
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
        Serial.printf("[APP] Heap=%uKB R1=%s R2=%s Mode=%s\n",
            ESP.getFreeHeap() / 1024,
            gJammer.isPrimaryRadioReady()   ? "OK" : "OFF",
            gJammer.isSecondaryRadioReady() ? "OK" : "OFF",
            jammerModeName(gJammer.getMode()));
    }

    delay(1);
}
