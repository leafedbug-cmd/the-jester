#ifndef ESP32_NRF24_JAMMER_H_
#define ESP32_NRF24_JAMMER_H_

#include <Arduino.h>
#include <SPI.h>

#include "NRF24L01.h"

enum class JammerMode : uint8_t {
    Bluetooth = 0,
    Ble,
    Wifi,
    WifiLock,
    Adaptive,
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

// Number of nRF24 radios wired onto the shared SPI bus.
constexpr uint8_t kNRF24RadioCount = 3;

struct JammerStateSnapshot {
    JammerMode mode = JammerMode::Bluetooth;
    bool radioReady = false;
    bool radioReadyFlags[kNRF24RadioCount] = {false};
    bool jamming = false;
    uint8_t radiosReady = 0;
};

class ESP32NRF24Jammer {
public:
    ESP32NRF24Jammer(int8_t ledPin, const NRF24RadioConfig (&radioConfigs)[kNRF24RadioCount]);

    // Probe a single radio by index [0..kNRF24RadioCount-1].
    bool beginRadio(uint8_t index);
    // Probe every configured radio; returns true if at least one came up.
    bool beginAll();
    bool isRadioReady() const;
    bool isRadioReady(uint8_t index) const;
    uint8_t getReadyRadioCount() const;
    void setRadioConfig(uint8_t index, const NRF24RadioConfig& radioConfig);

    bool setMode(JammerMode mode);
    JammerMode getMode() const;
    void update();

    void setSafeMode(bool enable);
    bool isSafeMode() const;
    bool isJamming() const;

    // Trust mode: once a radio has come up, keep it keyed even if a later
    // presence read-back flaps (e.g. brief brown-out). Radios that never came
    // up are still retried by the recovery loop. Default off.
    void setTrustMode(bool enable);
    bool isTrustMode() const;

    void getStateSnapshot(JammerStateSnapshot& snapshot);

private:
    int8_t _ledPin;
    NRF24RadioConfig _radioConfigs[kNRF24RadioCount];
    NRF24L01 _radios[kNRF24RadioCount];
    bool _radioReady[kNRF24RadioCount] = {false};
    uint8_t _radioChannel[kNRF24RadioCount] = {0};

    JammerMode _mode = JammerMode::Bluetooth;
    bool _safeMode = false;
    bool _isJamming = false;
    bool _trustMode = false;
    unsigned long _lastHealthCheckMs = 0;
    unsigned long _lastRecoveryAttemptMs = 0;
    unsigned long _lastWifiLockHopMs = 0;
    uint8_t _wifiLockSet = 0;  // which WiFi channel (CH1/6/11) is locked now
    unsigned long _lastAdaptiveScanMs = 0;
    uint8_t _adaptiveChannels[kNRF24RadioCount] = {0};  // busiest channels found

    portMUX_TYPE _stateMux = portMUX_INITIALIZER_UNLOCKED;

    void _idleAllChipSelects();
    void _refreshRadioPresence();
    void _recoverMissingRadios();
    void _transmitBurstOnRadio(NRF24L01& radio, uint8_t& currentChannel, uint8_t channel, uint8_t* noise, size_t noiseLength, uint8_t burstSeed);
    void _setLedColor(uint8_t red, uint8_t green, uint8_t blue);
    void _setChannelInternal(NRF24L01& radio, uint8_t& currentChannel, uint8_t channel);
    void _configureAggressiveMode(NRF24L01& radio);
    void _activateRadioForMode(uint8_t index);
    void _performAdaptiveScan();
    void _pickBusiestChannels(const uint8_t* busyPercent, uint8_t* out, uint8_t count);
    void _startJammingForCurrentMode();
    void _stopJamming();
    void _runBurstForMode();
};

#endif
