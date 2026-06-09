// =============================================================================
//  The Gizmo — Waveshare ESP32-S3-Touch-LCD-3.5-C
//  Board   : ESP32-S3R8, 16MB Flash, 8MB OPI PSRAM
//  Display : ST7796 SPI 480×320 landscape via GFX Library for Arduino + TCA9554
//  Touch   : FT6336 I2C (SDA=GPIO8, SCL=GPIO7)
//  Radio A : NRF24L01+PA+LNA  CE=GPIO19 CSN=GPIO20  SCK=GPIO11 MOSI=GPIO10 MISO=GPIO9
//  Radio B : NRF24L01+PA+LNA  CE=GPIO38 CSN=GPIO39  SCK=GPIO40 MOSI=GPIO41 MISO=GPIO42
//  SPI host: HSPI remapped per access so each radio has its own dedicated header pins
//  NOTE: Add 220µF cap on each radio's VCC pin.
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include "RF24.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
// esp_gap_bt_api.h (Classic BT GAP) omitted — Arduino ESP32-S3 framework
// is built without CONFIG_BT_CLASSIC_ENABLED; Classic BT inquiry requires a
// custom sdkconfig build. BT energy is still detected via NRF24 RPD on ch 2-80.
#include "esp_wifi.h"
#include "Preferences.h"
#include <Arduino_GFX_Library.h>
#include "TCA9554.h"
#include "XPowersLib.h"
#include "SD_MMC.h"
#include <FS.h>

XPowersAXP2101 PMU;
bool pmuOk = false;

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
// NRF24L01 SPI bus
// Radio A uses a top-row-only plug-in group: 19, 20, 11, 10, 9.
// Radio B uses a bottom-row-only plug-in group: 38, 39, 40, 41, 42.
// The code remaps the HSPI host before each access so the radios do not
// share signal wires on the board header.
// ---------------------------------------------------------------------------
#define NRF_CE_A   19
#define NRF_CSN_A  20
#define NRF_CLK_A  11
#define NRF_MOSI_A 10
#define NRF_MISO_A  9

#define NRF_CE_B   38
#define NRF_CSN_B  39
#define NRF_CLK_B  40
#define NRF_MOSI_B 41
#define NRF_MISO_B 42

constexpr int SPI_SPEED = 8000000;  // 8 MHz — NRF24 supports up to 10 MHz

// ---------------------------------------------------------------------------
// SD card (TF slot) — SDMMC 1-bit mode (board schematic: CLK=11, CMD=10, D0=9)
// These GPIOs are SHARED with NRF Radio A (CLK_A=11, MOSI_A=10, MISO_A=9).
// The firmware time-slices the pins: SD_MMC is mounted/unmounted around
// Radio A usage so both work correctly without hardware changes.
#define SD_CLK  11   // shared with NRF_CLK_A
#define SD_CMD  10   // shared with NRF_MOSI_A
#define SD_D0    9   // shared with NRF_MISO_A
bool sdOk = false;

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
#define COL_BG       0x1082u  // near-black
#define COL_TITLE    0xFFFFu  // white
#define COL_WIFI     0x07FFu  // cyan — WiFi jammer
#define COL_BLE      0xC01Fu  // violet (matches BLE accent in jam visualizer)
#define COL_BT       0xF800u  // red
#define COL_JAM      0xFC00u  // orange — JAM TIME
#define COL_ACTIVE   0xFFE0u  // yellow highlight border
#define COL_GREEN    0x07E0u  // radio OK
#define COL_RED      0xF800u  // radio FAIL
#define COL_SPECTRUM  0xB81Fu  // bright violet — spectrum analyzer
#define COL_NETSCAN   0xEFE0u  // bright yellow-green — network scanner
#define COL_WARDRIVE  0x07E3u  // lime-green — wardrive
#define COL_LOGS      0xFCC0u  // amber — logs viewer
#define COL_LABEL    0xC618u  // light gray — readable labels
#define COL_SECONDARY 0xAD75u // silver — secondary text, MACs
#define COL_DIVIDER  0x4A49u  // medium gray — separator lines

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

// Mini spectrum state for active jam buttons (WIFI / BLE / BLUETOOTH / JAMTIME)
static uint8_t  jamSpectrum[126]     = {};
static uint8_t  jamSpectrumPrev[126] = {};
static unsigned long lastJamScanMs   = 0;
#define JAM_SCAN_INTERVAL_MS  80

// Spectrum scanner state (SPECTRUM mode — full screen, with peak hold)
uint8_t gSpectrum[126]         = {0};
uint8_t gSpectrumPrev[126]     = {0};
uint8_t gSpectrumPeak[126]     = {0};
uint8_t gSpectrumPeakPrev[126] = {0};
// Spectrum state for NETSCAN combined mode (top-half chart, no peak hold)
uint8_t gComboSpectrum[126]    = {0};
uint8_t gComboSpectrumPrev[126]= {0};

// ---------------------------------------------------------------------------
// Mode
// ---------------------------------------------------------------------------
enum Mode { OFF, WIFI, BLUETOOTH, BLE, JAMTIME, SPECTRUM, NETSCAN, WARDRIVE, LOGS };
Mode currentMode = OFF;

String inputString = "";

Preferences preferences;
using RadioSelectFn = void (*)();

// Forward declarations
void drawSpectrumUI();
void runSpectrum();
void drawNetScanUI();
void runNetScan();

// ---------------------------------------------------------------------------
// UI layout — 6 buttons in a 2×3 grid, landscape 480×320
// ---------------------------------------------------------------------------
//  Title bar   :   0 →  39  (40 px tall)
//  Button row 1:  40 → 179 (140 px tall)  — 3 cols × 160 px wide
//  Button row 2: 180 → 319 (140 px tall)  — 3 cols × 160 px wide
#define TITLE_H    40
#define BTN_W     160   // 480 / 3
#define BTN_H     140   // (320 - 40) / 2
#define BTN_BORDER  4

struct Button {
  int x, y, w, h;
  uint16_t color;
  const char *label;
  Mode mode;
};

// Page 0: 6 core mode buttons  |  Page 1: WARDRIVE + LOGS
// Buttons 6-7 share the same screen positions as 0-1 but on page 1.
static Button buttons[8] = {
  {0,       TITLE_H,         BTN_W, BTN_H, COL_WIFI,     "WIFI 2.4",  WIFI},
  {BTN_W,   TITLE_H,         BTN_W, BTN_H, COL_BLE,      "BLE",       BLE},
  {2*BTN_W, TITLE_H,         BTN_W, BTN_H, COL_BT,       "BLUETOOTH", BLUETOOTH},
  {0,       TITLE_H + BTN_H, BTN_W, BTN_H, COL_JAM,      "JAM TIME",  JAMTIME},
  {BTN_W,   TITLE_H + BTN_H, BTN_W, BTN_H, COL_SPECTRUM, "SPECTRUM",  SPECTRUM},
  {2*BTN_W, TITLE_H + BTN_H, BTN_W, BTN_H, COL_NETSCAN,  "NET SCAN",  NETSCAN},
  // Page 1 — same screen coords as buttons 0 and 1
  {0,       TITLE_H,         BTN_W, BTN_H, COL_WARDRIVE, "WARDRIVE",  WARDRIVE},
  {BTN_W,   TITLE_H,         BTN_W, BTN_H, COL_LOGS,     "LOGS",      LOGS},
};
static int uiPage = 0;  // 0 = main grid, 1 = extended (wardrive/logs)

// ---------------------------------------------------------------------------
// Wardrive — device tables, scan state, SD logging
// ---------------------------------------------------------------------------
#define WD_MAX_WIFI          30
#define WD_MAX_BLE           50
#define WD_MAX_BT            20
#define WD_LOG_THRESHOLD      3   // seenCount at which a device is logged to SD
#define WD_WIFI_INTERVAL_MS 8000
#define WD_BT_INTVL_MS     30000
#define WD_NRF_INTERVAL_MS   500

struct WdWifi {
  char     ssid[33];
  uint8_t  bssid[6];
  int8_t   rssi;
  uint8_t  channel;
  uint8_t  auth;
  uint16_t seenCount;
  bool     logged;
};
struct WdBle {
  char     name[32];
  uint8_t  addr[6];
  int8_t   rssi;
  uint16_t seenCount;
  bool     logged;
};
struct WdBt {
  char     name[32];
  uint8_t  addr[6];
  int8_t   rssi;
  uint16_t seenCount;
  bool     logged;
};

static WdWifi wdWifi[WD_MAX_WIFI];
static WdBle  wdBle[WD_MAX_BLE];
static WdBt   wdBt[WD_MAX_BT];
static volatile uint8_t wdWifiCount = 0;
static volatile uint8_t wdBleCount  = 0;
static volatile uint8_t wdBtCount   = 0;

static uint8_t  wdTab        = 0;  // 0=WiFi 1=BLE 2=BT 3=RF
static int16_t  wdScroll     = 0;
static bool     wdBtInited   = false;
static bool     wdScanPending = false;
static unsigned long wdScanStartMs  = 0;
static unsigned long wdBtScanMs     = 0;
static unsigned long lastWdWifiMs   = 0;
static unsigned long lastWdNrfMs    = 0;
static unsigned long wdSessionMs    = 0;
static uint32_t      wdLoggedCount  = 0;
static char          wdLogFile[40]  = {};
static volatile bool wdNeedRedraw   = false;

static uint8_t wdSpectrum[126]     = {};
static uint8_t wdSpectrumPrev[126] = {};

static esp_ble_scan_params_t wdBleScanParams = {
  .scan_type          = BLE_SCAN_TYPE_PASSIVE,
  .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
  .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
  .scan_interval      = 0x50,
  .scan_window        = 0x30,
  .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE
};

// Wardrive layout constants
#define WD_TAB_WIFI  0
#define WD_TAB_BLE   1
#define WD_TAB_BT    2
#define WD_TAB_RF    3
#define WD_TAB_H    24
#define WD_LIST_TOP (TITLE_H + WD_TAB_H)   // y=64
#define WD_LIST_BOT 298
#define WD_ROW_H    18
#define WD_VISIBLE  ((WD_LIST_BOT - WD_LIST_TOP) / WD_ROW_H)  // 13 rows

static const uint16_t wdTabCol[]   = {0x07FF, 0xC01F, 0xF800, 0x07E3};  // bright: cyan, violet, red, lime
static const char   * wdTabLabel[] = {"WIFI", "BLE", "BT", "RF"};

// ---------------------------------------------------------------------------
// Logs — SD file browser
// ---------------------------------------------------------------------------
#define LOGS_MAX_FILES  20
#define LOGS_ROW_H      22
#define LOGS_VISIBLE    ((300 - TITLE_H) / LOGS_ROW_H)

struct LogFile { char name[32]; uint32_t size; };
static LogFile logFiles[LOGS_MAX_FILES];
static uint8_t  logFileCount    = 0;
static int8_t   logsSelFile     = -1;  // -1 = file list view
static int16_t  logsScroll      = 0;
static int16_t  logsFileScroll  = 0;

struct LogEntry { char type[8]; char name[33]; char mac[18]; int16_t rssi; uint8_t ch; uint16_t seen; };
#define LOGS_ENTRY_MAX 120
static LogEntry logEntries[LOGS_ENTRY_MAX];
static uint16_t logEntryCount = 0;

// Forward declarations
void activateMode(Mode mode);
void selectRadioA();
void selectRadioB();
void drawUI();
void drawBackButton();
void drawJamUI(Mode mode);
void drawWardriveUI();
void drawLogsUI();
void runWardrive();
static void wdLogWifi(int idx);
static void wdLogBle(int idx);
static void wdLogBt(int idx);
static void IRAM_ATTR wdBleGapCb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p);
static const char *authLabel(uint8_t auth);

// ---------------------------------------------------------------------------
// Power button — AXP2101 PMIC PWR key via I2C polling
// ---------------------------------------------------------------------------
#define POWER_DEBOUNCE_MS 400
bool deviceOn = true;
unsigned long lastPowerBtnMs = 0;

void powerOff() {
  activateMode(OFF);
  if (radioAok) { selectRadioA(); radioA.stopConstCarrier(); radioA.powerDown(); }
  if (radioBok) { selectRadioB(); radioB.stopConstCarrier(); radioB.powerDown(); }
  gfx->fillScreen(0x0000);
  tca.write1(TCA_BL_PIN, 0);
  digitalWrite(LCD_BL, LOW);
  deviceOn = false;
}

void powerOn() {
  tca.write1(TCA_BL_PIN, 1);
  digitalWrite(LCD_BL, HIGH);
  if (radioAok) { selectRadioA(); radioA.powerUp(); }
  if (radioBok) { selectRadioB(); radioB.powerUp(); }
  delay(5);
  drawUI();
  deviceOn = true;
}

void handlePowerButton() {
  if (!pmuOk) return;
  PMU.getIrqStatus();
  if (!PMU.isPekeyShortPressIrq()) { PMU.clearIrqStatus(); return; }
  PMU.clearIrqStatus();
  unsigned long now = millis();
  if (now - lastPowerBtnMs < POWER_DEBOUNCE_MS) return;
  lastPowerBtnMs = now;
  deviceOn ? powerOff() : powerOn();
}

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
    case OFF:       return "OFF";
    case WIFI:      return "WIFI";
    case BLUETOOTH: return "BLUETOOTH";
    case BLE:       return "BLE";
    case JAMTIME:   return "JAMTIME";
    case SPECTRUM:  return "SPECTRUM";
    case NETSCAN:   return "NETSCAN";
    case WARDRIVE:  return "WARDRIVE";
    case LOGS:      return "LOGS";
    default:        return "UNKNOWN";
  }
}

void saveDefaultMode(Mode mode) {
  if (mode == SPECTRUM || mode == NETSCAN || mode == WARDRIVE || mode == LOGS) return;
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
  // Three persistent indicators: SD | Radio A | Radio B
  gfx->fillCircle(416, 17, 6, sdOk    ? COL_GREEN : COL_RED);
  gfx->fillCircle(436, 17, 6, radioAok ? COL_GREEN : COL_RED);
  gfx->fillCircle(456, 17, 6, radioBok ? COL_GREEN : COL_RED);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_LABEL);
  gfx->setCursor(411, 27); gfx->print("SD");
  gfx->setCursor(432, 27); gfx->print("A");
  gfx->setCursor(452, 27); gfx->print("B");
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
  // Page flip arrow — tap x>395 in title bar to switch page
  gfx->setTextSize(1);
  gfx->setTextColor(COL_LABEL);
  gfx->setCursor(397, 16);
  gfx->print(uiPage == 0 ? "\x1a" : "\x1b");  // → or ←  (ASCII 26/27)
}

static bool isJamMode(Mode m) {
  return m == WIFI || m == BLE || m == BLUETOOTH || m == JAMTIME;
}

void drawButton(int idx, bool active) {
  const Button &b = buttons[idx];
  uint16_t border = active ? COL_ACTIVE : COL_DIVIDER;
  uint16_t fill   = active ? b.color : (uint16_t)(((b.color >> 1) & 0x7BEF) | 0x2108);

  gfx->fillRect(b.x + BTN_BORDER, b.y + BTN_BORDER,
                b.w - 2*BTN_BORDER, b.h - 2*BTN_BORDER, fill);
  gfx->drawRect(b.x, b.y, b.w, b.h, border);
  gfx->drawRect(b.x+1, b.y+1, b.w-2, b.h-2, border);
  gfx->drawRect(b.x+2, b.y+2, b.w-4, b.h-4, border);

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
  if (uiPage == 0) {
    for (int i = 0; i < 6; i++)
      drawButton(i, buttons[i].mode == currentMode);
  } else {
    // Page 1: WARDRIVE (slot 0) + LOGS (slot 1)
    drawButton(6, buttons[6].mode == currentMode);
    drawButton(7, buttons[7].mode == currentMode);
    // Empty third slot and entire second row
    gfx->fillRect(2*BTN_W, TITLE_H, BTN_W, BTN_H, COL_BG);
    gfx->drawRect(2*BTN_W, TITLE_H, BTN_W, BTN_H, COL_DIVIDER);
    for (int c = 0; c < 3; c++) {
      gfx->fillRect(c*BTN_W, TITLE_H + BTN_H, BTN_W, BTN_H, COL_BG);
      gfx->drawRect(c*BTN_W, TITLE_H + BTN_H, BTN_W, BTN_H, COL_DIVIDER);
    }
  }
}

void refreshButtons() {
  if (uiPage == 0) {
    for (int i = 0; i < 6; i++)
      drawButton(i, buttons[i].mode == currentMode);
  } else {
    drawButton(6, buttons[6].mode == currentMode);
    drawButton(7, buttons[7].mode == currentMode);
  }
}

// ---------------------------------------------------------------------------
// Radio config
// ---------------------------------------------------------------------------
void selectRadioA() {
  spiHSPI.end();
  delayMicroseconds(50);
  spiHSPI.begin(NRF_CLK_A, NRF_MISO_A, NRF_MOSI_A, -1);
}

void selectRadioB() {
  spiHSPI.end();
  delayMicroseconds(50);
  spiHSPI.begin(NRF_CLK_B, NRF_MISO_B, NRF_MOSI_B, -1);
}

// Noise payload — 32 bytes of random data, refreshed periodically
uint8_t noisePayload[32];

void refreshNoise() {
  for (int i = 0; i < 32; i++) noisePayload[i] = random(256);
}

// CW tone on one channel — diagnostic-only. startConstCarrier emits an
// unmodulated carrier (single frequency, ~0 Hz wide) which does not
// effectively disrupt wideband OFDM like WiFi. Use spamChannel for jamming.
void cwOnChannel(RF24 &radio, RadioSelectFn selectRadio, uint8_t ch) {
  selectRadio();
  radio.stopConstCarrier();
  radio.startConstCarrier(RF24_PA_HIGH, ch);
}

// Modulated noise burst: transmit N non-ACK packets of random payload on `ch`.
// At 2 Mbps + 32-byte payload each packet is ~140 µs on-air and occupies
// ~1-2 MHz of GFSK-modulated bandwidth — looks like real noise to a WiFi/BT
// receiver, unlike a CW tone.
void spamChannel(RF24 &radio, RadioSelectFn selectRadio, uint8_t ch, uint8_t packets = 4) {
  selectRadio();
  radio.stopConstCarrier();
  radio.setChannel(ch);
  for (uint8_t i = 0; i < packets; i++) {
    for (int j = 0; j < 8; j++) noisePayload[j] = random(256);
    radio.writeFast(noisePayload, 32, true);  // multicast=true → no ACK wait
  }
}

bool configureRadio(RF24 &radio, RadioSelectFn selectRadio) {
  selectRadio();
  if (radio.begin(&spiHSPI)) {
    radio.setAutoAck(false);
    radio.stopListening();
    radio.setRetries(0, 0);
    // PA_HIGH (-6dBm chip out, still huge through PA+LNA) — PA_MAX peak current
    // sags the shared 3.3V rail enough to brown out the sibling radio.
    radio.setPALevel(RF24_PA_HIGH, true);
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

// ===========================================================================
//  WARDRIVE — passive 2.4 GHz recon: WiFi SSIDs, BLE adverts, BT Classic
//  inquiry, plus NRF24 channel-energy sweep.  SD card logs devices that are
//  seen >= WD_LOG_THRESHOLD times in a CSV under /jester/<session_ms>.csv
// ===========================================================================

// ---- helpers ---------------------------------------------------------------
static void macToStr(const uint8_t *a, char *out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", a[0],a[1],a[2],a[3],a[4],a[5]);
}
static bool macEq(const uint8_t *a, const uint8_t *b) { return memcmp(a,b,6)==0; }

// ---- SD logging ------------------------------------------------------------
static void wdEnsureDir() {
  if (!sdOk) return;
  if (!SD_MMC.exists("/sdcard/jester")) SD_MMC.mkdir("/sdcard/jester");
}

static void wdAppend(const char *line) {
  if (!sdOk || !wdLogFile[0]) return;
  File f = SD_MMC.open(wdLogFile, FILE_APPEND);
  if (!f) return;
  f.print(line);
  f.close();
  wdLoggedCount++;
}

static void wdLogWifi(int i) {
  char mac[18]; macToStr(wdWifi[i].bssid, mac);
  char line[128];
  sprintf(line, "%lu,WIFI,%s,%s,%d,%d,%d,%d\n",
          millis()-wdSessionMs, wdWifi[i].ssid, mac,
          (int)wdWifi[i].rssi, wdWifi[i].channel, wdWifi[i].auth, wdWifi[i].seenCount);
  wdAppend(line);
  wdWifi[i].logged = true;
}

static void wdLogBle(int i) {
  char mac[18]; macToStr(wdBle[i].addr, mac);
  char line[96];
  sprintf(line, "%lu,BLE,%s,%s,%d,0,0,%d\n",
          millis()-wdSessionMs, wdBle[i].name[0] ? wdBle[i].name : "?",
          mac, (int)wdBle[i].rssi, wdBle[i].seenCount);
  wdAppend(line);
  wdBle[i].logged = true;
}

static void wdLogBt(int i) {
  char mac[18]; macToStr(wdBt[i].addr, mac);
  char line[96];
  sprintf(line, "%lu,BT,%s,%s,%d,0,0,%d\n",
          millis()-wdSessionMs, wdBt[i].name[0] ? wdBt[i].name : "?",
          mac, (int)wdBt[i].rssi, wdBt[i].seenCount);
  wdAppend(line);
  wdBt[i].logged = true;
}

// ---- device table helpers --------------------------------------------------
static void wdAddWifi(const char *ssid, const uint8_t *bssid,
                       int8_t rssi, uint8_t ch, uint8_t auth) {
  for (int i = 0; i < (int)wdWifiCount; i++) {
    if (macEq(wdWifi[i].bssid, bssid) || strncmp(wdWifi[i].ssid, ssid, 32)==0) {
      wdWifi[i].rssi = rssi;
      wdWifi[i].seenCount++;
      if (!wdWifi[i].logged && wdWifi[i].seenCount >= WD_LOG_THRESHOLD) { wdLogWifi(i); wdNeedRedraw = true; }
      return;  // no redraw for RSSI-only updates
    }
  }
  if (wdWifiCount >= WD_MAX_WIFI) return;
  int idx = wdWifiCount++;
  strncpy(wdWifi[idx].ssid, ssid, 32); wdWifi[idx].ssid[32] = '\0';
  memcpy(wdWifi[idx].bssid, bssid, 6);
  wdWifi[idx].rssi = rssi; wdWifi[idx].channel = ch;
  wdWifi[idx].auth = auth; wdWifi[idx].seenCount = 1; wdWifi[idx].logged = false;
  wdNeedRedraw = true;
}

// Parse BLE adv name (type 0x09 = complete, 0x08 = shortened)
static bool parseBLEName(const uint8_t *d, uint8_t len, char *out, uint8_t maxl) {
  uint16_t i = 0;
  while (i < len) {
    uint8_t fl = d[i];
    if (!fl || i + fl >= len) break;
    uint8_t tp = d[i+1];
    if (tp == 0x09 || tp == 0x08) {
      uint8_t nl = fl - 1;
      if (nl >= maxl) nl = maxl - 1;
      memcpy(out, d+i+2, nl); out[nl] = '\0';
      return true;
    }
    i += fl + 1;
  }
  return false;
}

// ---- BLE gap callback (runs on BT task — keep fast) -----------------------
static void IRAM_ATTR wdBleGapCb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p) {
  if (ev != ESP_GAP_BLE_SCAN_RESULT_EVT) return;
  if (p->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) return;
  if (currentMode != WARDRIVE) return;

  uint8_t *addr = p->scan_rst.bda;
  int8_t   rssi = p->scan_rst.rssi;
  char name[32] = "";
  bool hasName  = parseBLEName(p->scan_rst.ble_adv, p->scan_rst.adv_data_len, name, 32);
  if (!hasName)
    parseBLEName(p->scan_rst.ble_adv + p->scan_rst.adv_data_len,
                 p->scan_rst.scan_rsp_len, name, 32);

  for (int i = 0; i < (int)wdBleCount; i++) {
    if (macEq(wdBle[i].addr, addr)) {
      wdBle[i].rssi = rssi;
      wdBle[i].seenCount++;
      if (name[0] && !wdBle[i].name[0]) { strncpy(wdBle[i].name, name, 31); wdNeedRedraw = true; }
      if (!wdBle[i].logged && wdBle[i].seenCount >= WD_LOG_THRESHOLD) { wdLogBle(i); wdNeedRedraw = true; }
      return;  // no redraw for RSSI-only updates — avoids constant list flicker
    }
  }
  if (wdBleCount >= WD_MAX_BLE) return;
  int idx = wdBleCount++;
  memcpy(wdBle[idx].addr, addr, 6);
  strncpy(wdBle[idx].name, name, 31); wdBle[idx].name[31] = '\0';
  wdBle[idx].rssi = rssi; wdBle[idx].seenCount = 1; wdBle[idx].logged = false;
  wdNeedRedraw = true;
}

// BT Classic device inquiry is not available in this build (requires
// CONFIG_BT_CLASSIC_ENABLED in sdkconfig). The BT tab shows NRF24 RPD
// activity on channels 2-80 (2402-2480 MHz = BT Classic band) as a proxy.

// ---- Wardrive UI -----------------------------------------------------------
static void drawWdTabs() {
  for (int t = 0; t < 4; t++) {
    int tx = t * 120;
    bool sel = (wdTab == (uint8_t)t);
    uint16_t col = sel ? wdTabCol[t] : (uint16_t)((wdTabCol[t] >> 1) & 0x7BEF);
    gfx->fillRect(tx, TITLE_H, 120, WD_TAB_H, sel ? 0x0820 : 0x0000);
    gfx->drawRect(tx, TITLE_H, 120, WD_TAB_H, sel ? col : COL_DIVIDER);
    gfx->setTextColor(sel ? col : COL_LABEL); gfx->setTextSize(1);
    int lw = strlen(wdTabLabel[t]) * 6;
    gfx->setCursor(tx + (120-lw)/2, TITLE_H + 8); gfx->print(wdTabLabel[t]);
    // Count badge
    uint8_t cnt = (t==0)?wdWifiCount:(t==1)?wdBleCount:(t==2)?wdBtCount:0;
    if (cnt > 0) {
      char bd[4]; sprintf(bd, "%d", cnt);
      gfx->setTextColor(0xFFFF);
      gfx->setCursor(tx + 96, TITLE_H + 3); gfx->print(bd);
    }
  }
}

static void drawSigBars(int x, int y, int8_t rssi, uint16_t col) {
  int bars = (rssi>=-50)?5:(rssi>=-60)?4:(rssi>=-70)?3:(rssi>=-80)?2:1;
  for (int b = 0; b < 5; b++)
    gfx->fillRect(x + b*5, y + (4-b), 4, b+1, (b<bars)?col:(uint16_t)0x2104);
}

static void drawWdRow(int row, int di, uint8_t tab) {
  int y = WD_LIST_TOP + row * WD_ROW_H;
  gfx->fillRect(0, y, 480, WD_ROW_H, (row&1) ? 0x0820 : 0x0000);
  gfx->setTextSize(1);

  if (tab == WD_TAB_WIFI && di < (int)wdWifiCount) {
    WdWifi &d = wdWifi[di];
    uint16_t c = wdTabCol[0];
    drawSigBars(2, y+4, d.rssi, c);
    char nm[25]; strncpy(nm, d.ssid, 24); nm[24]='\0';
    gfx->setTextColor(d.logged ? c : (uint16_t)0xFFFF);
    gfx->setCursor(32, y+5); gfx->print(nm);
    char tmp[10];
    sprintf(tmp,"CH%d",d.channel); gfx->setTextColor(c); gfx->setCursor(256,y+5); gfx->print(tmp);
    sprintf(tmp,"%4d",(int)d.rssi);                      gfx->setCursor(300,y+5); gfx->print(tmp);
    gfx->setCursor(348, y+5); gfx->print(authLabel(d.auth));
    sprintf(tmp,"x%d",d.seenCount); gfx->setTextColor(COL_SECONDARY); gfx->setCursor(400,y+5); gfx->print(tmp);
    if (d.logged) { gfx->setTextColor(0x07E0); gfx->setCursor(452,y+5); gfx->print("SD"); }

  } else if (tab == WD_TAB_BLE && di < (int)wdBleCount) {
    WdBle &d = wdBle[di];
    uint16_t c = wdTabCol[1];
    drawSigBars(2, y+4, d.rssi, c);
    gfx->setTextColor(d.logged ? c : (uint16_t)0xFFFF);
    gfx->setCursor(32, y+5); gfx->print(d.name[0] ? d.name : "Unknown");
    char mac[18]; macToStr(d.addr, mac);
    gfx->setTextColor(COL_SECONDARY); gfx->setCursor(200,y+5); gfx->print(mac);
    char tmp[8]; sprintf(tmp,"%4d",(int)d.rssi);
    gfx->setTextColor(c); gfx->setCursor(352,y+5); gfx->print(tmp);
    sprintf(tmp,"x%d",d.seenCount); gfx->setTextColor(COL_SECONDARY); gfx->setCursor(398,y+5); gfx->print(tmp);
    if (d.logged) { gfx->setTextColor(0x07E0); gfx->setCursor(450,y+5); gfx->print("SD"); }

  } else if (tab == WD_TAB_BT && di < (int)wdBtCount) {
    WdBt &d = wdBt[di];
    uint16_t c = wdTabCol[2];
    drawSigBars(2, y+4, d.rssi, c);
    gfx->setTextColor(d.logged ? c : (uint16_t)0xFFFF);
    gfx->setCursor(32, y+5); gfx->print(d.name[0] ? d.name : "Unknown");
    char mac[18]; macToStr(d.addr, mac);
    gfx->setTextColor(COL_SECONDARY); gfx->setCursor(200,y+5); gfx->print(mac);
    char tmp[8]; sprintf(tmp,"%4d",(int)d.rssi);
    gfx->setTextColor(c); gfx->setCursor(352,y+5); gfx->print(tmp);
    sprintf(tmp,"x%d",d.seenCount); gfx->setTextColor(COL_SECONDARY); gfx->setCursor(398,y+5); gfx->print(tmp);
    if (d.logged) { gfx->setTextColor(0x07E0); gfx->setCursor(450,y+5); gfx->print("SD"); }
  }
}

static void drawWdList() {
  if (wdTab == WD_TAB_RF) {
    // Differential update — only repaint columns whose height changed (no full-area clear)
    const int ctop = WD_LIST_TOP, cbot = WD_LIST_BOT, ch = cbot - ctop;
    for (int c = 0; c < 126; c++) {
      int bx = (c * 480) / 126, bw = ((c+1)*480)/126 - bx;
      if (bw < 1) bw = 1;
      int bh    = ((int)wdSpectrum[c]     * ch) / 63;
      int prevH = ((int)wdSpectrumPrev[c] * ch) / 63;
      if (bh == prevH) continue;
      bool isBleAdv = (c==2||c==26||c==80);
      bool isWifi   = (c<=12)||(c>=21&&c<=33)||(c>=44&&c<=56);
      uint16_t col  = isBleAdv?0xC01F:(isWifi?0x07FF:(c>=2&&c<=80?0xF800:0x07E0));
      if (bh > prevH) {
        gfx->fillRect(bx, cbot-bh, bw, bh-prevH, col);
        if (bh >= 2) gfx->fillRect(bx, cbot-bh, bw, 2, 0xFFFF);
        if (prevH >= 2) gfx->fillRect(bx, cbot-prevH, bw, 2, col);
      } else {
        gfx->fillRect(bx, cbot-prevH, bw, prevH-bh, 0x0000);
        if (bh >= 2) gfx->fillRect(bx, cbot-bh, bw, 2, 0xFFFF);
        else if (bh > 0) gfx->fillRect(bx, cbot-bh, bw, bh, col);
      }
      wdSpectrumPrev[c] = wdSpectrum[c];
    }
    return;
  }
  // BT tab: redirect to RF tab message (Classic BT inquiry not compiled in)
  if (wdTab == WD_TAB_BT) {
    gfx->fillRect(0, WD_LIST_TOP, 480, WD_LIST_BOT-WD_LIST_TOP, 0x0000);
    gfx->setTextColor(0xF800); gfx->setTextSize(1);
    gfx->setCursor(60, WD_LIST_TOP+60);
    gfx->print("BT Classic inquiry requires CONFIG_BT_CLASSIC_ENABLED");
    gfx->setTextColor(0x528A);
    gfx->setCursor(60, WD_LIST_TOP+80);
    gfx->print("Switch to RF tab to see BT energy on ch 2-80 (2402-2480 MHz)");
    return;
  }
  uint8_t cnt = (wdTab==WD_TAB_WIFI)?wdWifiCount:wdBleCount;
  if (cnt == 0) {
    gfx->fillRect(0, WD_LIST_TOP, 480, WD_LIST_BOT-WD_LIST_TOP, 0x0000);
    gfx->setTextColor(COL_LABEL); gfx->setTextSize(1);
    gfx->setCursor(170, WD_LIST_TOP+90); gfx->print("Scanning...");
    return;
  }
  int maxScr = max(0, (int)cnt - WD_VISIBLE);
  if (wdScroll > maxScr) wdScroll = maxScr;
  if (wdScroll < 0)      wdScroll = 0;
  for (int r = 0; r < WD_VISIBLE; r++) {
    int di = wdScroll + r;
    if (di < cnt) drawWdRow(r, di, wdTab);
    else gfx->fillRect(0, WD_LIST_TOP + r*WD_ROW_H, 480, WD_ROW_H, 0x0000);
  }
  // Scroll bar
  if (cnt > WD_VISIBLE) {
    int barH = (WD_LIST_BOT-WD_LIST_TOP) * WD_VISIBLE / cnt;
    int barY = WD_LIST_TOP + (WD_LIST_BOT-WD_LIST_TOP) * wdScroll / cnt;
    gfx->fillRect(476, WD_LIST_TOP, 4, WD_LIST_BOT-WD_LIST_TOP, 0x1082);
    gfx->fillRect(476, barY, 4, barH, COL_SECONDARY);
  }
}

static void drawWdStatus() {
  gfx->fillRect(0, WD_LIST_BOT, 480, 320-WD_LIST_BOT, 0x0000);
  gfx->drawFastHLine(0, WD_LIST_BOT, 480, 0x2104);
  gfx->setTextSize(1);
  char st[64];
  sprintf(st, "W:%d  B:%d  BT:%d  Logged:%lu", (int)wdWifiCount, (int)wdBleCount, (int)wdBtCount, wdLoggedCount);
  gfx->setTextColor(COL_LABEL); gfx->setCursor(2, WD_LIST_BOT+6); gfx->print(st);
  if (sdOk) {
    gfx->setTextColor(0x07E0); gfx->setCursor(420, WD_LIST_BOT+6); gfx->print("SD OK");
  } else {
    gfx->setTextColor(0xF800); gfx->setCursor(412, WD_LIST_BOT+6); gfx->print("NO SD");
  }
}

void drawWardriveUI() {
  gfx->fillScreen(0x0000);
  // Title bar
  gfx->fillRect(0, 0, 480, TITLE_H, 0x0000);
  drawBackButton();
  gfx->setTextColor(COL_WARDRIVE); gfx->setTextSize(2);
  const char *t = "WARDRIVE";
  gfx->setCursor((480 - (int)strlen(t)*12)/2, (TITLE_H-16)/2);
  gfx->print(t);
  drawStatusDots();
  gfx->setTextSize(1);
  gfx->setTextColor(sdOk ? (uint16_t)0x07E0 : (uint16_t)0xF800);
  gfx->setCursor(100, 28); gfx->print(sdOk ? "SD" : "NO SD");
  // Tab bar
  drawWdTabs();
  gfx->drawFastHLine(0, TITLE_H+WD_TAB_H, 480, COL_DIVIDER);
  // List area
  gfx->drawFastHLine(0, WD_LIST_BOT, 480, COL_DIVIDER);
  drawWdList();
  drawWdStatus();
}

// ---- Wardrive execute loop ------------------------------------------------
void runWardrive() {
  unsigned long now = millis();

  // NRF24 energy sweep (both radios in RX; no jamming in wardrive mode)
  if (now - lastWdNrfMs >= WD_NRF_INTERVAL_MS) {
    lastWdNrfMs = now;
    // Use Radio B only — Radio A is powered down (GPIO 9/10/11 used by SD_MMC)
    RF24 *sr = radioBok ? &radioB : nullptr;
    RadioSelectFn sf = radioBok ? selectRadioB : nullptr;
    if (sr) {
      sf(); sr->stopConstCarrier();
      for (uint8_t c = 0; c < 126; c++) {
        sr->setChannel(c); sr->startListening(); delayMicroseconds(200);
        bool hit = sr->testRPD(); sr->stopListening();
        if (hit) wdSpectrum[c] = 63;
        else     wdSpectrum[c] = wdSpectrum[c] > 3 ? wdSpectrum[c] - 3 : 0;
      }
    }
    if (wdTab == WD_TAB_RF) wdNeedRedraw = true;
  }

  // Non-blocking WiFi scan every WD_WIFI_INTERVAL_MS
  if (!wdScanPending && now - lastWdWifiMs >= WD_WIFI_INTERVAL_MS) {
    wifi_scan_config_t sc = {};
    sc.show_hidden = 1; sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 50; sc.scan_time.active.max = 200;
    esp_wifi_scan_start(&sc, false);
    wdScanPending = true; wdScanStartMs = now;
  }
  if (wdScanPending && now - wdScanStartMs >= 600) {
    uint16_t count = 0; esp_wifi_scan_get_ap_num(&count);
    if (count > WD_MAX_WIFI) count = WD_MAX_WIFI;
    wifi_ap_record_t *recs = (wifi_ap_record_t*)malloc(count * sizeof(wifi_ap_record_t));
    if (recs) {
      esp_wifi_scan_get_ap_records(&count, recs);
      for (int i = 0; i < (int)count; i++)
        wdAddWifi((char*)recs[i].ssid, recs[i].bssid, recs[i].rssi,
                  recs[i].primary, recs[i].authmode);
      free(recs);
    }
    wdScanPending = false; lastWdWifiMs = now;
  }

  // BT Classic inquiry not available (needs CONFIG_BT_CLASSIC_ENABLED sdkconfig).
  // BT activity is visible on the RF tab via NRF24 RPD on ch 2-80.

  // Redraw list when data arrives (throttle to ~10 Hz)
  static unsigned long lastWdRedrawMs = 0;
  if (wdNeedRedraw && now - lastWdRedrawMs >= 400) {
    lastWdRedrawMs = now;
    drawWdTabs();
    drawWdList();
    drawWdStatus();
    wdNeedRedraw = false;
  }
}

// ===========================================================================
//  LOGS — SD card log browser
// ===========================================================================
static void logsLoadFiles() {
  logFileCount = 0;
  if (!sdOk) return;
  File dir = SD_MMC.open("/sdcard/jester");
  if (!dir || !dir.isDirectory()) return;
  File f;
  while (logFileCount < LOGS_MAX_FILES && (f = dir.openNextFile())) {
    if (!f.isDirectory()) {
      strncpy(logFiles[logFileCount].name, f.name(), 31);
      logFiles[logFileCount].name[31] = '\0';
      logFiles[logFileCount].size = f.size();
      logFileCount++;
    }
    f.close();
  }
  dir.close();
}

static void logsLoadEntries(int fi) {
  logEntryCount = 0;
  if (!sdOk || fi < 0 || fi >= logFileCount) return;
  char path[48]; sprintf(path, "/jester/%s", logFiles[fi].name);
  File f = SD_MMC.open(path);
  if (!f) return;
  while (f.available() && logEntryCount < LOGS_ENTRY_MAX) {
    String line = f.readStringUntil('\n');
    if (line.length() < 8) continue;
    // ms,TYPE,name,mac,rssi,ch,auth,seen
    int p = 0, c;
    c = line.indexOf(',',p); if(c<0) continue; p=c+1; // skip ms
    LogEntry &e = logEntries[logEntryCount];
    c = line.indexOf(',',p); if(c<0) continue; line.substring(p,c).toCharArray(e.type, 8); p=c+1;
    c = line.indexOf(',',p); if(c<0) continue; line.substring(p,c).toCharArray(e.name, 33); p=c+1;
    c = line.indexOf(',',p); if(c<0) continue; line.substring(p,c).toCharArray(e.mac, 18);  p=c+1;
    c = line.indexOf(',',p); if(c<0) continue; e.rssi = (int16_t)line.substring(p,c).toInt(); p=c+1;
    c = line.indexOf(',',p); if(c<0) continue; e.ch   = (uint8_t)line.substring(p,c).toInt(); p=c+1;
    c = line.indexOf(',',p); if(c<0) continue; p=c+1; // skip auth
    e.seen = (uint16_t)line.substring(p).toInt();
    logEntryCount++;
  }
  f.close();
}

static void drawLogsFileList() {
  gfx->fillRect(0, TITLE_H, 480, 320-TITLE_H, 0x0000);
  if (!sdOk) {
    gfx->setTextColor(0xF800); gfx->setTextSize(1);
    gfx->setCursor(140, 160); gfx->print("SD card not detected"); return;
  }
  if (logFileCount == 0) {
    gfx->setTextColor(0x4208); gfx->setTextSize(1);
    gfx->setCursor(150, 160); gfx->print("No wardrive logs yet"); return;
  }
  int maxScr = max(0, (int)logFileCount - LOGS_VISIBLE);
  if (logsFileScroll < 0) logsFileScroll = 0;
  if (logsFileScroll > maxScr) logsFileScroll = maxScr;

  // Header
  gfx->setTextSize(1); gfx->setTextColor(COL_LABEL);
  gfx->setCursor(8, TITLE_H+4); gfx->print("FILENAME");
  gfx->setCursor(350, TITLE_H+4); gfx->print("SIZE");
  gfx->drawFastHLine(0, TITLE_H+13, 480, COL_DIVIDER);

  for (int r = 0; r < LOGS_VISIBLE && r + logsFileScroll < logFileCount; r++) {
    int i = r + logsFileScroll;
    int y = TITLE_H + 16 + r * LOGS_ROW_H;
    gfx->fillRect(0, y, 480, LOGS_ROW_H, (r&1) ? 0x0820 : 0x0000);
    gfx->setTextColor(COL_LOGS); gfx->setTextSize(1);
    gfx->setCursor(8, y+7); gfx->print(logFiles[i].name);
    char sz[16]; sprintf(sz, "%lu B", logFiles[i].size);
    gfx->setTextColor(COL_SECONDARY); gfx->setCursor(340, y+7); gfx->print(sz);
  }
  char st[32]; sprintf(st, "%d log file%s", logFileCount, logFileCount==1?"":"s");
  gfx->setTextColor(COL_LABEL); gfx->setCursor(2, 310); gfx->print(st);
}

static void drawLogsEntries() {
  gfx->fillRect(0, TITLE_H, 480, 320-TITLE_H, 0x0000);
  // "< FILES" link
  gfx->setTextColor(COL_LOGS); gfx->setTextSize(1);
  gfx->setCursor(4, TITLE_H+4); gfx->print("< FILES");
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(58, TITLE_H+4); gfx->print(logFiles[logsSelFile].name);
  gfx->drawFastHLine(0, TITLE_H+14, 480, 0x2104);

  int listTop = TITLE_H + 16;
  int rowH = 16;
  int vis   = (300 - listTop) / rowH;

  if (logEntryCount == 0) {
    gfx->setTextColor(0x4208); gfx->setCursor(160, 160); gfx->print("Empty log file"); return;
  }
  int maxScr = max(0, (int)logEntryCount - vis);
  if (logsScroll < 0) logsScroll = 0;
  if (logsScroll > maxScr) logsScroll = maxScr;

  for (int r = 0; r < vis && r + logsScroll < (int)logEntryCount; r++) {
    int i = r + logsScroll;
    int y = listTop + r * rowH;
    gfx->fillRect(0, y, 480, rowH, (r&1) ? 0x0820 : 0x0000);
    LogEntry &e = logEntries[i];
    uint16_t col = (strncmp(e.type,"WIFI",4)==0)?0x07FF:
                   (strncmp(e.type,"BLE", 3)==0)?0xC01F:
                   (strncmp(e.type,"BT",  2)==0)?0xF800:0x07E0;
    gfx->setTextColor(col); gfx->setTextSize(1);
    gfx->setCursor(2,  y+4); gfx->print(e.type);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(34, y+4); gfx->print(e.name);
    gfx->setTextColor(COL_SECONDARY);
    gfx->setCursor(218,y+4); gfx->print(e.mac);
    char tmp[8]; sprintf(tmp,"%4d",(int)e.rssi);
    gfx->setTextColor(col); gfx->setCursor(380,y+4); gfx->print(tmp);
    if (e.ch) { sprintf(tmp,"C%d",e.ch); gfx->setTextColor(COL_SECONDARY); gfx->setCursor(430,y+4); gfx->print(tmp); }
  }
  char st[32]; sprintf(st, "%d entries", logEntryCount);
  gfx->setTextColor(COL_LABEL); gfx->setCursor(2, 308); gfx->print(st);
}

void drawLogsUI() {
  gfx->fillScreen(0x0000);
  gfx->fillRect(0, 0, 480, TITLE_H, 0x0000);
  drawBackButton();
  gfx->setTextColor(COL_LOGS); gfx->setTextSize(2);
  const char *t = "WARDRIVE LOGS";
  gfx->setCursor((480-(int)strlen(t)*12)/2, (TITLE_H-16)/2);
  gfx->print(t);
  drawStatusDots();
  if (logsSelFile < 0) drawLogsFileList();
  else                  drawLogsEntries();
}

// ---------------------------------------------------------------------------
// Mode control
// ---------------------------------------------------------------------------
void activateMode(Mode mode) {
  if (radioAok) { selectRadioA(); radioA.stopConstCarrier(); }
  if (radioBok) { selectRadioB(); radioB.stopConstCarrier(); }

  // Exiting SPECTRUM (radio in RX): restore TX config
  if (currentMode == SPECTRUM && mode != SPECTRUM) {
    if (radioAok) configureRadio(radioA, selectRadioA);
    if (radioBok) configureRadio(radioB, selectRadioB);
    currentMode = mode;
    drawUI();
    return;
  }

  // Exiting NETSCAN: restore radio TX config and tear down WiFi
  if (currentMode == NETSCAN && mode != NETSCAN) {
    if (radioAok) configureRadio(radioA, selectRadioA);
    if (radioBok) configureRadio(radioB, selectRadioB);
    esp_wifi_stop();
    esp_wifi_deinit();
    currentMode = mode;
    drawUI();
    return;
  }

  // Exiting WARDRIVE
  if (currentMode == WARDRIVE && mode != WARDRIVE) {
    if (wdBtInited) esp_ble_gap_stop_scanning();
    if (wdScanPending) { esp_wifi_scan_stop(); wdScanPending = false; }
    esp_wifi_stop(); esp_wifi_deinit();
    // Unmount SD_MMC so GPIO 9/10/11 return to Radio A
    SD_MMC.end(); sdOk = false;
    if (radioAok) configureRadio(radioA, selectRadioA);
    if (radioBok) configureRadio(radioB, selectRadioB);
    currentMode = mode;
    drawUI();
    return;
  }

  // Entering WARDRIVE
  if (mode == WARDRIVE) {
    wdWifiCount = 0; wdBleCount = 0; wdBtCount = 0;
    wdScroll = 0; wdTab = 0; wdNeedRedraw = false;
    memset(wdSpectrum, 0, sizeof(wdSpectrum));
    memset(wdSpectrumPrev, 0, sizeof(wdSpectrumPrev));
    wdScanPending = false; lastWdWifiMs = 0; wdBtScanMs = 0;
    lastWdNrfMs = 0; wdLoggedCount = 0; wdSessionMs = millis();
    sprintf(wdLogFile, "/sdcard/jester/%lu.csv", wdSessionMs / 1000);
    // Power down Radio A so GPIO 9/10/11 can be used by SD_MMC
    if (radioAok) { selectRadioA(); radioA.powerDown(); spiHSPI.end(); }
    // Mount SD_MMC (1-bit, GPIO 9/10/11)
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
    sdOk = SD_MMC.begin("/sdcard", true);
    if (sdOk) wdEnsureDir();
    // WiFi
    wifi_init_config_t wCfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wCfg); esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_start();
    // BT/BLE stack — init once per boot (BT controller can't be fully re-inited)
    if (!wdBtInited) {
      esp_bt_controller_config_t btCfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
      bool ok = (esp_bt_controller_init(&btCfg)     == ESP_OK) &&
                (esp_bt_controller_enable(ESP_BT_MODE_BLE)  == ESP_OK) &&
                (esp_bluedroid_init()                == ESP_OK) &&
                (esp_bluedroid_enable()              == ESP_OK);
      if (ok) {
        esp_ble_gap_register_callback(wdBleGapCb);
        esp_ble_gap_set_scan_params(&wdBleScanParams);
        wdBtInited = true;
      }
    }
    if (wdBtInited)
      esp_ble_gap_start_scanning(0);  // 0 = continuous
    currentMode = WARDRIVE;
    drawWardriveUI();
    return;
  }

  // Entering LOGS
  if (mode == LOGS) {
    logsSelFile = -1; logsScroll = 0; logsFileScroll = 0;
    logsLoadFiles();
    currentMode = LOGS;
    drawLogsUI();
    return;
  }

  // Exiting a jam mode: restore both radios to TX
  if (isJamMode(currentMode) && !isJamMode(mode)) {
    if (radioAok) configureRadio(radioA, selectRadioA);
    if (radioBok) configureRadio(radioB, selectRadioB);
    currentMode = mode;
    drawUI();
    return;
  }

  // Entering a jam mode: go full-screen
  if (isJamMode(mode)) {
    memset(jamSpectrum,     0, sizeof(jamSpectrum));
    memset(jamSpectrumPrev, 0, sizeof(jamSpectrumPrev));
    lastJamScanMs = 0;
    currentMode = mode;
    drawJamUI(mode);
    return;
  }

  currentMode = mode;

  if (mode == SPECTRUM) {
    memset(gSpectrum,         0, sizeof(gSpectrum));
    memset(gSpectrumPrev,     0, sizeof(gSpectrumPrev));
    memset(gSpectrumPeak,     0, sizeof(gSpectrumPeak));
    memset(gSpectrumPeakPrev, 0, sizeof(gSpectrumPeakPrev));
    drawSpectrumUI();
  } else if (mode == NETSCAN) {
    memset(gComboSpectrum,     0, sizeof(gComboSpectrum));
    memset(gComboSpectrumPrev, 0, sizeof(gComboSpectrumPrev));
    // Re-init internal WiFi for non-blocking AP scanning
    wifi_init_config_t wifiCfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifiCfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    drawNetScanUI();
  } else {
    refreshButtons();
  }
}

// ---------------------------------------------------------------------------
// Jamming — packet-based noise for real wideband interference
// Each call blasts 3×32-byte noise packets per radio per channel hop.
// 2MBPS + 32-byte payload → each packet occupies ~2 MHz for ~140 µs.
// ---------------------------------------------------------------------------

// WiFi 2.4 GHz — full 20 MHz band coverage around each of the 3 non-overlapping centers.
// CH1 occupies NRF24 ch 0–12, CH6 ch 21–33, CH11 ch 44–56 (13 channels each).
// Radio A sweeps the lower half (indices 0–6), Radio B sweeps the upper half (indices 6–12).
// Each call advances one step so every channel in each band is hit every 7 calls.
static const uint8_t wifiBandStart[] = {0,  21, 44};
static const uint8_t wifiBandMid[]   = {6,  27, 50};
static uint8_t wifiSweepA = 0, wifiSweepB = 0;

void jamWifi() {
  for (int b = 0; b < 3; b++) {
    uint8_t chA = wifiBandStart[b] + (wifiSweepA % 7);
    uint8_t chB = wifiBandMid[b]   + (wifiSweepB % 7);
    if (radioAok) spamChannel(radioA, selectRadioA, chA, 8);
    if (radioBok) spamChannel(radioB, selectRadioB, chB, 8);
  }
  wifiSweepA = (wifiSweepA + 1) % 7;
  wifiSweepB = (wifiSweepB + 1) % 7;
}

// BLE: 3 advertising channels (2, 26, 80) + 37 data channels (4–78)
// Radio A hammers ALL 3 adv channels every call (8 pkts each) — no rotation.
// Radio B sweeps 2 data channels per call stepping by 2 (covers band in ~19 calls).
static const uint8_t bleAdvCh[] = {2, 26, 80};

void jamBLE() {
  static uint8_t bleSweepData = 4;
  if (radioAok) {
    for (int i = 0; i < 3; i++)
      spamChannel(radioA, selectRadioA, bleAdvCh[i], 8);
  }
  if (radioBok) {
    spamChannel(radioB, selectRadioB, bleSweepData, 4);
    uint8_t next = (bleSweepData + 1 <= 78) ? bleSweepData + 1 : 4;
    spamChannel(radioB, selectRadioB, next, 4);
  }
  bleSweepData += 2;
  if (bleSweepData > 78) bleSweepData = 4;
}

// Classic Bluetooth: 79 channels (NRF24 ch 2–80)
// Both radios sweep from opposite ends, stepping by 2 for faster band coverage.
// 6 packets per channel (was 3) for denser interference per hop slot.
void jamBluetooth() {
  if (radioAok) spamChannel(radioA, selectRadioA, btSweepA, 6);
  if (radioBok) spamChannel(radioB, selectRadioB, btSweepB, 6);
  btSweepA += 2; if (btSweepA > 80) btSweepA = 2;
  btSweepB  = (btSweepB <= 4) ? 80 : btSweepB - 2;
}

// ---------------------------------------------------------------------------
// JAM TIME — All-Protocol Saturator
// Runs all three targeted sweeps every call so WiFi, BLE, and Bluetooth
// are all hit at their tuned revisit rates simultaneously.
// ---------------------------------------------------------------------------
void jamAll() {
  jamWifi();
  jamBLE();
  jamBluetooth();
}

// ---------------------------------------------------------------------------
// Full-screen jam energy visualizer — BuggedLab cyberlab aesthetic
// Radio B time-slices RX for RPD sweep every 80 ms; Radio A jams throughout.
// Back button (< HOME, top-left) stops jam and returns to main menu.
// ---------------------------------------------------------------------------
static const char* getModeLabel(Mode mode) {
  switch (mode) {
    case WIFI:      return "WIFI 2.4";
    case BLE:       return "BLE";
    case BLUETOOTH: return "BLUETOOTH";
    case JAMTIME:   return "JAM TIME";
    default:        return "";
  }
}

static uint16_t jamAccent(Mode m) {
  switch (m) {
    case WIFI:      return 0x07FF;  // cyan
    case BLE:       return 0xC01F;  // violet
    case BLUETOOTH: return 0xF800;  // red
    case JAMTIME:   return 0xF81F;  // magenta
    default:        return 0xFFFF;
  }
}

// Background tint for a given channel — used when erasing a bar
static uint16_t zoneBgColor(uint8_t ch) {
  bool isBleAdv = (ch == 2 || ch == 26 || ch == 80);
  bool isWifi   = (ch <= 12) || (ch >= 21 && ch <= 33) || (ch >= 44 && ch <= 56);
  bool isBt     = (ch >= 2 && ch <= 80);
  switch (currentMode) {
    case WIFI:      return isWifi   ? 0x0082 : 0x0000;
    case BLE:       return isBleAdv ? 0x0830 : (isBt ? 0x0010 : 0x0000);
    case BLUETOOTH: return isBt     ? 0x1800 : 0x0000;
    default:        return 0x0000;
  }
}

// Sweep cursor state
static uint8_t  jamCurA = 0,   jamCurB = 63;
static uint8_t  jamPrevCurA = 255, jamPrevCurB = 255;  // 255 = uninitialised
// Blink state
static bool          jamBlinkOn    = true;
static unsigned long lastJamBlinkMs = 0;
#define JAM_BLINK_MS  500
#define BLINK_X  80
#define BLINK_Y  16
#define BLINK_W   8
#define BLINK_H   8

// Inner chart bounds (inside the 3-layer neon border)
#define JAM_CTOP  43
#define JAM_CBOT 297
#define JAM_CH   (JAM_CBOT - JAM_CTOP)   // 254 px

void drawJamUI(Mode mode) {
  uint16_t accent = jamAccent(mode);
  bool doWifi = (mode == WIFI      || mode == JAMTIME);
  bool doBle  = (mode == BLE       || mode == JAMTIME);
  bool doBt   = (mode == BLUETOOTH || mode == JAMTIME);

  // --- Pure black canvas ---
  gfx->fillScreen(0x0000);

  // --- Protocol zone tint backgrounds ---
  if (doBt) {
    int x1 = (2  * 480) / 126, x2 = (81 * 480) / 126;
    gfx->fillRect(x1, JAM_CTOP, x2 - x1, JAM_CH, 0x1800);
  }
  if (doWifi) {
    static const int ws[] = {0, 21, 44}, we[] = {12, 33, 56};
    for (int b = 0; b < 3; b++) {
      int x1 = (ws[b] * 480) / 126, x2 = ((we[b] + 1) * 480) / 126;
      gfx->fillRect(x1, JAM_CTOP, x2 - x1, JAM_CH, 0x0082);
    }
  }
  if (doBle) {
    gfx->fillRect(0, JAM_CTOP, (81 * 480) / 126, JAM_CH, 0x0010);
    static const uint8_t adv[] = {2, 26, 80};
    for (int i = 0; i < 3; i++) {
      int bx = (adv[i] * 480) / 126;
      int bw = (((int)adv[i] + 1) * 480) / 126 - bx;
      gfx->fillRect(bx, JAM_CTOP, bw, JAM_CH, 0x0830);
    }
  }

  // --- 3-layer neon glow border around chart ---
  uint16_t c3 = (accent >> 2) & 0x39E7;
  uint16_t c2 = (accent >> 1) & 0x7BEF;
  gfx->drawRect(0, 40,  480, 260, c3);
  gfx->drawRect(1, 41,  478, 258, c2);
  gfx->drawRect(2, 42,  476, 256, accent);

  // --- Title bar ---
  gfx->fillRect(0, 0, 480, TITLE_H, 0x0000);
  drawBackButton();

  // Blinking indicator square (starts ON)
  jamBlinkOn = true; lastJamBlinkMs = 0;
  gfx->fillRect(BLINK_X, BLINK_Y, BLINK_W, BLINK_H, accent);

  // Mode name in accent colour
  const char *label = getModeLabel(mode);
  gfx->setTextColor(accent);
  gfx->setTextSize(2);
  int tw = strlen(label) * 12;
  int lx = (480 - tw) / 2;
  gfx->setCursor(lx, (TITLE_H - 16) / 2);
  gfx->print(label);

  // "JAMMING" sub-label right of title (half-bright)
  gfx->setTextSize(1);
  gfx->setTextColor(c2);
  gfx->setCursor(lx + tw + 8, (TITLE_H - 8) / 2 + 1);
  gfx->print("JAMMING");

  drawStatusDots();

  // --- Frequency axis ---
  gfx->setTextSize(1);
  gfx->setTextColor(COL_LABEL);
  gfx->setCursor(3,   303); gfx->print("2400");
  gfx->setCursor(441, 303); gfx->print("2525");

  if (doWifi) {
    gfx->setTextColor(0x07FF);
    gfx->setCursor((2  * 480) / 126, 303); gfx->print("W1");
    gfx->setCursor((27 * 480) / 126, 303); gfx->print("W6");
    gfx->setCursor((52 * 480) / 126, 303); gfx->print("W11");
  }
  if (doBle) {
    gfx->setTextColor(0xC01F);
    gfx->setCursor((26 * 480) / 126, 311); gfx->print("B38");
    gfx->setCursor((80 * 480) / 126, 311); gfx->print("B39");
    static const uint8_t bleAdvNRF[] = {2, 26, 80};
    for (int i = 0; i < 3; i++)
      gfx->drawFastVLine((bleAdvNRF[i] * 480) / 126, 295, 4, 0xC01F);
  }
  if (doBt && !doBle) {
    gfx->setTextColor(0xF800);
    gfx->setCursor(3, 311); gfx->print("BT 2402-2480 MHz");
  }

  // Reset cursor state
  jamCurA = 0; jamCurB = 63;
  jamPrevCurA = 255; jamPrevCurB = 255;
}

void updateJamBar(uint8_t ch) {
  int bx = ((int)ch * 480) / 126;
  int bw = (((int)ch + 1) * 480) / 126 - bx;
  if (bw < 1) bw = 1;

  bool isBleAdv = (ch == 2 || ch == 26 || ch == 80);
  bool isWifi   = (ch <= 12) || (ch >= 21 && ch <= 33) || (ch >= 44 && ch <= 56);
  uint16_t col  = isBleAdv ? 0xC01F : (isWifi ? 0x07FF : (ch >= 2 && ch <= 80 ? 0xF800 : 0x07E0));

  int barH  = ((int)jamSpectrum[ch]     * JAM_CH) / 63;
  int prevH = ((int)jamSpectrumPrev[ch] * JAM_CH) / 63;
  if (barH == prevH) return;

  if (barH > prevH) {
    // Fill new body region
    gfx->fillRect(bx, JAM_CBOT - barH, bw, barH - prevH, col);
    // Repaint old tip (was white) with body colour
    if (prevH >= 2)
      gfx->fillRect(bx, JAM_CBOT - prevH, bw, 2, col);
    // New white-hot tip at bar top
    int tip = (barH >= 2) ? 2 : barH;
    gfx->fillRect(bx, JAM_CBOT - barH, bw, tip, 0xFFFF);
  } else {
    // Erase region above new bar
    gfx->fillRect(bx, JAM_CBOT - prevH, bw, prevH - barH, zoneBgColor(ch));
    // New tip
    if (barH >= 2)
      gfx->fillRect(bx, JAM_CBOT - barH, bw, 2, 0xFFFF);
    else if (barH > 0)
      gfx->fillRect(bx, JAM_CBOT - barH, bw, barH, 0xFFFF);
  }

  jamSpectrumPrev[ch] = jamSpectrum[ch];
}

// Draw a 1-px cursor line in the empty space above the bar at channel ch
static void paintCursor(uint8_t ch, uint16_t col) {
  if (ch >= 126) return;
  int bx = ((int)ch * 480) / 126 + 1;
  int barH  = ((int)jamSpectrum[ch] * JAM_CH) / 63;
  int lineBot = JAM_CBOT - barH - 3;
  if (lineBot > JAM_CTOP)
    gfx->drawFastVLine(bx, JAM_CTOP, lineBot - JAM_CTOP, col);
}

void runJamWithSpectrum() {
  // Jam
  switch (currentMode) {
    case WIFI:      jamWifi();      break;
    case BLE:       jamBLE();       break;
    case BLUETOOTH: jamBluetooth(); break;
    case JAMTIME:   jamAll();       break;
    default: break;
  }

  uint16_t accent = jamAccent(currentMode);

  // Sweep cursors: A forward, B backward, opposite ends of the band
  paintCursor(jamPrevCurA, zoneBgColor(jamPrevCurA));   // erase A
  paintCursor(jamPrevCurB, zoneBgColor(jamPrevCurB));   // erase B
  jamCurA = (jamCurA + 3) % 126;
  jamCurB = (jamCurB < 3) ? 125 : jamCurB - 3;
  paintCursor(jamCurA, accent);    // draw A in mode accent colour
  paintCursor(jamCurB, 0xFFFF);    // draw B in white
  jamPrevCurA = jamCurA;
  jamPrevCurB = jamCurB;

  // Blink indicator in title bar
  unsigned long now = millis();
  if (now - lastJamBlinkMs >= JAM_BLINK_MS) {
    lastJamBlinkMs = now;
    jamBlinkOn = !jamBlinkOn;
    uint16_t bc = jamBlinkOn ? accent : ((accent >> 2) & 0x39E7);
    gfx->fillRect(BLINK_X, BLINK_Y, BLINK_W, BLINK_H, bc);
  }

  // Time-slice Radio B for RPD scan
  if (now - lastJamScanMs < JAM_SCAN_INTERVAL_MS) return;
  lastJamScanMs = now;

  RF24 *scanRadio     = radioBok ? &radioB      : (radioAok ? &radioA      : nullptr);
  RadioSelectFn selFn = radioBok ? selectRadioB : (radioAok ? selectRadioA : nullptr);
  if (!scanRadio) return;

  selFn();
  scanRadio->stopConstCarrier();
  for (uint8_t ch = 0; ch < 126; ch++) {
    scanRadio->setChannel(ch);
    scanRadio->startListening();
    delayMicroseconds(200);
    bool hit = scanRadio->testRPD();
    scanRadio->stopListening();
    if (hit)
      jamSpectrum[ch] = 63;
    else
      jamSpectrum[ch] = (jamSpectrum[ch] > 3) ? jamSpectrum[ch] - 3 : 0;
    updateJamBar(ch);
  }
  configureRadio(*scanRadio, selFn);
}

// ---------------------------------------------------------------------------
// SPECTRUM mode — full-screen NRF24 RPD sweep with yellow peak-hold markers
// Back button in title bar: tap x=0-64, y=0-39 to return to main menu.
// ---------------------------------------------------------------------------

void drawBackButton() {
  gfx->drawRect(2, 5, 62, 28, COL_ACTIVE);
  gfx->setTextColor(COL_ACTIVE);
  gfx->setTextSize(1);
  gfx->setCursor(7, 14);
  gfx->print("< HOME");
}

void drawSpectrumUI() {
  gfx->fillScreen(COL_BG);

  // Title bar
  gfx->fillRect(0, 0, 480, TITLE_H, COL_BG);
  drawBackButton();
  gfx->setTextColor(COL_TITLE);
  gfx->setTextSize(2);
  const char *label = "SPECTRUM";
  int tw = strlen(label) * 12;
  gfx->setCursor((480 - tw) / 2, (TITLE_H - 16) / 2);
  gfx->print(label);
  // Peak-hold legend (left of dots)
  gfx->setTextColor(COL_ACTIVE);
  gfx->setTextSize(1);
  gfx->setCursor(374, 10);
  gfx->print("PEAK");
  gfx->drawFastHLine(374, 20, 24, COL_ACTIVE);  // sample tick
  drawStatusDots();

  // Frequency axis labels
  gfx->setTextSize(1);
  gfx->setTextColor(COL_TITLE);
  gfx->setCursor(0,   302); gfx->print("2400");
  gfx->setCursor(440, 302); gfx->print("2525");
  gfx->setTextColor(COL_WIFI);
  gfx->setCursor((2  * 480) / 126, 302); gfx->print("W1");
  gfx->setCursor((27 * 480) / 126, 302); gfx->print("W6");
  gfx->setCursor((52 * 480) / 126, 302); gfx->print("W11");
  gfx->setTextColor(COL_BLE);
  gfx->setCursor((26 * 480) / 126, 310); gfx->print("B38");
  gfx->setCursor((80 * 480) / 126, 310); gfx->print("B39");
  static const uint8_t bleAdvNRF[] = {2, 26, 80};
  for (int i = 0; i < 3; i++) {
    gfx->drawFastVLine((bleAdvNRF[i] * 480) / 126, 295, 4, COL_BLE);
  }
}

void updateSpectrumBar(uint8_t ch) {
  const int chartTop = 40;
  const int chartBot = 299;
  const int chartH   = chartBot - chartTop;

  int bx  = ((int)ch * 480) / 126;
  int bw  = (((int)ch + 1) * 480) / 126 - bx;
  if (bw < 1) bw = 1;

  bool isBleAdv = (ch == 2 || ch == 26 || ch == 80);
  bool isWifi   = (ch <= 12) || (ch >= 21 && ch <= 33) || (ch >= 44 && ch <= 56);
  uint16_t col  = isBleAdv ? COL_BLE : (isWifi ? COL_WIFI : COL_GREEN);

  // --- Bar (same proven logic as old updateScanBar) ---
  int barH  = ((int)gSpectrum[ch]     * chartH) / 63;
  int prevH = ((int)gSpectrumPrev[ch] * chartH) / 63;
  if (barH != prevH) {
    if (barH > prevH)
      gfx->fillRect(bx, chartBot - barH, bw, barH - prevH, col);
    else
      gfx->fillRect(bx, chartBot - prevH, bw, prevH - barH, COL_BG);
    gSpectrumPrev[ch] = gSpectrum[ch];
  }

  // --- Peak hold (only touches pixels strictly above the bar) ---
  uint8_t oldPeak = gSpectrumPeak[ch];
  if (gSpectrum[ch] >= gSpectrumPeak[ch])
    gSpectrumPeak[ch] = gSpectrum[ch];
  else if (gSpectrumPeak[ch] > 0)
    gSpectrumPeak[ch]--;

  if (oldPeak != gSpectrumPeak[ch]) {
    int oldPH = ((int)oldPeak           * chartH) / 63;
    int newPH = ((int)gSpectrumPeak[ch] * chartH) / 63;
    if (oldPH > barH && oldPH > 0)
      gfx->drawFastHLine(bx, chartBot - oldPH, bw, COL_BG);
    if (newPH > barH && newPH > 0)
      gfx->drawFastHLine(bx, chartBot - newPH, bw, COL_ACTIVE);
  }
}

void runSpectrum() {
  RF24 *scanRadio     = radioAok ? &radioA      : (radioBok ? &radioB      : nullptr);
  RadioSelectFn selFn = radioAok ? selectRadioA : (radioBok ? selectRadioB : nullptr);
  if (!scanRadio) return;

  selFn();
  for (uint8_t ch = 0; ch < 126; ch++) {
    scanRadio->setChannel(ch);
    scanRadio->startListening();
    delayMicroseconds(200);
    bool hit = scanRadio->testRPD();
    scanRadio->stopListening();
    if (hit)
      gSpectrum[ch] = 63;
    else
      gSpectrum[ch] = (gSpectrum[ch] > 3) ? gSpectrum[ch] - 3 : 0;
    updateSpectrumBar(ch);
  }
}

// ---------------------------------------------------------------------------
// NETSCAN mode — combined view: NRF24 spectrum (top) + 802.11 AP list (bottom)
// The NRF24 sweep and internal WiFi scan run concurrently (independent hardware).
// WiFi scan is non-blocking to keep spectrum live during the scan.
// Back button: tap x=0-64, y=0-39 to return to main menu.
// ---------------------------------------------------------------------------

struct APRecord {
  char    ssid[33];
  int8_t  rssi;
  uint8_t channel;
  uint8_t auth;
};

static APRecord apRecords[20];
static uint16_t apCount           = 0;
static unsigned long lastNetScanMs = 0;
static bool      wifiScanPending   = false;
static unsigned long wifiScanStartMs = 0;
#define NET_SCAN_INTERVAL_MS  10000  // start a new scan every 10 s
#define WIFI_SCAN_SETTLE_MS    4000  // wait 4 s after non-blocking start before reading

// Spectrum chart occupies the top half: y=40-178 (138 px)
#define COMBO_CHART_TOP   40
#define COMBO_CHART_BOT  178

static const char *authLabel(uint8_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN:         return "OPEN";
    case WIFI_AUTH_WEP:          return "WEP";
    case WIFI_AUTH_WPA_PSK:      return "WPA";
    case WIFI_AUTH_WPA2_PSK:     return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA+";
    case WIFI_AUTH_WPA3_PSK:     return "WPA3";
    default:                      return "OTH";
  }
}

void drawNetScanUI() {
  gfx->fillScreen(COL_BG);

  // Title bar
  gfx->fillRect(0, 0, 480, TITLE_H, COL_BG);
  drawBackButton();
  gfx->setTextColor(COL_TITLE);
  gfx->setTextSize(2);
  const char *title = "RF RECON";
  int tw = strlen(title) * 12;
  gfx->setCursor((480 - tw) / 2, (TITLE_H - 16) / 2);
  gfx->print(title);
  drawStatusDots();

  // Spectrum axis labels
  gfx->setTextSize(1);
  gfx->setTextColor(COL_TITLE);
  gfx->setCursor(0,   170); gfx->print("2400");
  gfx->setCursor(440, 170); gfx->print("2525");
  gfx->setTextColor(COL_WIFI);
  gfx->setCursor((2  * 480) / 126, 170); gfx->print("W1");
  gfx->setCursor((27 * 480) / 126, 170); gfx->print("W6");
  gfx->setCursor((52 * 480) / 126, 170); gfx->print("W11");

  // Divider between spectrum and network panel
  gfx->drawFastHLine(0, COMBO_CHART_BOT + 1, 480, COL_DIVIDER);

  // Network panel header
  gfx->setTextColor(COL_LABEL);
  gfx->setCursor(2,   182); gfx->print("SSID");
  gfx->setCursor(256, 182); gfx->print("CH");
  gfx->setCursor(292, 182); gfx->print("RSSI");
  gfx->setCursor(360, 182); gfx->print("AUTH");
  gfx->drawFastHLine(0, 191, 480, COL_DIVIDER);

  // Initial scanning placeholder
  gfx->setTextColor(COL_LABEL);
  gfx->setTextSize(1);
  gfx->setCursor(190, 255);
  gfx->print("Scanning...");
}

void updateComboSpectrumBar(uint8_t ch) {
  const int chartH = COMBO_CHART_BOT - COMBO_CHART_TOP;

  int bx  = ((int)ch * 480) / 126;
  int bx2 = (((int)ch + 1) * 480) / 126;
  int bw  = (bx2 > bx) ? bx2 - bx : 1;

  int barH  = ((int)gComboSpectrum[ch]     * chartH) / 63;
  int prevH = ((int)gComboSpectrumPrev[ch] * chartH) / 63;
  if (barH == prevH) return;

  bool isBleAdv = (ch == 2 || ch == 26 || ch == 80);
  bool isWifi   = (ch <= 12) || (ch >= 21 && ch <= 33) || (ch >= 44 && ch <= 56);
  uint16_t col  = isBleAdv ? COL_BLE : (isWifi ? COL_WIFI : COL_GREEN);

  if (barH > prevH)
    gfx->fillRect(bx, COMBO_CHART_BOT - barH, bw, barH - prevH, col);
  else
    gfx->fillRect(bx, COMBO_CHART_BOT - prevH, bw, prevH - barH, COL_BG);

  gComboSpectrumPrev[ch] = gComboSpectrum[ch];
}

void drawNetworkPanel() {
  gfx->fillRect(0, 192, 480, 127, COL_BG);  // clear data rows

  if (apCount == 0) {
    gfx->setTextColor(COL_RED);
    gfx->setTextSize(1);
    gfx->setCursor(190, 250);
    gfx->print("No networks");
    return;
  }

  // Sort by RSSI descending
  for (int i = 0; i < (int)apCount - 1; i++)
    for (int j = 0; j < (int)apCount - 1 - i; j++)
      if (apRecords[j].rssi < apRecords[j+1].rssi) {
        APRecord tmp = apRecords[j]; apRecords[j] = apRecords[j+1]; apRecords[j+1] = tmp;
      }

  int show = (apCount > 6) ? 6 : (int)apCount;
  gfx->setTextSize(1);

  for (int i = 0; i < show; i++) {
    int y = 193 + i * 20;
    int8_t rssi = apRecords[i].rssi;
    uint16_t col = (rssi >= -60) ? COL_GREEN : (rssi >= -75) ? COL_ACTIVE : COL_RED;

    // Signal bars
    int bars = (rssi >= -55) ? 5 : (rssi >= -65) ? 4 : (rssi >= -75) ? 3 : (rssi >= -85) ? 2 : 1;
    for (int b = 0; b < 5; b++)
      gfx->fillRect(2 + b * 8, y + 4, 6, 10, (b < bars) ? col : COL_DIVIDER);

    // SSID (truncate to 22 chars)
    char ssid[23];
    strncpy(ssid, apRecords[i].ssid, 22); ssid[22] = '\0';
    if (strlen(apRecords[i].ssid) > 22) { ssid[20] = '.'; ssid[21] = '.'; }
    gfx->setTextColor(col);
    gfx->setCursor(46, y + 5); gfx->print(ssid);

    char tmp[12];
    sprintf(tmp, "%2d", apRecords[i].channel);
    gfx->setCursor(258, y + 5); gfx->print(tmp);

    sprintf(tmp, "%4ddBm", rssi);
    gfx->setCursor(280, y + 5); gfx->print(tmp);

    gfx->setCursor(360, y + 5); gfx->print(authLabel(apRecords[i].auth));
  }

  // Status line
  char status[48];
  sprintf(status, "%d network%s | refresh 10s", (int)apCount, apCount == 1 ? "" : "s");
  gfx->setTextColor(COL_LABEL);
  gfx->setCursor(2, 313);
  gfx->print(status);
}

void runNetScan() {
  // ---- NRF24 spectrum sweep (top half, always running) ----
  RF24 *scanRadio     = radioAok ? &radioA      : (radioBok ? &radioB      : nullptr);
  RadioSelectFn selFn = radioAok ? selectRadioA : (radioBok ? selectRadioB : nullptr);
  if (scanRadio) {
    selFn();
    for (uint8_t ch = 0; ch < 126; ch++) {
      scanRadio->setChannel(ch);
      scanRadio->startListening();
      delayMicroseconds(200);
      bool hit = scanRadio->testRPD();
      scanRadio->stopListening();
      if (hit)
        gComboSpectrum[ch] = 63;
      else
        gComboSpectrum[ch] = (gComboSpectrum[ch] > 3) ? gComboSpectrum[ch] - 3 : 0;
      updateComboSpectrumBar(ch);
    }
  }

  // ---- Non-blocking WiFi scan (bottom half, periodic) ----
  unsigned long now = millis();

  if (!wifiScanPending && now - lastNetScanMs >= NET_SCAN_INTERVAL_MS) {
    wifi_scan_config_t sc = {};
    sc.show_hidden  = 1;
    sc.scan_type    = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 50;
    sc.scan_time.active.max = 100;
    esp_wifi_scan_start(&sc, false);  // non-blocking — NRF24 sweep continues
    wifiScanPending  = true;
    wifiScanStartMs  = now;
    // Show scanning indicator
    gfx->setTextColor(0x4208);
    gfx->setTextSize(1);
    gfx->fillRect(0, 310, 200, 9, COL_BG);
    gfx->setCursor(2, 313); gfx->print("Scanning...");
  }

  if (wifiScanPending && now - wifiScanStartMs >= WIFI_SCAN_SETTLE_MS) {
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 20) count = 20;
    wifi_ap_record_t *recs = (wifi_ap_record_t*)malloc(count * sizeof(wifi_ap_record_t));
    if (recs) {
      esp_wifi_scan_get_ap_records(&count, recs);
      apCount = count;
      for (int i = 0; i < (int)count; i++) {
        strncpy(apRecords[i].ssid, (char*)recs[i].ssid, 32);
        apRecords[i].ssid[32] = '\0';
        apRecords[i].rssi    = recs[i].rssi;
        apRecords[i].channel = recs[i].primary;
        apRecords[i].auth    = recs[i].authmode;
      }
      free(recs);
    }
    wifiScanPending = false;
    lastNetScanMs   = now;
    drawNetworkPanel();
  }
}

// ---------------------------------------------------------------------------

void executeMode() {
  switch (currentMode) {
    case OFF:                          delay(50);              break;
    case WIFI:
    case BLE:
    case BLUETOOTH:
    case JAMTIME:                      runJamWithSpectrum();   break;
    case SPECTRUM:                     runSpectrum();          break;
    case NETSCAN:                      runNetScan();           break;
    case WARDRIVE:                     runWardrive();          break;
    case LOGS:                         delay(50);              break;
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

  // WARDRIVE full-screen touch handling
  if (currentMode == WARDRIVE) {
    if (tx < 65 && ty < TITLE_H) { activateMode(OFF); return; }
    // Tab bar taps
    if (ty >= TITLE_H && ty < TITLE_H + WD_TAB_H) {
      uint8_t newTab = tx / 120;
      if (newTab != wdTab) {
        wdTab = newTab; wdScroll = 0;
        // Clear list area first; for RF tab reset prev so differential draws from scratch
        gfx->fillRect(0, WD_LIST_TOP, 480, WD_LIST_BOT-WD_LIST_TOP, 0x0000);
        if (newTab == WD_TAB_RF) memset(wdSpectrumPrev, 0, sizeof(wdSpectrumPrev));
        drawWdTabs(); drawWdList();
      }
      return;
    }
    // List area: upper/lower third scrolls
    if (ty >= WD_LIST_TOP && ty < WD_LIST_BOT) {
      int zone = (ty - WD_LIST_TOP) * 3 / (WD_LIST_BOT - WD_LIST_TOP);
      if      (zone == 0) wdScroll = max(0, wdScroll - 1);
      else if (zone == 2) wdScroll++;
      drawWdList(); drawWdStatus();
    }
    return;
  }

  // LOGS full-screen touch handling
  if (currentMode == LOGS) {
    if (tx < 65 && ty < TITLE_H) { activateMode(OFF); return; }
    if (logsSelFile < 0) {
      // File list — tap a row to open
      if (ty >= TITLE_H + 16) {
        int row = (ty - TITLE_H - 16) / LOGS_ROW_H + logsFileScroll;
        if (row >= 0 && row < logFileCount) {
          logsSelFile = row; logsScroll = 0;
          logsLoadEntries(row);
          drawLogsEntries();
        }
      }
    } else {
      // Entry list — "< FILES" tap
      if (tx < 58 && ty >= TITLE_H && ty < TITLE_H + 16) {
        logsSelFile = -1; logsScroll = 0; drawLogsFileList(); return;
      }
      // Upper/lower third scrolls
      int listTop = TITLE_H + 16;
      if (ty >= listTop) {
        int zone = (ty - listTop) * 3 / (290 - listTop);
        if      (zone == 0) logsScroll = max(0, logsScroll - 1);
        else if (zone == 2) logsScroll++;
        drawLogsEntries();
      }
    }
    return;
  }

  // Other full-screen modes: only < HOME exits
  if (currentMode == SPECTRUM || currentMode == NETSCAN || isJamMode(currentMode)) {
    if (tx < 65 && ty < TITLE_H)
      activateMode(OFF);
    return;
  }

  // Home grid — check page-flip zone first (right edge of title bar)
  if (ty < TITLE_H && tx >= 395 && tx < 430) {
    uiPage = 1 - uiPage;
    drawUI();
    return;
  }

  // Button grid (page-aware)
  int bStart = (uiPage == 0) ? 0 : 6;
  int bCount = (uiPage == 0) ? 6 : 2;
  for (int i = bStart; i < bStart + bCount; i++) {
    // Map logical button to visual position
    int vi   = (uiPage == 0) ? i : (i - 6);
    int bx   = (vi % 3) * BTN_W;
    int by   = TITLE_H + (vi / 3) * BTN_H;
    if (tx >= bx && tx < bx + BTN_W && ty >= by && ty < by + BTN_H) {
      if (buttons[i].mode == currentMode) activateMode(OFF);
      else                                activateMode(buttons[i].mode);
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
  else if (inputString == "mode:spectrum")     { activateMode(SPECTRUM); }
  else if (inputString == "mode:netscan")      { activateMode(NETSCAN); }
  else if (inputString == "default:off")       { saveDefaultMode(OFF); }
  else if (inputString == "default:wifi")      { saveDefaultMode(WIFI); }
  else if (inputString == "default:bluetooth") { saveDefaultMode(BLUETOOTH); }
  else if (inputString == "default:ble")       { saveDefaultMode(BLE); }
  else if (inputString == "default:jamtime")   { saveDefaultMode(JAMTIME); }
  else if (inputString.startsWith("diag:")) {
    int ch = inputString.substring(5).toInt();
    if (ch >= 0 && ch <= 125) {
      activateMode(OFF);
      if (radioAok) cwOnChannel(radioA, selectRadioA, (uint8_t)ch);
      Serial.printf("DIAG: Radio A CW on ch %d (%d MHz)\n", ch, 2400 + ch);
    }
  }
  else if (inputString.startsWith("diagb:")) {
    int ch = inputString.substring(6).toInt();
    if (ch >= 0 && ch <= 125) {
      activateMode(OFF);
      if (radioBok) cwOnChannel(radioB, selectRadioB, (uint8_t)ch);
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

  // 1. I2C for TCA9554 IO expander + FT6336 touch + AXP2101 PMIC
  Wire.begin(I2C_SDA, I2C_SCL);

  // 2. Init AXP2101 PMIC (PWR button)
  pmuOk = PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
  if (pmuOk) {
    PMU.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    PMU.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    PMU.clearIrqStatus();
  }

  // 3. Init TCA9554 at 0x20
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

  // SD card probe — GPIO 9/10/11 (SDMMC 1-bit, shared with Radio A).
  // Probe before Radio A init, then unmount so the pins are free for Radio A.
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  sdOk = SD_MMC.begin("/sdcard", true /*1-bit mode*/);
  if (sdOk) {
    if (!SD_MMC.exists("/sdcard/jester")) SD_MMC.mkdir("/sdcard/jester");
    Serial.println("SD OK");
  } else {
    Serial.println("SD not found — wardrive logging disabled");
  }
  SD_MMC.end();  // release GPIO 9/10/11 back to Radio A

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

  Serial.println("Initializing Radio A (CE=" + String(NRF_CE_A) + " CSN=" + String(NRF_CSN_A) +
                 " SCK=" + String(NRF_CLK_A) + " MOSI=" + String(NRF_MOSI_A) +
                 " MISO=" + String(NRF_MISO_A) + ")...");
  radioAok = configureRadio(radioA, selectRadioA);
  Serial.println("Radio A: " + String(radioAok ? "OK" : "FAIL"));
  delay(10);
  Serial.println("Initializing Radio B (CE=" + String(NRF_CE_B) + " CSN=" + String(NRF_CSN_B) +
                 " SCK=" + String(NRF_CLK_B) + " MOSI=" + String(NRF_MOSI_B) +
                 " MISO=" + String(NRF_MISO_B) + ")...");
  radioBok = configureRadio(radioB, selectRadioB);
  Serial.println("Radio B: " + String(radioBok ? "OK" : "FAIL"));
  drawUI();  // redraw to update status dots

  Serial.println("The Gizmo — Waveshare ESP32-S3-Touch-LCD-3.5-C");
  Serial.println("Mode: " + getModeString(currentMode));
  sendCurrentMode();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void serialEvent();  // forward decl — defined below

void loop() {
  handlePowerButton();
  if (!deviceOn) return;
  handleTouch();
  executeMode();
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
