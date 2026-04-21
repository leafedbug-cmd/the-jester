#include "NRF24L01.h"

#define CMD_R_REGISTER 0x00
#define CMD_W_REGISTER 0x20
#define REG_CONFIG 0x00
#define NRF24_SPI_FREQUENCY 10000000

namespace {
constexpr int kScanListenTimeUs = 260;
constexpr int kRadioBootSettleMs = 8;
}

NRF24L01::NRF24L01(SPIClass& spi, uint8_t cePin, uint8_t csnPin, uint8_t sckPin, uint8_t misoPin, uint8_t mosiPin)
    : _spi(&spi), _cePin(cePin), _csnPin(csnPin), _sckPin(sckPin), _misoPin(misoPin), _mosiPin(mosiPin) {}

void NRF24L01::configure(SPIClass& spi, uint8_t cePin, uint8_t csnPin, uint8_t sckPin, uint8_t misoPin, uint8_t mosiPin) {
    _spi = &spi;
    _cePin = cePin;
    _csnPin = csnPin;
    _sckPin = sckPin;
    _misoPin = misoPin;
    _mosiPin = mosiPin;
}

bool NRF24L01::begin() {
    pinMode(_cePin, OUTPUT);
    pinMode(_csnPin, OUTPUT);
    digitalWrite(_cePin, LOW);
    digitalWrite(_csnPin, HIGH);

    activateBus();

    delay(kRadioBootSettleMs);
    reset();

    delay(5);
    return isPresent();
}

void NRF24L01::powerDown() {
    uint8_t config = readRegister(REG_CONFIG);
    config &= static_cast<uint8_t>(~0x02);
    writeRegister(REG_CONFIG, config);
}

void NRF24L01::powerUp() {
    uint8_t config = readRegister(REG_CONFIG);
    config |= 0x02;
    writeRegister(REG_CONFIG, config);
    delay(2);
}

void NRF24L01::reset() {
    powerDown();
    writeRegister(REG_CONFIG, 0x0A);
}

bool NRF24L01::isPresent() {
    const uint8_t probe = 0x2A;
    writeRegister(REG_CONFIG, probe);
    return readRegister(REG_CONFIG) == probe;
}

void NRF24L01::writeRegister(uint8_t reg, uint8_t value) {
    activateBus();
    digitalWrite(_csnPin, LOW);
    _spi->beginTransaction(SPISettings(NRF24_SPI_FREQUENCY, MSBFIRST, SPI_MODE0));
    transfer(CMD_W_REGISTER | (reg & 0x1F));
    transfer(value);
    _spi->endTransaction();
    digitalWrite(_csnPin, HIGH);
}

uint8_t NRF24L01::readRegister(uint8_t reg) {
    activateBus();
    digitalWrite(_csnPin, LOW);
    _spi->beginTransaction(SPISettings(NRF24_SPI_FREQUENCY, MSBFIRST, SPI_MODE0));
    transfer(CMD_R_REGISTER | (reg & 0x1F));
    const uint8_t value = transfer(0xFF);
    _spi->endTransaction();
    digitalWrite(_csnPin, HIGH);
    return value;
}

void NRF24L01::pulseCE() {
    digitalWrite(_cePin, LOW);
    delayMicroseconds(4);
    digitalWrite(_cePin, HIGH);
    delayMicroseconds(4);
}

void NRF24L01::setCEHigh() {
    digitalWrite(_cePin, HIGH);
}

void NRF24L01::setCELow() {
    digitalWrite(_cePin, LOW);
}

void NRF24L01::setMaxPower() {
    // 250kbps + max PA power (0x06: RF_SETUP = 0x27)
    // 250kbps gives longest symbol duration = most RF energy per packet
    // PA on E01-ML01DP5 amplifies chip output to +20dBm
    writeRegister(0x06, 0x27);
}

void NRF24L01::disableAutoAck() {
    writeRegister(0x01, 0x00);
}

void NRF24L01::disableRetransmit() {
    // SETUP_RETR: 0 retries, 0 delay
    writeRegister(0x04, 0x00);
}

void NRF24L01::setChannel(uint8_t channel) {
    writeRegister(0x05, channel & 0x7F);
}

void NRF24L01::setRxMode() {
    uint8_t config = readRegister(REG_CONFIG);
    config |= 0x03;
    writeRegister(REG_CONFIG, config);
    setCEHigh();
    delayMicroseconds(130);
}

void NRF24L01::setTxMode() {
    uint8_t config = readRegister(REG_CONFIG);
    config |= 0x02;
    config &= ~0x01;
    writeRegister(REG_CONFIG, config);
}

bool NRF24L01::detectSignal() {
    return (readRegister(0x09) & 0x01) != 0;
}

void NRF24L01::scanAllChannels(uint8_t* results, int numSamples) {
    disableAutoAck();

    for (int ch = 0; ch < 126; ++ch) {
        setChannel(ch);
        int detected = 0;

        for (int sample = 0; sample < numSamples; ++sample) {
            setRxMode();
            delayMicroseconds(kScanListenTimeUs);
            if (detectSignal()) {
                detected++;
            }
            setCELow();
        }

        results[ch] = (detected * 100) / numSamples;

        if (ch % 5 == 0) {
            yield();
            delay(1);
        }
    }
}

void NRF24L01::flushTx() {
    activateBus();
    digitalWrite(_csnPin, LOW);
    _spi->beginTransaction(SPISettings(NRF24_SPI_FREQUENCY, MSBFIRST, SPI_MODE0));
    transfer(0xE1);
    _spi->endTransaction();
    digitalWrite(_csnPin, HIGH);
}

void NRF24L01::writeTxPayload(const uint8_t* data, uint8_t len) {
    activateBus();
    digitalWrite(_csnPin, LOW);
    _spi->beginTransaction(SPISettings(NRF24_SPI_FREQUENCY, MSBFIRST, SPI_MODE0));
    transfer(0xA0);
    for (uint8_t i = 0; i < len; ++i) {
        transfer(data[i]);
    }
    _spi->endTransaction();
    digitalWrite(_csnPin, HIGH);
}

void NRF24L01::transmit() {
    setCEHigh();
    delayMicroseconds(10);
    setCELow();
}

void NRF24L01::activateBus() {
    if (!_busStarted) {
        _spi->begin(_sckPin, _misoPin, _mosiPin, -1);
        _busStarted = true;
    }
}

uint8_t NRF24L01::transfer(uint8_t value) {
    return _spi->transfer(value);
}
