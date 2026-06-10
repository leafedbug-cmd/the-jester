#include "esp32_nrf24_jammer.h"

#define REG_RF_CH 0x05

namespace {
constexpr uint8_t kRadioInitAttempts = 3;
constexpr int kRadioInitRetryDelayMs = 40;
constexpr unsigned long kHealthCheckIntervalMs = 750;
constexpr unsigned long kRecoveryAttemptIntervalMs = 500;

constexpr uint8_t kBluetoothChannels[] = {32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80};
constexpr uint8_t kBleChannels[]       = {2, 26, 80};
// 2.4GHz WiFi CH1-13 mapped to nRF24 RF channel numbers (2400 + ch_mhz_center - 2400)
constexpr uint8_t kWifiChannels[]      = {2, 7, 12, 17, 22, 27, 32, 37, 42, 47, 52, 57, 62};

// Full 2.4GHz sweep counter — radios walk the whole band offset by even slices
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

ESP32NRF24Jammer::ESP32NRF24Jammer(int8_t ledPin, const NRF24RadioConfig (&radioConfigs)[kNRF24RadioCount])
    : _ledPin(ledPin) {
    for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
        _radioConfigs[i] = radioConfigs[i];
        if (_radioConfigs[i].spi != nullptr) {
            _radios[i].configure(*_radioConfigs[i].spi, _radioConfigs[i].cePin, _radioConfigs[i].csnPin,
                _radioConfigs[i].sckPin, _radioConfigs[i].misoPin, _radioConfigs[i].mosiPin);
        }
    }
}

void ESP32NRF24Jammer::_idleAllChipSelects() {
    // Park every module deselected (CSN high) so a not-yet-probed radio with a
    // floating CSN can't drive the shared MISO line while we talk to another.
    for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
        if (_radioConfigs[i].spi != nullptr) {
            _radios[i].deselect();
        }
    }
}

bool ESP32NRF24Jammer::beginRadio(uint8_t index) {
    if (index >= kNRF24RadioCount) {
        return false;
    }

    const uint8_t label = index + 1;
    Serial.printf("[nRF24-%u] begin() starting...\n", label);

    if (_radioConfigs[index].spi == nullptr) {
        Serial.printf("[nRF24-%u] ERROR: SPI bus not configured\n", label);
        _radioReady[index] = false;
        return false;
    }

    _idleAllChipSelects();

    for (uint8_t attempt = 1; attempt <= kRadioInitAttempts; ++attempt) {
        if (_radios[index].begin()) {
            _radios[index].reset();
            _radioReady[index] = true;
            Serial.printf("[nRF24-%u] SPI contact OK on attempt %u\n", label, attempt);
            return true;
        }
        Serial.printf("[nRF24-%u] SPI contact attempt %u/%u failed\n", label, attempt, kRadioInitAttempts);
        delay(kRadioInitRetryDelayMs);
    }

    Serial.printf("[nRF24-%u] ERROR: module not found on CE=%u CSN=%u SCK=%u MISO=%u MOSI=%u\n",
        label, _radioConfigs[index].cePin, _radioConfigs[index].csnPin, _radioConfigs[index].sckPin,
        _radioConfigs[index].misoPin, _radioConfigs[index].mosiPin);

    // Diagnostic: write 0x2A to CONFIG and read it back raw.
    //   0x2A -> module present and answering (should not reach here)
    //   0xFF -> MISO stuck high / disconnected (no data returning)
    //   0x00 -> MISO or SCK dead / stuck low
    //   other -> marginal wiring or power on the shared bus
    _idleAllChipSelects();
    const uint8_t raw = _radios[index].readConfigRaw();
    const char* hint = "marginal bus/power";
    if (raw == 0xFF)      hint = "MISO stuck HIGH / disconnected";
    else if (raw == 0x00) hint = "MISO/SCK dead (stuck LOW)";
    else if (raw == 0x2A) hint = "answered correctly (intermittent!)";
    Serial.printf("[nRF24-%u] DIAG: CONFIG readback=0x%02X -> %s\n", label, raw, hint);

    _radioReady[index] = false;
    return false;
}

bool ESP32NRF24Jammer::beginAll() {
    bool any = false;
    for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
        any = beginRadio(i) || any;
    }
    return any;
}

bool ESP32NRF24Jammer::isRadioReady() const {
    for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
        if (_radioReady[i]) {
            return true;
        }
    }
    return false;
}

bool ESP32NRF24Jammer::isRadioReady(uint8_t index) const {
    return index < kNRF24RadioCount && _radioReady[index];
}

uint8_t ESP32NRF24Jammer::getReadyRadioCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
        if (_radioReady[i]) {
            ++count;
        }
    }
    return count;
}

void ESP32NRF24Jammer::setRadioConfig(uint8_t index, const NRF24RadioConfig& radioConfig) {
    if (index >= kNRF24RadioCount) {
        return;
    }
    _radioConfigs[index] = radioConfig;
    if (_radioConfigs[index].spi != nullptr) {
        _radios[index].configure(*_radioConfigs[index].spi, _radioConfigs[index].cePin, _radioConfigs[index].csnPin,
            _radioConfigs[index].sckPin, _radioConfigs[index].misoPin, _radioConfigs[index].mosiPin);
    }
    _radioReady[index] = false;
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

void ESP32NRF24Jammer::setTrustMode(bool enable) {
    _trustMode = enable;
    Serial.printf("[APP] Trust mode %s\n", enable ? "ENABLED" : "disabled");
}

bool ESP32NRF24Jammer::isTrustMode() const {
    return _trustMode;
}

void ESP32NRF24Jammer::getStateSnapshot(JammerStateSnapshot& snapshot) {
    portENTER_CRITICAL(&_stateMux);
    snapshot.mode = _mode;
    snapshot.radioReady = isRadioReady();
    for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
        snapshot.radioReadyFlags[i] = _radioReady[i];
    }
    snapshot.jamming = _isJamming;
    snapshot.radiosReady = getReadyRadioCount();
    portEXIT_CRITICAL(&_stateMux);
}

void ESP32NRF24Jammer::_refreshRadioPresence() {
    // In trust mode a radio that came up stays keyed even if a presence
    // read-back flaps, so a momentary brown-out never gates off a working TX.
    if (_trustMode) {
        return;
    }

    bool presenceChanged = false;

    for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
        if (_radioReady[i] && !_radios[i].isPresent()) {
            _radioReady[i] = false;
            presenceChanged = true;
            Serial.printf("[nRF24-%u] Radio removed or not responding\n", i + 1);
        }
    }

    if (presenceChanged && !isRadioReady()) {
        _isJamming = false;
    }
}

void ESP32NRF24Jammer::_recoverMissingRadios() {
    bool recoveredAny = false;

    // Keep already-up radios deselected while we re-probe the missing ones.
    _idleAllChipSelects();

    for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
        if (_radioReady[i] || _radioConfigs[i].spi == nullptr) {
            continue;
        }
        for (uint8_t attempt = 1; attempt <= kRadioInitAttempts; ++attempt) {
            if (_radios[i].begin()) {
                _radios[i].reset();
                _radioReady[i] = true;
                recoveredAny = true;
                // If we're already mid-jam, bring the freshly-found radio
                // straight into the active burst.
                if (_isJamming && _mode != JammerMode::Off) {
                    _configureAggressiveMode(_radios[i]);
                    _radios[i].powerUp();
                }
                Serial.printf("[nRF24-%u] Radio reconnected on attempt %u (jamming=%s)\n",
                    i + 1, attempt, _isJamming ? "yes" : "no");
                break;
            }
            delay(kRadioInitRetryDelayMs);
        }
    }

    // Covers the "no radios at boot" case: once the first one shows up and the
    // mode wants TX, start jamming now instead of waiting for a mode change.
    if (recoveredAny && !_isJamming && !_safeMode && _mode != JammerMode::Off && isRadioReady()) {
        _startJammingForCurrentMode();
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

    for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
        if (_radioReady[i]) {
            _configureAggressiveMode(_radios[i]);
            _radios[i].powerUp();
        }
    }

    _isJamming = true;
}

void ESP32NRF24Jammer::_stopJamming() {
    if (_isJamming) {
        for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
            if (_radioReady[i]) {
                _radios[i].powerDown();
            }
        }
        _isJamming = false;
    }
}

void ESP32NRF24Jammer::_runBurstForMode() {
    // One distinct noise buffer per radio so co-located modules don't
    // transmit identical bit patterns.
    static uint8_t radioNoise[kNRF24RadioCount][32] = {
        {
            0xFF, 0x00, 0xAA, 0x55, 0xFF, 0x00, 0xAA, 0x55,
            0x0F, 0xF0, 0x0F, 0xF0, 0x33, 0xCC, 0x33, 0xCC,
            0x55, 0xAA, 0x55, 0xAA, 0xFF, 0xFF, 0x00, 0x00,
            0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0
        },
        {
            0x0F, 0xF0, 0xCC, 0x33, 0x77, 0x88, 0x55, 0xAA,
            0xFE, 0xEF, 0xC3, 0x3C, 0x69, 0x96, 0xA5, 0x5A,
            0x10, 0x20, 0x30, 0x40, 0xBA, 0xAD, 0xF0, 0x0D,
            0x9A, 0xBC, 0x78, 0x56, 0x34, 0x12, 0x00, 0xFF
        },
        {
            0xC3, 0x3C, 0x5A, 0xA5, 0x96, 0x69, 0x0F, 0xF0,
            0x1F, 0x8E, 0x7D, 0x6C, 0x5B, 0x4A, 0x39, 0x28,
            0xAA, 0x55, 0xF0, 0x0F, 0xCC, 0x33, 0x99, 0x66,
            0xDE, 0xAD, 0xBE, 0xEF, 0xFA, 0xCE, 0xB0, 0x0C
        }
    };

    constexpr size_t kBurstLen = 48;
    // Even spacing across the 126-channel band for the ALL sweep.
    constexpr uint8_t kSweepStep = 126 / kNRF24RadioCount;

    for (size_t burst = 0; burst < kBurstLen; ++burst) {
        const uint8_t b = static_cast<uint8_t>(burst);

        if (_mode == JammerMode::Bluetooth || _mode == JammerMode::Ble || _mode == JammerMode::Wifi) {
            const uint8_t* channels = kBluetoothChannels;
            size_t channelCount = sizeof(kBluetoothChannels);
            if (_mode == JammerMode::Ble) {
                channels = kBleChannels;
                channelCount = sizeof(kBleChannels);
            } else if (_mode == JammerMode::Wifi) {
                channels = kWifiChannels;
                channelCount = sizeof(kWifiChannels);
            }

            // Partition the channel list across the radios instead of each one
            // picking at random. Radio i strides the list by kNRF24RadioCount
            // starting at offset i, so the radios always hammer three different
            // channels simultaneously. For BLE (exactly 3 advertising channels)
            // this becomes a 1:1 lock: each radio nails one adv channel
            // continuously. For BT/WiFi it interleaves thirds of the band.
            for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
                if (!_radioReady[i]) {
                    continue;
                }
                const uint8_t idx = static_cast<uint8_t>((i + static_cast<size_t>(b) * kNRF24RadioCount) % channelCount);
                _transmitBurstOnRadio(_radios[i], _radioChannel[i],
                    channels[idx],
                    radioNoise[i], sizeof(radioNoise[i]), static_cast<uint8_t>(b + i * 31));
            }
            continue;
        }

        if (_mode == JammerMode::All) {
            // Each radio sweeps the full band offset by an even slice so they
            // always cover different regions simultaneously.
            const uint8_t base = sAllSweepPos;
            sAllSweepPos = (sAllSweepPos + 1) % 126;

            const uint8_t readyCount = getReadyRadioCount();
            if (readyCount == 0) {
                continue;
            }

            if (readyCount == 1) {
                // Single radio: fire every sweep slice on it to keep density up.
                for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
                    if (!_radioReady[i]) {
                        continue;
                    }
                    for (uint8_t slice = 0; slice < kNRF24RadioCount; ++slice) {
                        const uint8_t ch = static_cast<uint8_t>((base + slice * kSweepStep) % 126);
                        _transmitBurstOnRadio(_radios[i], _radioChannel[i], ch,
                            radioNoise[i], sizeof(radioNoise[i]), static_cast<uint8_t>(b + slice * 17));
                    }
                    break;
                }
                continue;
            }

            for (uint8_t i = 0; i < kNRF24RadioCount; ++i) {
                if (!_radioReady[i]) {
                    continue;
                }
                const uint8_t ch = static_cast<uint8_t>((base + i * kSweepStep) % 126);
                _transmitBurstOnRadio(_radios[i], _radioChannel[i], ch,
                    radioNoise[i], sizeof(radioNoise[i]), static_cast<uint8_t>(b + i * 31));
            }
        }
    }
}
