// =============================================================================
//  The Gizmo — Waveshare ESP32-S3-Touch-LCD-3.5-C
//  Board   : ESP32-S3R8, 16MB Flash, 8MB OPI PSRAM
//  Display : ST7796 SPI 480×320 landscape via GFX Library for Arduino + TCA9554
//  Touch   : FT6336 I2C (SDA=GPIO8, SCL=GPIO7)
//  Radio A : NRF24L01+PA+LNA  CE=GPIO9  CSN=GPIO10  (left dot)
//  Radio B : NRF24L01+PA+LNA  CE=GPIO38 CSN=GPIO47  (right dot)
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
#define NRF_CE_A   9   // white  — top row header (Radio A)
#define NRF_CSN_A 10   // orange — top row header (Radio A)
#define NRF_CE_B  38   // white  — header pin 7  (Radio B)
#define NRF_CSN_B 47   // orange — header pin 20 (Radio B) — GPIO46 is strapping pin, avoid

constexpr int SPI_SPEED = 8000000;  // 8 MHz — NRF24 supports up to 10 MHz

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
#define COL_WIFI    0x07FFu  // cyan — WiFi jammer
#define COL_BLE     0x041Fu  // blue
#define COL_BT      0xF800u  // red
#define COL_JAM     0xFC00u  // orange — JAM TIME
#define COL_ACTIVE  0xFFE0u  // yellow highlight border
#define COL_GREEN   0x07E0u  // radio OK
#define COL_RED     0xF800u  // radio FAIL

// ---------------------------------------------------------------------------
// Radio
// ---------------------------------------------------------------------------
bool radioAok = false;
bool radioBok = false;
SPIClass spiHSPI(HSPI);
RF24 radioA(NRF_CE_A, NRF_CSN_A, SPI_SPEED);
RF24 radioB(NRF_CE_B, NRF_CSN_B, SPI_SPEED);

// Classic Bluetooth sweep counters (NRF24 ch 2–80)
static uint8_t btSweepA = 2;
static uint8_t btSweepB = 80;

// ---------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------
enum Mode { OFF, WIFI, BLUETOOTH, BLE, JAMTIME };
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
  {0,            TITLE_H,          BTN_W, BTN_H, COL_WIFI, "WIFI 2.4",  WIFI},
  {BTN_W,        TITLE_H,          BTN_W, BTN_H, COL_BLE,  "BLE",       BLE},
  {0,            TITLE_H + BTN_H,  BTN_W, BTN_H, COL_BT,   "BLUETOOTH", BLUETOOTH},
  {BTN_W,        TITLE_H + BTN_H,  BTN_W, BTN_H, COL_JAM,  "JAM TIME",  JAMTIME},
};

// ---------------------------------------------------------------------------
// Touch state
// ---------------------------------------------------------------------------
unsigned long lastTouchMs = 0;
#define TOUCH_DEBOUNCE_MS 300
unsigned long lastHealthMs = 0;
#define HEALTH_CHECK_MS 2000  // re-check radio connectivity every 2s

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
String getModeString(Mode mode) {
  switch (mode) {
    case OFF:        return "OFF";
    case WIFI:       return "WIFI";
    case BLUETOOTH:  return "BLUETOOTH";
    case BLE:        return "BLE";
    case JAMTIME:   return "JAMTIME";
    default:         return "UNKNOWN";
  }
}

void saveDefaultMode(Mode mode) {
  preferences.begin("rfclown", false);
  preferences.putUChar("mode", (uint8_t)mode);
  preferences.end();
}

Mode loadDefaultMode() {
  preferences.begin("rfclown", false);
  uint8_t mode = preferences.getUChar("mode", (uint8_t)OFF);
  preferences.end();
  return (Mode)mode;
}

void sendCurrentMode() { }

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------
void drawStatusDots() {
  gfx->fillCircle(440, 20, 8, radioAok ? COL_GREEN : COL_RED);
  gfx->fillCircle(462, 20, 8, radioBok ? COL_GREEN : COL_RED);
}

void drawTitleBar() {
  gfx->fillRect(0, 0, 480, TITLE_H, COL_BG);
  gfx->setTextColor(COL_TITLE);
  gfx->setTextSize(2);
  const char *title = "THE GIZMO";
  int tw = strlen(title) * 12;
  gfx->setCursor((480 - tw) / 2, (TITLE_H - 16) / 2);
  gfx->print(title);
  drawStatusDots();
}

void drawButton(int idx, bool active) {
  const Button &b = buttons[idx];
  uint16_t border = active ? COL_ACTIVE : COL_RED;
  uint16_t fill   = active ? b.color : (uint16_t)(b.color >> 1) & 0x7BEF;

  gfx->fillRect(b.x + BTN_BORDER, b.y + BTN_BORDER,
                b.w - 2*BTN_BORDER, b.h - 2*BTN_BORDER, fill);
  gfx->drawRect(b.x, b.y, b.w, b.h, border);
  gfx->drawRect(b.x+1, b.y+1, b.w-2, b.h-2, border);
  gfx->drawRect(b.x+2, b.y+2, b.w-4, b.h-4, border);

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

// Redraw only the buttons (no full screen clear) to avoid flicker
void refreshButtons() {
  for (int i = 0; i < 4; i++) {
    drawButton(i, buttons[i].mode == currentMode);
  }
}

// ---------------------------------------------------------------------------
// Radio config
// ---------------------------------------------------------------------------
// Noise payload — 32 bytes of random data, refreshed periodically
uint8_t noisePayload[32];

void refreshNoise() {
  for (int i = 0; i < 32; i++) noisePayload[i] = random(256);
}

// CW tone on one channel — diagnostic-only. startConstCarrier emits an
// unmodulated carrier (single frequency, ~0 Hz wide) which does not
// effectively disrupt wideband OFDM like WiFi. Use spamChannel for jamming.
void cwOnChannel(RF24 &radio, uint8_t ch) {
  radio.stopConstCarrier();
  radio.startConstCarrier(RF24_PA_MAX, ch);
}

// Modulated noise burst: transmit N non-ACK packets of random payload on `ch`.
// At 2 Mbps + 32-byte payload each packet is ~140 µs on-air and occupies
// ~1-2 MHz of GFSK-modulated bandwidth — looks like real noise to a WiFi/BT
// receiver, unlike a CW tone.
void spamChannel(RF24 &radio, uint8_t ch, uint8_t packets = 4) {
  radio.stopConstCarrier();
  radio.setChannel(ch);
  for (uint8_t i = 0; i < packets; i++) {
    for (int j = 0; j < 8; j++) noisePayload[j] = random(256);
    radio.writeFast(noisePayload, 32, true);  // multicast=true → no ACK wait
  }
}

bool configureRadio(RF24 &radio) {
  if (radio.begin(&spiHSPI)) {
    radio.setAutoAck(false);
    radio.stopListening();
    radio.setRetries(0, 0);
    radio.setPALevel(RF24_PA_MAX, true);
    radio.setDataRate(RF24_2MBPS);
    radio.setCRCLength(RF24_CRC_DISABLED);
    radio.setPayloadSize(32);
    // Open a writing pipe — address doesn't matter, we're blasting noise
    radio.openWritingPipe((const uint8_t *)"\xE7\xE7\xE7\xE7\xE7");
    Serial.println("Radio OK");
    return true;
  } else {
    Serial.println("Radio FAIL — check wiring & 220uF cap");
    return false;
  }
}

// ---------------------------------------------------------------------------
// Mode control
// ---------------------------------------------------------------------------
void activateMode(Mode mode) {
  // Always silence the carrier on mode change. The new mode (if any) will rekey on its first jam call.
  if (radioAok) radioA.stopConstCarrier();
  if (radioBok) radioB.stopConstCarrier();
  currentMode = mode;
  refreshButtons();
}

// ---------------------------------------------------------------------------
// Jamming — packet-based noise for real wideband interference
// Each call blasts 3×32-byte noise packets per radio per channel hop.
// 2MBPS + 32-byte payload → each packet occupies ~2 MHz for ~140 µs.
// ---------------------------------------------------------------------------

// WiFi 2.4 GHz US channel centers: 1=NRF24 ch 2, 6=27, 11=52.
// Radio A pins a heavy burst on one center; Radio B sweeps the other two.
// Rotate the primary each call so all three centers get heavy dwell over time.
static const uint8_t wifiCenters[] = {2, 27, 52};
static uint8_t wifiPrimaryIdx = 0;

void jamWifi() {
  if (radioAok) spamChannel(radioA, wifiCenters[wifiPrimaryIdx], 15);
  if (radioBok) {
    spamChannel(radioB, wifiCenters[(wifiPrimaryIdx + 1) % 3], 8);
    spamChannel(radioB, wifiCenters[(wifiPrimaryIdx + 2) % 3], 8);
  }
  wifiPrimaryIdx = (wifiPrimaryIdx + 1) % 3;
}

// BLE: 3 advertising channels (2, 26, 80) + 37 data channels (4–78)
// Both radios active: one hammers advertising, other sweeps data, then swap
static uint8_t bleSweepAdv = 0;
static uint8_t bleSweepData = 4;
static const uint8_t bleAdvCh[] = {2, 26, 80};
static bool bleSwapRadios = false;  // alternate which radio does what

void jamBLE() {
  RF24 *advRadio  = bleSwapRadios ? &radioB : &radioA;
  RF24 *dataRadio = bleSwapRadios ? &radioA : &radioB;
  bool advOk  = bleSwapRadios ? radioBok : radioAok;
  bool dataOk = bleSwapRadios ? radioAok : radioBok;

  // Hammer advertising channel — heavy packet burst
  if (advOk) spamChannel(*advRadio, bleAdvCh[bleSweepAdv], 6);
  bleSweepAdv = (bleSweepAdv + 1) % 3;

  // Sweep data channels — all 37 BLE data channels (NRF24 ch 4–78)
  if (dataOk) spamChannel(*dataRadio, bleSweepData, 2);
  bleSweepData++;
  if (bleSweepData > 78) {
    bleSweepData = 4;
    bleSwapRadios = !bleSwapRadios;  // swap roles each full sweep
  }
}

// Classic Bluetooth: 79 channels (NRF24 ch 2–80)
// Both radios sweep from opposite ends
void jamBluetooth() {
  if (radioAok) spamChannel(radioA, btSweepA, 3);
  if (radioBok) spamChannel(radioB, btSweepB, 3);
  btSweepA = (btSweepA >= 80) ? 2 : btSweepA + 1;
  btSweepB = (btSweepB <= 2) ? 80 : btSweepB - 1;
}

// ---------------------------------------------------------------------------
// JAM TIME — Brute Force Full-Band Saturator
// Both radios blast from opposite ends, 12 packets per channel per pass.
// In a 12ft room at PA_MAX this will saturate the entire 2.4 GHz band.
// No scanning, no wasted RX time — 100% transmit duty cycle.
// ---------------------------------------------------------------------------
static uint8_t jamSweepA = 0;
static uint8_t jamSweepB = 125;

void jamAll() {
  if (radioAok) spamChannel(radioA, jamSweepA, 2);
  if (radioBok) spamChannel(radioB, jamSweepB, 2);
  jamSweepA = (jamSweepA + 1) % 126;
  jamSweepB = (jamSweepB == 0) ? 125 : jamSweepB - 1;
}

void executeMode() {
  switch (currentMode) {
    case OFF:        delay(100);    break;
    case WIFI:       jamWifi();     break;
    case BLE:        jamBLE();      break;
    case BLUETOOTH:  jamBluetooth(); break;
    case JAMTIME:    jamAll();      break;
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

  int16_t raw_x = ((xh & 0x0F) << 8) | xl;  // 0–319 (portrait width axis)
  int16_t raw_y = ((yh & 0x0F) << 8) | yl;  // 0–479 (portrait height axis)

  // rotation=1 landscape: portrait height axis becomes screen X (0–479)
  //                        portrait width axis becomes screen Y (0–319, inverted)
  sx = raw_y;
  sy = 319 - raw_x;
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
      if (buttons[i].mode == currentMode) {
        activateMode(OFF);  // toggle off on second tap
      } else {
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
    ;
  } else if (inputString == "mode:off")        { activateMode(OFF); }
  else if (inputString == "mode:wifi")         { activateMode(WIFI); }
  else if (inputString == "mode:bluetooth")    { activateMode(BLUETOOTH); }
  else if (inputString == "mode:ble")          { activateMode(BLE); }
  else if (inputString == "mode:jamtime")      { activateMode(JAMTIME); }
  else if (inputString == "default:off")       { saveDefaultMode(OFF); }
  else if (inputString == "default:wifi")      { saveDefaultMode(WIFI); }
  else if (inputString == "default:bluetooth") { saveDefaultMode(BLUETOOTH); }
  else if (inputString == "default:ble")       { saveDefaultMode(BLE); }
  else if (inputString == "default:jamtime")   { saveDefaultMode(JAMTIME); }
  else if (inputString.startsWith("diag:")) {
    int ch = inputString.substring(5).toInt();
    if (ch >= 0 && ch <= 125) {
      activateMode(OFF);
      if (radioAok) cwOnChannel(radioA, (uint8_t)ch);
      Serial.printf("DIAG: Radio A CW on ch %d (%d MHz)\n", ch, 2400 + ch);
    }
  }
  else if (inputString.startsWith("diagb:")) {
    int ch = inputString.substring(6).toInt();
    if (ch >= 0 && ch <= 125) {
      activateMode(OFF);
      if (radioBok) cwOnChannel(radioB, (uint8_t)ch);
      Serial.printf("DIAG: Radio B CW on ch %d (%d MHz)\n", ch, 2400 + ch);
    }
  }
  else {
    ;
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

  // Seed noise payload for jamming
  randomSeed(esp_random());
  refreshNoise();

  // =========================================================================
  //  Boot splash — neon cyberlab style
  // =========================================================================
  gfx->fillScreen(0x0000);  // pure black

  // Neon green scanlines background effect
  for (int y = 0; y < 320; y += 4) {
    gfx->drawFastHLine(0, y, 480, 0x0280);  // dark green scanlines
  }

  // Outer neon border — double-line cyan glow
  for (int i = 0; i < 3; i++) {
    gfx->drawRect(4 + i, 4 + i, 472 - 2*i, 312 - 2*i, 0x07FF);  // cyan
  }
  gfx->drawRect(8, 8, 464, 304, 0x03EF);  // dimmer inner cyan

  // "BuggedLab Productions" — large neon magenta
  gfx->setTextSize(3);
  const char *studio = "BuggedLab";
  int sw = strlen(studio) * 18;
  gfx->setTextColor(0xF81F);  // magenta
  gfx->setCursor((480 - sw) / 2, 90);
  gfx->print(studio);

  const char *prod = "Productions";
  int pw = strlen(prod) * 18;
  gfx->setCursor((480 - pw) / 2, 125);
  gfx->print(prod);

  // "THE GIZMO" — big neon cyan title
  gfx->setTextSize(4);
  const char *gizmo = "THE GIZMO";
  int jw = strlen(gizmo) * 24;
  gfx->setTextColor(0x07FF);  // cyan
  gfx->setCursor((480 - jw) / 2, 180);
  gfx->print(gizmo);

  // Version / tagline — small neon green
  gfx->setTextSize(1);
  gfx->setTextColor(0x07E0);  // green
  const char *ver = "v1.0  //  2.4 GHz RF Platform";
  int vw = strlen(ver) * 6;
  gfx->setCursor((480 - vw) / 2, 240);
  gfx->print(ver);

  // Animated loading bar — neon magenta fill
  int barX = 100, barY = 270, barW = 280, barH = 16;
  gfx->drawRect(barX - 1, barY - 1, barW + 2, barH + 2, 0x07FF);  // cyan border
  for (int i = 0; i < barW; i += 2) {
    gfx->fillRect(barX, barY, i, barH, 0xF81F);  // magenta fill
    delay(8);
  }
  gfx->fillRect(barX, barY, barW, barH, 0xF81F);

  delay(600);  // hold splash

  // =========================================================================
  //  End splash — switch to main UI
  // =========================================================================

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

  Serial.println("Initializing Radio A (CE=" + String(NRF_CE_A) + " CSN=" + String(NRF_CSN_A) + ")...");
  radioAok = configureRadio(radioA);  // Radio A
  Serial.println("Radio A: " + String(radioAok ? "OK" : "FAIL"));
  delay(10);
  Serial.println("Initializing Radio B (CE=" + String(NRF_CE_B) + " CSN=" + String(NRF_CSN_B) + ")...");
  radioBok = configureRadio(radioB);  // Radio B
  Serial.println("Radio B: " + String(radioBok ? "OK" : "FAIL"));
  drawUI();  // redraw to update status dots

  Serial.println("The Gizmo — Waveshare ESP32-S3-Touch-LCD-3.5-C");
  Serial.println("Mode: " + getModeString(currentMode));
  sendCurrentMode();
}

// ---------------------------------------------------------------------------
// Radio health check — periodic re-verification
// ---------------------------------------------------------------------------
void checkRadioHealth() {
  unsigned long now = millis();
  if (now - lastHealthMs < HEALTH_CHECK_MS) return;
  lastHealthMs = now;

  bool prevA = radioAok;
  bool prevB = radioBok;
  radioAok = radioA.isChipConnected();
  radioBok = radioB.isChipConnected();

  if (radioAok != prevA || radioBok != prevB) {
    drawStatusDots();
  }
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void serialEvent();  // forward decl — defined below

void loop() {
  handleTouch();
  executeMode();
  checkRadioHealth();
  serialEvent();  // ESP32 Arduino doesn't auto-call this; do it explicitly
}

// ---------------------------------------------------------------------------
// Serial input (fallback commands — typed at 115200, ends with newline)
// ---------------------------------------------------------------------------
void serialEvent() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputString.length() > 0) handleCommand();
    } else {
      inputString += c;
    }
  }
}

