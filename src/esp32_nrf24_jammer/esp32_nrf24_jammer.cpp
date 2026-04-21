#include "esp32_nrf24_jammer.h"

#define REG_RF_CH 0x05

namespace {
constexpr uint8_t kRadioInitAttempts = 3;
constexpr int kRadioInitRetryDelayMs = 40;
constexpr unsigned long kHealthCheckIntervalMs = 750;
constexpr unsigned long kRecoveryAttemptIntervalMs = 2000;

constexpr uint8_t kBluetoothChannels[] = {32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80};
constexpr uint8_t kBleChannels[]       = {2, 26, 80};
// 2.4GHz WiFi CH1-13 mapped to nRF24 RF channel numbers (2400 + ch_mhz_center - 2400)
constexpr uint8_t kWifiChannels[]      = {2, 7, 12, 17, 22, 27, 32, 37, 42, 47, 52, 57, 62};

uint8_t randomChannelFrom(const uint8_t* channels, size_t count) {
    return channels[random(0, static_cast<long>(count))];
}

// Full 2.4GHz sweep counter — both radios walk the whole band offset by half
static uint8_t sAllSweepPos = 0;
}

const char* jammerModeName(JammerMode mode) {
    switch (mode) {
        case JammerMode::Bluetooth: return "BLUETOOTH";
        case JammerMode::Ble:       return "BLE";
        case JammerMode::Wifi:      return "WIFI";
        case JammerMode::All:       return "ALL";
        case JammerMode::Off:       return "OFF";
        default:                    return "UNKNOWN";
    }
}

ESP32NRF24Jammer::ESP32NRF24Jammer(int8_t ledPin, const NRF24RadioConfig& primaryRadioConfig, const NRF24RadioConfig& secondaryRadioConfig)
    : _ledPin(ledPin),
      _primaryRadioConfig(primaryRadioConfig),
      _secondaryRadioConfig(secondaryRadioConfig),
      _primaryRadio(*primaryRadioConfig.spi, primaryRadioConfig.cePin, primaryRadioConfig.csnPin, primaryRadioConfig.sckPin, primaryRadioConfig.misoPin, primaryRadioConfig.mosiPin),
      _secondaryRadio(*secondaryRadioConfig.spi, secondaryRadioConfig.cePin, secondaryRadioConfig.csnPin, secondaryRadioConfig.sckPin, secondaryRadioConfig.misoPin, secondaryRadioConfig.mosiPin) {}

bool ESP32NRF24Jammer::beginPrimary() {
    Serial.println("[nRF24-1] begin() starting...");

    if (_primaryRadioConfig.spi == nullptr) {
        Serial.println("[nRF24-1] ERROR: SPI bus not configured");
        _primaryRadioReady = false;
        return false;
    }

    for (uint8_t attempt = 1; attempt <= kRadioInitAttempts; ++attempt) {
        if (_primaryRadio.begin()) {
            _primaryRadio.reset();
            _primaryRadioReady = true;
            Serial.printf("[nRF24-1] SPI contact OK on attempt %u\n", attempt);
            return true;
        }
        Serial.printf("[nRF24-1] SPI contact attempt %u/%u failed\n", attempt, kRadioInitAttempts);
        delay(kRadioInitRetryDelayMs);
    }

    Serial.printf("[nRF24-1] ERROR: module not found on CE=%u CSN=%u SCK=%u MISO=%u MOSI=%u\n",
        _primaryRadioConfig.cePin, _primaryRadioConfig.csnPin, _primaryRadioConfig.sckPin, _primaryRadioConfig.misoPin, _primaryRadioConfig.mosiPin);
    _primaryRadioReady = false;
    return false;
}

bool ESP32NRF24Jammer::beginSecondary() {
    Serial.println("[nRF24-2] begin() starting...");

    if (_secondaryRadioConfig.spi == nullptr) {
        Serial.println("[nRF24-2] ERROR: SPI bus not configured");
        _secondaryRadioReady = false;
        return false;
    }

    for (uint8_t attempt = 1; attempt <= kRadioInitAttempts; ++attempt) {
        if (_secondaryRadio.begin()) {
            _secondaryRadio.reset();
            _secondaryRadioReady = true;
            Serial.printf("[nRF24-2] SPI contact OK on attempt %u\n", attempt);
            return true;
        }
        Serial.printf("[nRF24-2] SPI contact attempt %u/%u failed\n", attempt, kRadioInitAttempts);
        delay(kRadioInitRetryDelayMs);
    }

    Serial.printf("[nRF24-2] ERROR: module not found on CE=%u CSN=%u SCK=%u MISO=%u MOSI=%u\n",
        _secondaryRadioConfig.cePin, _secondaryRadioConfig.csnPin, _secondaryRadioConfig.sckPin, _secondaryRadioConfig.misoPin, _secondaryRadioConfig.mosiPin);
    _secondaryRadioReady = false;
    return false;
}

bool ESP32NRF24Jammer::isRadioReady() const {
    return _primaryRadioReady || _secondaryRadioReady;
}

bool ESP32NRF24Jammer::isPrimaryRadioReady() const {
    return _primaryRadioReady;
}

bool ESP32NRF24Jammer::isSecondaryRadioReady() const {
    return _secondaryRadioReady;
}

uint8_t ESP32NRF24Jammer::getReadyRadioCount() const {
    return static_cast<uint8_t>(_primaryRadioReady ? 1 : 0) + static_cast<uint8_t>(_secondaryRadioReady ? 1 : 0);
}

void ESP32NRF24Jammer::setPrimaryRadioConfig(const NRF24RadioConfig& radioConfig) {
    _primaryRadioConfig = radioConfig;
    if (_primaryRadioConfig.spi != nullptr) {
        _primaryRadio.configure(*_primaryRadioConfig.spi, _primaryRadioConfig.cePin, _primaryRadioConfig.csnPin,
            _primaryRadioConfig.sckPin, _primaryRadioConfig.misoPin, _primaryRadioConfig.mosiPin);
    }
    _primaryRadioReady = false;
}

void ESP32NRF24Jammer::setSecondaryRadioConfig(const NRF24RadioConfig& radioConfig) {
    _secondaryRadioConfig = radioConfig;
    if (_secondaryRadioConfig.spi != nullptr) {
        _secondaryRadio.configure(*_secondaryRadioConfig.spi, _secondaryRadioConfig.cePin, _secondaryRadioConfig.csnPin,
            _secondaryRadioConfig.sckPin, _secondaryRadioConfig.misoPin, _secondaryRadioConfig.mosiPin);
    }
    _secondaryRadioReady = false;
}

bool ESP32NRF24Jammer::setMode(JammerMode mode) {
    if (mode != JammerMode::Off && _safeMode) {
        Serial.println("[APP] TX mode blocked because safe mode is enabled");
        return false;
    }

    _mode = mode;

    if (mode == JammerMode::Off) {
        _stopJamming();
        Serial.println("[APP] Mode set to OFF");
        return true;
    }

    _startJammingForCurrentMode();
    Serial.printf("[APP] Mode set to %s\n", jammerModeName(mode));
    return isRadioReady();
}

JammerMode ESP32NRF24Jammer::getMode() const {
    return _mode;
}

void ESP32NRF24Jammer::update() {
    if ((millis() - _lastHealthCheckMs) >= kHealthCheckIntervalMs) {
        _refreshRadioPresence();
        _lastHealthCheckMs = millis();
    }
    if ((millis() - _lastRecoveryAttemptMs) >= kRecoveryAttemptIntervalMs) {
        _recoverMissingRadios();
        _lastRecoveryAttemptMs = millis();
    }

    if (_mode == JammerMode::Off || !_isJamming || !isRadioReady()) {
        return;
    }

    _runBurstForMode();
}

void ESP32NRF24Jammer::setSafeMode(bool enable) {
    _safeMode = enable;
    if (_safeMode && _mode != JammerMode::Off) {
        setMode(JammerMode::Off);
    }
}

bool ESP32NRF24Jammer::isSafeMode() const {
    return _safeMode;
}

bool ESP32NRF24Jammer::isJamming() const {
    return _isJamming;
}

void ESP32NRF24Jammer::getStateSnapshot(JammerStateSnapshot& snapshot) {
    portENTER_CRITICAL(&_stateMux);
    snapshot.mode = _mode;
    snapshot.radioReady = isRadioReady();
    snapshot.primaryRadioReady = _primaryRadioReady;
    snapshot.secondaryRadioReady = _secondaryRadioReady;
    snapshot.jamming = _isJamming;
    snapshot.radiosReady = getReadyRadioCount();
    portEXIT_CRITICAL(&_stateMux);
}

void ESP32NRF24Jammer::_refreshRadioPresence() {
    bool presenceChanged = false;

    if (_primaryRadioReady && !_primaryRadio.isPresent()) {
        _primaryRadioReady = false;
        presenceChanged = true;
        Serial.println("[nRF24-1] Radio removed or not responding");
    }

    if (_secondaryRadioReady && !_secondaryRadio.isPresent()) {
        _secondaryRadioReady = false;
        presenceChanged = true;
        Serial.println("[nRF24-2] Radio removed or not responding");
    }

    if (presenceChanged && !isRadioReady()) {
        _isJamming = false;
    }
}

void ESP32NRF24Jammer::_recoverMissingRadios() {
    if (!_primaryRadioReady) {
        for (uint8_t attempt = 1; attempt <= kRadioInitAttempts; ++attempt) {
            if (_primaryRadio.begin()) {
                _primaryRadio.reset();
                _primaryRadioReady = true;
                Serial.printf("[nRF24-1] Radio reconnected on attempt %u\n", attempt);
                break;
            }
            delay(kRadioInitRetryDelayMs);
        }
    }

    if (!_secondaryRadioReady) {
        for (uint8_t attempt = 1; attempt <= kRadioInitAttempts; ++attempt) {
            if (_secondaryRadio.begin()) {
                _secondaryRadio.reset();
                _secondaryRadioReady = true;
                Serial.printf("[nRF24-2] Radio reconnected on attempt %u\n", attempt);
                break;
            }
            delay(kRadioInitRetryDelayMs);
        }
    }
}

void ESP32NRF24Jammer::_transmitBurstOnRadio(NRF24L01& radio, uint8_t& currentChannel, uint8_t channel, uint8_t* noise, size_t noiseLength, uint8_t burstSeed) {
    _setChannelInternal(radio, currentChannel, channel);
    noise[0] ^= static_cast<uint8_t>(burstSeed + currentChannel);
    noise[15] += currentChannel;
    radio.flushTx();
    radio.writeTxPayload(noise, static_cast<uint8_t>(noiseLength));
    radio.transmit();
}

void ESP32NRF24Jammer::_setLedColor(uint8_t red, uint8_t green, uint8_t blue) {
    if (_ledPin >= 0) {
        neopixelWrite(_ledPin, red, green, blue);
    }
}

void ESP32NRF24Jammer::_setChannelInternal(NRF24L01& radio, uint8_t& currentChannel, uint8_t channel) {
    currentChannel = channel % 126;
    radio.writeRegister(REG_RF_CH, currentChannel);
    radio.pulseCE();
}

void ESP32NRF24Jammer::_configureAggressiveMode(NRF24L01& radio) {
    radio.setMaxPower();
    radio.disableAutoAck();
    radio.disableRetransmit();
    radio.setTxMode();
}

void ESP32NRF24Jammer::_startJammingForCurrentMode() {
    if (!isRadioReady()) {
        _isJamming = false;
        return;
    }

    if (_primaryRadioReady) {
        _configureAggressiveMode(_primaryRadio);
        _primaryRadio.powerUp();
    }
    if (_secondaryRadioReady) {
        _configureAggressiveMode(_secondaryRadio);
        _secondaryRadio.powerUp();
    }

    _isJamming = true;
}

void ESP32NRF24Jammer::_stopJamming() {
    if (_isJamming) {
        if (_primaryRadioReady) {
            _primaryRadio.powerDown();
        }
        if (_secondaryRadioReady) {
            _secondaryRadio.powerDown();
        }
        _isJamming = false;
    }
}

void ESP32NRF24Jammer::_runBurstForMode() {
    static uint8_t primaryNoise[32] = {
        0xFF, 0x00, 0xAA, 0x55, 0xFF, 0x00, 0xAA, 0x55,
        0x0F, 0xF0, 0x0F, 0xF0, 0x33, 0xCC, 0x33, 0xCC,
        0x55, 0xAA, 0x55, 0xAA, 0xFF, 0xFF, 0x00, 0x00,
        0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0
    };
    static uint8_t secondaryNoise[32] = {
        0x0F, 0xF0, 0xCC, 0x33, 0x77, 0x88, 0x55, 0xAA,
        0xFE, 0xEF, 0xC3, 0x3C, 0x69, 0x96, 0xA5, 0x5A,
        0x10, 0x20, 0x30, 0x40, 0xBA, 0xAD, 0xF0, 0x0D,
        0x9A, 0xBC, 0x78, 0x56, 0x34, 0x12, 0x00, 0xFF
    };

    constexpr size_t kBurstLen = 48;

    for (size_t burst = 0; burst < kBurstLen; ++burst) {
        const uint8_t b = static_cast<uint8_t>(burst);

        if (_mode == JammerMode::Bluetooth) {
            if (_primaryRadioReady) {
                _transmitBurstOnRadio(_primaryRadio, _primaryChannel,
                    randomChannelFrom(kBluetoothChannels, sizeof(kBluetoothChannels)), primaryNoise, sizeof(primaryNoise), b);
            }
            if (_secondaryRadioReady) {
                _transmitBurstOnRadio(_secondaryRadio, _secondaryChannel,
                    randomChannelFrom(kBluetoothChannels, sizeof(kBluetoothChannels)), secondaryNoise, sizeof(secondaryNoise), b + 31);
            }
            continue;
        }

        if (_mode == JammerMode::Ble) {
            if (_primaryRadioReady) {
                _transmitBurstOnRadio(_primaryRadio, _primaryChannel,
                    randomChannelFrom(kBleChannels, sizeof(kBleChannels)), primaryNoise, sizeof(primaryNoise), b);
            }
            if (_secondaryRadioReady) {
                _transmitBurstOnRadio(_secondaryRadio, _secondaryChannel,
                    randomChannelFrom(kBleChannels, sizeof(kBleChannels)), secondaryNoise, sizeof(secondaryNoise), b + 31);
            }
            continue;
        }

        if (_mode == JammerMode::Wifi) {
            if (_primaryRadioReady) {
                _transmitBurstOnRadio(_primaryRadio, _primaryChannel,
                    randomChannelFrom(kWifiChannels, sizeof(kWifiChannels)), primaryNoise, sizeof(primaryNoise), b);
            }
            if (_secondaryRadioReady) {
                _transmitBurstOnRadio(_secondaryRadio, _secondaryChannel,
                    randomChannelFrom(kWifiChannels, sizeof(kWifiChannels)), secondaryNoise, sizeof(secondaryNoise), b + 31);
            }
            continue;
        }

        if (_mode == JammerMode::All) {
            // Both radios sweep the full 2.4GHz band (ch 0-125) offset by 63
            // so they always hit different halves simultaneously
            const uint8_t ch1 = sAllSweepPos % 126;
            const uint8_t ch2 = (sAllSweepPos + 63) % 126;
            sAllSweepPos = (sAllSweepPos + 1) % 126;

            if (_primaryRadioReady) {
                _transmitBurstOnRadio(_primaryRadio, _primaryChannel, ch1, primaryNoise, sizeof(primaryNoise), b);
            }
            if (_secondaryRadioReady) {
                _transmitBurstOnRadio(_secondaryRadio, _secondaryChannel, ch2, secondaryNoise, sizeof(secondaryNoise), b + 31);
            } else if (_primaryRadioReady) {
                // Single radio: fire ch2 as well to keep sweep density up
                _transmitBurstOnRadio(_primaryRadio, _primaryChannel, ch2, primaryNoise, sizeof(primaryNoise), b + 17);
            }
        }
    }
}
