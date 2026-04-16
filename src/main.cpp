// =============================================================================
//  The Jester — Waveshare ESP32-S3-Touch-LCD-3.5-C
//  Board   : ESP32-S3R8, 16MB Flash, 8MB OPI PSRAM
//  Display : ST7796 SPI 480×320 landscape via GFX Library for Arduino + TCA9554
//  Touch   : FT6336 I2C (SDA=GPIO8, SCL=GPIO7)
//  Radio 1 : NRF24L01+PA+LNA  CE=GPIO9  CSN=GPIO10  (BLE jammer)
//  Radio 2 : NRF24L01+PA+LNA  CE=GPIO11 CSN=GPIO46  (Bluetooth jammer)
//  SPI bus : HSPI  CLK=GPIO17  MOSI=GPIO18  MISO=GPIO21  (both radios)
//  NOTE: Add 220µF cap on each radio's VCC pin.
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include "RF24.h"
#include "esp_bt.h"
#include "esp_wifi.h"
#include "Preferences.h"
#include <Arduino_GFX_Library.h>
#include "TCA9554.h"

// ---------------------------------------------------------------------------
// LCD hardware pins  (SPI2/FSPI — internal, NOT on the header)
// ---------------------------------------------------------------------------
#define LCD_SCLK   5
#define LCD_MOSI   1
#define LCD_MISO   2
#define LCD_CS    -1   // not wired on this board
#define LCD_DC     3
#define LCD_BL     6   // fallback backlight GPIO (also driven by TCA P0)

// TCA9554 IO expander (I2C 0x20) — P0=backlight, P1=LCD reset
#define I2C_SDA    8
#define I2C_SCL    7
#define TCA_BL_PIN       0   // TCA9554 P0 — backlight enable
#define TCA_LCD_RST_PIN  1   // TCA9554 P1 — LCD reset

// Touch controller FT6336 I2C address
#define FT6336_ADDR   0x38

// ---------------------------------------------------------------------------
// NRF24L01 SPI bus (HSPI — shared SCK/MOSI/MISO, split via splitters)
// ---------------------------------------------------------------------------
#define NRF_CLK   17   // grey  — header pin 14 (split to both radios)
#define NRF_MOSI  18   // yellow — header pin 16 (split to both radios)
#define NRF_MISO  21   // purple — header pin 5  (split to both radios)
#define NRF_CE_A   9   // white  — header pin 12 (Radio A)
#define NRF_CSN_A 10   // orange — header pin 10 (Radio A)
#define NRF_CE_B  38   // white  — header pin 7  (Radio B)
#define NRF_CSN_B 46   // orange — header pin 19 (Radio B)

constexpr int SPI_SPEED = 16000000;

// ---------------------------------------------------------------------------
// Display — landscape 480×320
// ---------------------------------------------------------------------------
TCA9554 tca(0x20);

Arduino_DataBus *bus = new Arduino_ESP32SPI(
    LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, LCD_MISO,
    FSPI   // SPI2 host — dedicated to the LCD
);
// rotation=1 → landscape; IPS=true for ST7796 on this board
Arduino_GFX *gfx = new Arduino_ST7796(bus, GFX_NOT_DEFINED, 1 /*rotation*/, true);

// ---------------------------------------------------------------------------
// Colour palette
// ---------------------------------------------------------------------------
#define COL_BG      0x1082u  // near-black
#define COL_TITLE   0xFFFFu  // white
#define COL_OFF     0x8410u  // grey
#define COL_BLE     0x041Fu  // blue
#define COL_BT      0xF800u  // red
#define COL_BOTH    0xFC00u  // orange
#define COL_ACTIVE  0xFFE0u  // yellow highlight border

// ---------------------------------------------------------------------------
// Radio
// ---------------------------------------------------------------------------
SPIClass spiHSPI(HSPI);
RF24 radioA(NRF_CE_A, NRF_CSN_A, SPI_SPEED);
RF24 radioB(NRF_CE_B, NRF_CSN_B, SPI_SPEED);

int bluetooth_channels[] = {32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80};
int ble_channels[]        = {2, 26, 80};

// ---------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------
enum Mode { OFF, BLUETOOTH, BLE, BOTH };
Mode currentMode = OFF;

String inputString = "";

Preferences preferences;

// ---------------------------------------------------------------------------
// UI layout — 4 buttons in a 2×2 grid, landscape 480×320
// ---------------------------------------------------------------------------
//  Title bar : 0  → 39  (40 px tall)
//  Button row 1 : 40 → 179 (140 px tall)
//  Button row 2 : 180 → 319 (140 px tall)
//  Each col 240 px wide → 4 buttons total (2 cols × 2 rows)
#define TITLE_H   40
#define BTN_W    240
#define BTN_H    140
#define BTN_BORDER 4

struct Button {
  int x, y, w, h;
  uint16_t color;
  const char *label;
  Mode mode;
};

static const Button buttons[4] = {
  {0,            TITLE_H,          BTN_W, BTN_H, COL_OFF,  "OFF",       OFF},
  {BTN_W,        TITLE_H,          BTN_W, BTN_H, COL_BLE,  "BLE",       BLE},
  {0,            TITLE_H + BTN_H,  BTN_W, BTN_H, COL_BT,   "BLUETOOTH", BLUETOOTH},
  {BTN_W,        TITLE_H + BTN_H,  BTN_W, BTN_H, COL_BOTH, "BOTH",      BOTH},
};

// ---------------------------------------------------------------------------
// Touch state
// ---------------------------------------------------------------------------
unsigned long lastTouchMs = 0;
#define TOUCH_DEBOUNCE_MS 300

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
String getModeString(Mode mode) {
  switch (mode) {
    case OFF:        return "OFF";
    case BLUETOOTH:  return "BLUETOOTH";
    case BLE:        return "BLE";
    case BOTH:       return "BOTH";
    default:         return "UNKNOWN";
  }
}

void saveDefaultMode(Mode mode) {
  preferences.begin("rfclown", false);
  preferences.putUChar("mode", (uint8_t)mode);
  preferences.end();
  Serial.println("Default mode saved: " + getModeString(mode));
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

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------
void drawTitleBar() {
  gfx->fillRect(0, 0, 480, TITLE_H, COL_BG);
  gfx->setTextColor(COL_TITLE);
  gfx->setTextSize(2);
  // Centre "THE JESTER" in the title bar
  const char *title = "THE JESTER";
  int tw = strlen(title) * 12;  // ~12px per char at size 2
  gfx->setCursor((480 - tw) / 2, (TITLE_H - 16) / 2);
  gfx->print(title);
}

void drawButton(int idx, bool active) {
  const Button &b = buttons[idx];
  uint16_t border = active ? COL_ACTIVE : COL_BG;
  uint16_t fill   = active ? b.color : (uint16_t)(b.color >> 1) & 0x7BEF;

  gfx->fillRect(b.x + BTN_BORDER, b.y + BTN_BORDER,
                b.w - 2*BTN_BORDER, b.h - 2*BTN_BORDER, fill);
  gfx->drawRect(b.x, b.y, b.w, b.h, border);
  if (active) {
    gfx->drawRect(b.x+1, b.y+1, b.w-2, b.h-2, border);
    gfx->drawRect(b.x+2, b.y+2, b.w-4, b.h-4, border);
  }

  // Label
  gfx->setTextColor(COL_TITLE);
  gfx->setTextSize(2);
  int tw = strlen(b.label) * 12;
  int tx = b.x + (b.w - tw) / 2;
  int ty = b.y + (b.h - 16) / 2;
  gfx->setCursor(tx, ty);
  gfx->print(b.label);
}

void drawUI() {
  gfx->fillScreen(COL_BG);
  drawTitleBar();
  for (int i = 0; i < 4; i++) {
    drawButton(i, buttons[i].mode == currentMode);
  }
}

// ---------------------------------------------------------------------------
// Radio config
// ---------------------------------------------------------------------------
void configureRadio(RF24 &radio, int channel) {
  if (radio.begin(&spiHSPI)) {
    radio.setAutoAck(false);
    radio.stopListening();
    radio.setRetries(0, 0);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
    radio.setCRCLength(RF24_CRC_DISABLED);
    radio.startConstCarrier(RF24_PA_HIGH, channel);
    Serial.println("Radio OK");
  } else {
    Serial.println("Radio FAIL — check wiring & 220uF cap");
  }
}

// ---------------------------------------------------------------------------
// Mode control
// ---------------------------------------------------------------------------
void activateMode(Mode mode) {
  currentMode = mode;
  Serial.println("Mode: " + getModeString(mode));
  drawUI();
}

// ---------------------------------------------------------------------------
// Jamming
// ---------------------------------------------------------------------------
void jamBLE() {
  int ch = ble_channels[random(0, sizeof(ble_channels)/sizeof(ble_channels[0]))];
  radioA.setChannel(ch);
  radioB.setChannel(ch);
}

void jamBluetooth() {
  int ch = bluetooth_channels[random(0, sizeof(bluetooth_channels)/sizeof(bluetooth_channels[0]))];
  radioA.setChannel(ch);
  radioB.setChannel(ch);
}

void jamAll() {
  if (random(0, 2)) jamBluetooth();
  else              jamBLE();
}

void executeMode() {
  switch (currentMode) {
    case OFF:        delay(100);    break;
    case BLE:        jamBLE();      break;
    case BLUETOOTH:  jamBluetooth(); break;
    case BOTH:       jamAll();      break;
  }
}

// ---------------------------------------------------------------------------
// Touch — FT6336 raw I2C read, adapted for landscape rotation=1
// landscape: raw touch X maps to screen Y, raw touch Y maps to screen X (mirrored)
// ---------------------------------------------------------------------------
bool readTouch(int16_t &sx, int16_t &sy) {
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(0x02);  // status register
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(FT6336_ADDR, 6);
  if (Wire.available() < 6) return false;

  uint8_t stat = Wire.read();
  if ((stat & 0x0F) == 0) return false;  // no touch points

  uint8_t xh = Wire.read();
  uint8_t xl = Wire.read();
  uint8_t yh = Wire.read();
  uint8_t yl = Wire.read();
  Wire.read();  // weight

  int16_t raw_x = ((xh & 0x0F) << 8) | xl;  // 0–479 in portrait (width axis)
  int16_t raw_y = ((yh & 0x0F) << 8) | yl;  // 0–319 in portrait (height axis)

  // rotation=1 (landscape): screen X = raw_x, screen Y = (319 - raw_y)
  sx = raw_x;
  sy = 319 - raw_y;
  return true;
}

void handleTouch() {
  unsigned long now = millis();
  if (now - lastTouchMs < TOUCH_DEBOUNCE_MS) return;

  int16_t tx, ty;
  if (!readTouch(tx, ty)) return;

  lastTouchMs = now;

  for (int i = 0; i < 4; i++) {
    const Button &b = buttons[i];
    if (tx >= b.x && tx < b.x + b.w && ty >= b.y && ty < b.y + b.h) {
      if (buttons[i].mode != currentMode) {
        activateMode(buttons[i].mode);
      }
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Serial command handler (kept for live demo / diagnostics)
// ---------------------------------------------------------------------------
void handleCommand() {
  inputString.trim();
  inputString.toLowerCase();

  if (inputString == "help") {
    Serial.println(
      "mode:<mode>    - set mode: off, bluetooth, ble, both\n"
      "default:<mode> - save default mode"
    );
  } else if (inputString == "mode:off")        { activateMode(OFF); }
  else if (inputString == "mode:bluetooth")    { activateMode(BLUETOOTH); }
  else if (inputString == "mode:ble")          { activateMode(BLE); }
  else if (inputString == "mode:both")         { activateMode(BOTH); }
  else if (inputString == "default:off")       { saveDefaultMode(OFF); }
  else if (inputString == "default:bluetooth") { saveDefaultMode(BLUETOOTH); }
  else if (inputString == "default:ble")       { saveDefaultMode(BLE); }
  else if (inputString == "default:both")      { saveDefaultMode(BOTH); }
  else {
    Serial.print("Unknown command: ");
    Serial.println(inputString);
  }

  inputString = "";
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  // 1. I2C for TCA9554 IO expander + FT6336 touch
  Wire.begin(I2C_SDA, I2C_SCL);

  // 2. Init TCA9554 at 0x20
  tca.begin();

  // 3. Set P0 (backlight) and P1 (LCD reset) as outputs
  tca.pinMode1(TCA_BL_PIN, OUTPUT);
  tca.pinMode1(TCA_LCD_RST_PIN, OUTPUT);

  // 4. Backlight on — TCA P0 HIGH + fallback GPIO6 HIGH
  tca.write1(TCA_BL_PIN, 1);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  // 5. LCD reset pulse: HIGH → LOW → HIGH
  tca.write1(TCA_LCD_RST_PIN, 1);
  delay(10);
  tca.write1(TCA_LCD_RST_PIN, 0);
  delay(10);
  tca.write1(TCA_LCD_RST_PIN, 1);
  delay(120);

  // 6. Display init (SPI2/FSPI, ST7796, landscape)
  if (!gfx->begin()) {
    Serial.println("Display init FAILED");
  }

  // Load saved mode and draw UI
  currentMode = loadDefaultMode();
  drawUI();

  // Disable onboard BT/WiFi radios (not needed, frees RF for NRF24)
  esp_bt_controller_deinit();
  esp_wifi_stop();
  esp_wifi_deinit();
  esp_wifi_disconnect();

  // NRF24 SPI bus — HSPI shared by both radios (split grey/yellow/purple wires)
  spiHSPI.begin(NRF_CLK, NRF_MISO, NRF_MOSI, -1);

  configureRadio(radioA, ble_channels[0]);        // Radio A → BLE
  configureRadio(radioB, bluetooth_channels[0]);  // Radio B → Bluetooth

  Serial.println("The Jester — Waveshare ESP32-S3-Touch-LCD-3.5-C");
  Serial.println("Mode: " + getModeString(currentMode));
  sendCurrentMode();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  handleTouch();
  executeMode();
}

// ---------------------------------------------------------------------------
// Serial input (fallback commands)
// ---------------------------------------------------------------------------
void serialEvent() {
  while (Serial.available()) {
    char inChar = (char)Serial.read();
    if (inChar == '\n') {
      if (inputString.length() > 0) handleCommand();
    } else {
      inputString += inChar;
    }
  }
}
