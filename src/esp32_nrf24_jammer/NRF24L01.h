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

    // Emit a continuous unmodulated carrier on `channel` at max power (100%
    // duty cycle). CE is held high; call powerDown()/setCELow() to stop.
    // NOTE: CONT_WAVE is weak/absent on many nRF24L01+ clones; prefer the
    // modulated flood below for real-world jamming.
    void startConstantCarrier(uint8_t channel);

    // Modulated max-power flood on `channel`: load `payload`, mark it for reuse
    // (REUSE_TX_PL) and hold CE high so the radio retransmits it back-to-back at
    // ~100% duty with no per-packet SPI overhead. Keys the PA reliably (unlike
    // CONT_WAVE) and occupies channel bandwidth. Stop with powerDown()+setCELow().
    void startPayloadFlood(uint8_t channel, const uint8_t* payload, uint8_t len);

    // Move a running flood to a new channel with minimal overhead — CE stays
    // high and reuse stays armed, so TX never stops. Used for fast band sweeps.
    void retuneFlood(uint8_t channel);

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
    void reuseTxPayload();  // REUSE_TX_PL command (0xE3)
};

#endif
