#ifndef ESP32_NRF24_JAMMER_H_
#define ESP32_NRF24_JAMMER_H_

#include <Arduino.h>
#include <SPI.h>

#include "NRF24L01.h"

enum class JammerMode : uint8_t {
    Bluetooth = 0,
    Ble,
    Wifi,
    All,
    Off,
};

const char* jammerModeName(JammerMode mode);

struct NRF24RadioConfig {
    SPIClass* spi = nullptr;
    uint8_t cePin = 0;
    uint8_t csnPin = 0;
    uint8_t sckPin = 0;
    uint8_t misoPin = 0;
    uint8_t mosiPin = 0;
};

struct JammerStateSnapshot {
    JammerMode mode = JammerMode::Bluetooth;
    bool radioReady = false;
    bool primaryRadioReady = false;
    bool secondaryRadioReady = false;
    bool jamming = false;
    uint8_t radiosReady = 0;
};

class ESP32NRF24Jammer {
public:
    ESP32NRF24Jammer(int8_t ledPin, const NRF24RadioConfig& primaryRadioConfig, const NRF24RadioConfig& secondaryRadioConfig);

    bool beginPrimary();
    bool beginSecondary();
    bool isRadioReady() const;
    bool isPrimaryRadioReady() const;
    bool isSecondaryRadioReady() const;
    uint8_t getReadyRadioCount() const;
    void setPrimaryRadioConfig(const NRF24RadioConfig& radioConfig);
    void setSecondaryRadioConfig(const NRF24RadioConfig& radioConfig);

    bool setMode(JammerMode mode);
    JammerMode getMode() const;
    void update();

    void setSafeMode(bool enable);
    bool isSafeMode() const;
    bool isJamming() const;

    void getStateSnapshot(JammerStateSnapshot& snapshot);

private:
    int8_t _ledPin;
    NRF24RadioConfig _primaryRadioConfig;
    NRF24RadioConfig _secondaryRadioConfig;
    NRF24L01 _primaryRadio;
    NRF24L01 _secondaryRadio;

    JammerMode _mode = JammerMode::Bluetooth;
    bool _safeMode = false;
    bool _isJamming = false;
    bool _primaryRadioReady = false;
    bool _secondaryRadioReady = false;
    uint8_t _primaryChannel = 0;
    uint8_t _secondaryChannel = 0;
    uint8_t _allPattern = 0;
    unsigned long _lastHealthCheckMs = 0;
    unsigned long _lastRecoveryAttemptMs = 0;

    portMUX_TYPE _stateMux = portMUX_INITIALIZER_UNLOCKED;

    void _refreshRadioPresence();
    void _recoverMissingRadios();
    void _transmitBurstOnRadio(NRF24L01& radio, uint8_t& currentChannel, uint8_t channel, uint8_t* noise, size_t noiseLength, uint8_t burstSeed);
    void _setLedColor(uint8_t red, uint8_t green, uint8_t blue);
    void _setChannelInternal(NRF24L01& radio, uint8_t& currentChannel, uint8_t channel);
    void _configureAggressiveMode(NRF24L01& radio);
    void _startJammingForCurrentMode();
    void _stopJamming();
    void _runBurstForMode();
};

#endif
