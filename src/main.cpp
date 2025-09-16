#include "RF24.h"
#include <SPI.h>
#include "esp_bt.h"
#include "esp_wifi.h"
#include "Preferences.h"

constexpr int SPI_SPEED = 16000000;
constexpr int LED_PIN = 27;

SPIClass *spiVSPI = nullptr;
SPIClass *spiHSPI = nullptr;
RF24 radioVSPI(22, 21, SPI_SPEED);
RF24 radioHSPI(16, 15, SPI_SPEED);

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

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  currentMode = loadDefaultMode();
   
  esp_bt_controller_deinit();
  esp_wifi_stop();
  esp_wifi_deinit();
  esp_wifi_disconnect();
  
  spiVSPI = new SPIClass(VSPI);
  spiVSPI->begin();
  configureRadio(radioVSPI, ble_channels[0], spiVSPI);
  
  spiHSPI = new SPIClass(HSPI);
  spiHSPI->begin();
  configureRadio(radioHSPI, bluetooth_channels[0], spiHSPI);

  Serial.println("The Jester");
  Serial.println("Current mode: " + getModeString(currentMode));
  
  delay(500);
  if (Serial) {
    sendCurrentMode();
  }
}

void activateMode(Mode mode) {
  currentMode = mode;
  blinkCount = 0;
  lastBlinkTime = 0;
  Serial.println("Mode: " + getModeString(mode));
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
    activateMode(OFF);
  } else if (inputString == "mode:bluetooth") {
    activateMode(BLUETOOTH);
  } else if (inputString == "mode:ble") {
    activateMode(BLE);
  } else if (inputString == "mode:both") {
    activateMode(BOTH);
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

void checkBlink(unsigned long currentTime, int interval, int times) {
  if (ledOn) {
    if (currentTime - lastBlinkTime >= interval) {
      digitalWrite(LED_PIN, LOW);
      ledOn = false;
      blinkCount++;
      lastBlinkTime = currentTime;
    }
  } else {
    if (blinkCount < times) {
      if (currentTime - lastBlinkTime >= interval) {
        digitalWrite(LED_PIN, HIGH);
        ledOn = true;
        lastBlinkTime = currentTime;
      }
    } else {
      if (currentTime - lastBlinkTime >= 1000) {
        digitalWrite(LED_PIN, HIGH);
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
      digitalWrite(LED_PIN, LOW);
      break;
      
    case BLUETOOTH:
      checkBlink(currentTime, 1000, 1);
      break;
      
    case BLE:
      checkBlink(currentTime, 300, 2);
      break;
      
    case BOTH:
      checkBlink(currentTime, 200, 3);
      break;
  }
}

void jamBLE() {
  int randomIndex = random(0, sizeof(ble_channels) / sizeof(ble_channels[0]));
  int channel = ble_channels[randomIndex];
  radioVSPI.setChannel(channel);
  radioHSPI.setChannel(channel);
}

void jamBluetooth() {
  int randomIndex = random(0, sizeof(bluetooth_channels) / sizeof(bluetooth_channels[0]));
  int channel = bluetooth_channels[randomIndex];
  radioVSPI.setChannel(channel);
  radioHSPI.setChannel(channel);
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
            //radioVSPI.powerDown();
            //radioHSPI.powerDown();
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

void loop() {
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
