#ifndef NRF24L01_H_
#define NRF24L01_H_

#include <Arduino.h>
#include <SPI.h>

class NRF24L01 {
public:
    NRF24L01() = default;
    NRF24L01(SPIClass& spi, uint8_t cePin, uint8_t csnPin, uint8_t sckPin, uint8_t misoPin, uint8_t mosiPin);

    void configure(SPIClass& spi, uint8_t cePin, uint8_t csnPin, uint8_t sckPin, uint8_t misoPin, uint8_t mosiPin);
    bool begin();

    // Park this radio in the idle/deselected state (CSN high, CE low) without
    // touching the bus. Used to keep every module off the shared MISO line
    // before probing any single one.
    void deselect();

    void powerDown();
    void powerUp();
    void reset();

    bool isPresent();
    // Diagnostic: write a known value to CONFIG and return the raw read-back
    // byte so the caller can distinguish a dead MISO/clock from real data.
    uint8_t readConfigRaw();
    void writeRegister(uint8_t reg, uint8_t value);
    uint8_t readRegister(uint8_t reg);
    void pulseCE();
    void setCEHigh();
    void setCELow();

    void setRxMode();
    void setTxMode();
    void setMaxPower();
    void disableAutoAck();
    void disableRetransmit();
    void setChannel(uint8_t channel);
    bool detectSignal();
    void scanAllChannels(uint8_t* results, int numSamples = 100);
    void flushTx();
    void writeTxPayload(const uint8_t* data, uint8_t len);
    void transmit();

private:
    SPIClass* _spi = nullptr;
    uint8_t _cePin = 0;
    uint8_t _csnPin = 0;
    uint8_t _sckPin = 0;
    uint8_t _misoPin = 0;
    uint8_t _mosiPin = 0;
    bool _busStarted = false;

    void activateBus();
    uint8_t transfer(uint8_t value);
};

#endif
