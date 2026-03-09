#include "RF24.h"
#include <SPI.h>
#include "esp_bt.h"
#include "esp_wifi.h"
#include "Preferences.h"

constexpr int SPI_SPEED = 16000000;

// ===== Pin Assignments =====
constexpr int LED_PIN    = 48;  // WS2812 RGB
constexpr int CE_PIN     = 9;   // nRF24L01 CE
constexpr int CSN_PIN    = 10;  // nRF24L01 CSN
constexpr int SCK_PIN    = 12;  // SPI CLK
constexpr int MOSI_PIN   = 11;  // SPI MOSI
constexpr int MISO_PIN   = 13;  // SPI MISO
constexpr int JAM_SWITCH  = 21;  // External 3-pin switch
constexpr int BOOT_BUTTON = 0;   // On-board BOOT button
constexpr int PEER_RX_PIN = 17;  // UART1 RX (connect to other board's TX)
constexpr int PEER_TX_PIN = 18;  // UART1 TX (connect to other board's RX)

SPIClass *spi1 = nullptr;
RF24 radio1(CE_PIN, CSN_PIN, SPI_SPEED);

#ifndef SINGLE_RADIO
SPIClass *spi2 = nullptr;
RF24 radio2(CE_PIN, CSN_PIN, SPI_SPEED);
#endif

int bluetooth_channels[] = {32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80};
int ble_channels[] = {2, 26, 80};

enum Mode {
  OFF,
  BLUETOOTH,
  BLE,
  BOTH
};

Mode currentMode = OFF;
String inputString = "";
unsigned long lastBlinkTime = 0;
int blinkCount = 0;
bool ledOn = false;
bool lastSwitchState = HIGH;
bool lastBootState = HIGH;
bool switchActive = false;
String peerInputString = "";

Preferences preferences;

String getModeString(Mode mode) {
  switch (mode) {
    case OFF:
      return "OFF";
    case BLUETOOTH:
      return "BLUETOOTH";
    case BLE:
      return "BLE";
    case BOTH:
      return "BOTH";
    default:
      return "UNKNOWN";
  }
}

void saveDefaultMode(Mode mode) {
  preferences.begin("rfclown", false);
  preferences.putUChar("mode", (uint8_t)mode);
  preferences.end();
  Serial.println("Default mode: " + getModeString(mode));
}

Mode loadDefaultMode() {
  preferences.begin("rfclown", false);
  uint8_t mode = preferences.getUChar("mode", (uint8_t)OFF);
  preferences.end();
  return (Mode)mode;
}

void sendCurrentMode() {
  if (Serial) {
    Serial.println("default:" + getModeString(currentMode));
    Serial.flush();
  }
}

void configureRadio(RF24 &radio, int channel, SPIClass *spi) {
    if (radio.begin(spi)) {
        radio.setAutoAck(false);
        radio.stopListening();
        radio.setRetries(0, 0);
        radio.setPALevel(RF24_PA_MAX, true);
        radio.setDataRate(RF24_2MBPS);
        radio.setCRCLength(RF24_CRC_DISABLED);
        radio.startConstCarrier(RF24_PA_HIGH, channel);
    }
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, PEER_RX_PIN, PEER_TX_PIN);

  neopixelWrite(LED_PIN, 0, 0, 0); // LED off
  pinMode(JAM_SWITCH, INPUT_PULLUP);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  lastSwitchState = digitalRead(JAM_SWITCH);
  lastBootState = digitalRead(BOOT_BUTTON);
  
  currentMode = loadDefaultMode();

  // If switch is already ON at boot, activate the saved default mode
  if (lastSwitchState == LOW) {
    switchActive = true;
    currentMode = loadDefaultMode();
    if (currentMode == OFF) currentMode = BOTH; // fallback if default is OFF
  } else {
    currentMode = OFF;
  }
   
  esp_bt_controller_deinit();
  esp_wifi_stop();
  esp_wifi_deinit();
  esp_wifi_disconnect();
  
  spi1 = new SPIClass(FSPI);
  spi1->begin(SCK_PIN, MISO_PIN, MOSI_PIN, CSN_PIN);
  configureRadio(radio1, ble_channels[0], spi1);

#ifndef SINGLE_RADIO
  spi2 = new SPIClass(HSPI);
  spi2->begin(SCK_PIN, MISO_PIN, MOSI_PIN, CSN_PIN);
  configureRadio(radio2, bluetooth_channels[0], spi2);
#endif

  Serial.println("The Jester");
  Serial.println("Current mode: " + getModeString(currentMode));
  
  delay(500);
  if (Serial) {
    sendCurrentMode();
  }
}

void sendToPeer(Mode mode) {
  Serial1.println("mode:" + getModeString(mode));
}

void activateMode(Mode mode) {
  currentMode = mode;
  blinkCount = 0;
  lastBlinkTime = 0;
  Serial.println("Mode: " + getModeString(mode));
}

void activateModeAndSync(Mode mode) {
  activateMode(mode);
  sendToPeer(mode);
}

void handleCommand() {
  inputString.trim();
  inputString.toLowerCase();
  
  if (inputString == "help") {
    Serial.println(
      "mode:<mode> - set mode off, bluetooth, ble, both\n"
      "default:<mode> - set default mode off, bluetooth, ble, both"
    );
    
  } else if (inputString == "mode:off") {
    activateModeAndSync(OFF);
  } else if (inputString == "mode:bluetooth") {
    activateModeAndSync(BLUETOOTH);
  } else if (inputString == "mode:ble") {
    activateModeAndSync(BLE);
  } else if (inputString == "mode:both") {
    activateModeAndSync(BOTH);
  } else if (inputString == "default:off") {
    saveDefaultMode(OFF);
  } else if (inputString == "default:bluetooth") {
    saveDefaultMode(BLUETOOTH);
  } else if (inputString == "default:ble") {
    saveDefaultMode(BLE);
  } else if (inputString == "default:both") {
    saveDefaultMode(BOTH);
  } else {
    Serial.print("Unknown command: ");
    Serial.println(inputString);
  }
  
  inputString = "";
}

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(LED_PIN, r, g, b);
}

void checkBlink(unsigned long currentTime, int interval, int times, uint8_t r, uint8_t g, uint8_t b) {
  if (ledOn) {
    if (currentTime - lastBlinkTime >= interval) {
      setLedColor(0, 0, 0);
      ledOn = false;
      blinkCount++;
      lastBlinkTime = currentTime;
    }
  } else {
    if (blinkCount < times) {
      if (currentTime - lastBlinkTime >= interval) {
        setLedColor(r, g, b);
        ledOn = true;
        lastBlinkTime = currentTime;
      }
    } else {
      if (currentTime - lastBlinkTime >= 1000) {
        setLedColor(r, g, b);
        ledOn = true;
        blinkCount = 0;
        lastBlinkTime = currentTime;
      }
    }
  }
}

void handleLed() {
  unsigned long currentTime = millis();
  
  switch (currentMode) {
    case OFF:
      setLedColor(0, 0, 0);
      break;
      
    case BLUETOOTH:
      if (switchActive) setLedColor(0, 0, 255);
      else checkBlink(currentTime, 1000, 1, 0, 0, 255);   // Blue
      break;
      
    case BLE:
      if (switchActive) setLedColor(0, 255, 0);
      else checkBlink(currentTime, 300, 2, 0, 255, 0);    // Green
      break;
      
    case BOTH:
      if (switchActive) setLedColor(255, 0, 0);
      else checkBlink(currentTime, 200, 3, 255, 0, 0);    // Red
      break;
  }
}

void jamBLE() {
  int randomIndex = random(0, sizeof(ble_channels) / sizeof(ble_channels[0]));
  int channel = ble_channels[randomIndex];
  radio1.setChannel(channel);
#ifndef SINGLE_RADIO
  radio2.setChannel(channel);
#endif
}

void jamBluetooth() {
  int randomIndex = random(0, sizeof(bluetooth_channels) / sizeof(bluetooth_channels[0]));
  int channel = bluetooth_channels[randomIndex];
  radio1.setChannel(channel);
#ifndef SINGLE_RADIO
  radio2.setChannel(channel);
#endif
}

void jamAll() {
  if (random(0, 2)) {
      jamBluetooth();        
  } else {
      jamBLE();
  }
  //delayMicroseconds(20);
}

void executeMode() {
    switch (currentMode) {
        case OFF:
            //radio1.powerDown();
#ifndef SINGLE_RADIO
            //radio2.powerDown();
#endif
            delay(100);
            break;
        case BLE:
            jamBLE();
            break;
        case BLUETOOTH:
            jamBluetooth();
            break;
        case BOTH:
            jamAll();
            break;
    }
}

void handleSwitch() {
  bool switchState = digitalRead(JAM_SWITCH);
  if (switchState != lastSwitchState) {
    delay(50); // debounce
    switchState = digitalRead(JAM_SWITCH);
    if (switchState != lastSwitchState) {
      lastSwitchState = switchState;
      if (switchState == LOW) {
        // Switch turned ON — activate saved default mode
        switchActive = true;
        Mode saved = loadDefaultMode();
        if (saved == OFF) saved = BOTH; // fallback if default is OFF
        activateModeAndSync(saved);
      } else {
        // Switch turned OFF
        switchActive = false;
        activateModeAndSync(OFF);
      }
    }
  }
}

void handleBootButton() {
  bool state = digitalRead(BOOT_BUTTON);
  if (state == LOW && lastBootState == HIGH) {
    delay(50); // debounce
    state = digitalRead(BOOT_BUTTON);
    if (state == LOW) {
      // Cycle: OFF -> BLUETOOTH -> BLE -> BOTH -> OFF
      switch (currentMode) {
        case OFF:       activateModeAndSync(BLUETOOTH); break;
        case BLUETOOTH: activateModeAndSync(BLE);       break;
        case BLE:       activateModeAndSync(BOTH);      break;
        case BOTH:      activateModeAndSync(OFF);       break;
      }
    }
  }
  lastBootState = state;
}

void handlePeer() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\n') {
      peerInputString.trim();
      peerInputString.toLowerCase();
      if (peerInputString == "mode:off") {
        activateMode(OFF);
      } else if (peerInputString == "mode:bluetooth") {
        activateMode(BLUETOOTH);
      } else if (peerInputString == "mode:ble") {
        activateMode(BLE);
      } else if (peerInputString == "mode:both") {
        activateMode(BOTH);
      }
      peerInputString = "";
    } else {
      peerInputString += c;
    }
  }
}

void loop() {
  handleSwitch();
  handleBootButton();
  handlePeer();
  handleLed();
  executeMode();
}

void serialEvent() {
  while (Serial.available()) {
    char inChar = (char)Serial.read();
    
    if (inChar == '\n') {
      if (inputString.length() > 0) {
        handleCommand();
      }
    } else {
      inputString += inChar;
    }
  }
}
