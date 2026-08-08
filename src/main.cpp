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
#include "esp_wifi.h"
#include "Preferences.h"
#include <Arduino_GFX_Library.h>
#include "TCA9554.h"
#include "esp_rom_gpio.h"
#include "soc/spi_periph.h"

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
#define COL_BLE      0x041Fu  // blue
#define COL_BT       0xF800u  // red
#define COL_JAM      0xFC00u  // orange — JAM TIME
#define COL_ACTIVE   0xFFE0u  // yellow highlight border
#define COL_GREEN    0x07E0u  // radio OK
#define COL_RED      0xF800u  // radio FAIL
#define COL_SPECTRUM 0xC01Fu  // violet — spectrum analyzer
#define COL_NETSCAN  0x0480u  // teal-green — network scanner

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

// BLE sweep counters — same band, both radios from opposite ends
static uint8_t bleSweepA = 2;
static uint8_t bleSweepB = 80;

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
enum Mode { OFF, WIFI, BLUETOOTH, BLE, JAMTIME, SPECTRUM, NETSCAN };
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

// Row 1 (y=40,  h=140): WIFI 2.4 | BLE | BLUETOOTH    (160 px each)
// Row 2 (y=180, h=140): JAM TIME | SPECTRUM | NET SCAN (160 px each)
static Button buttons[6] = {
  {0,       TITLE_H,         BTN_W, BTN_H, COL_WIFI,     "WIFI 2.4",  WIFI},
  {BTN_W,   TITLE_H,         BTN_W, BTN_H, COL_BLE,      "BLE",       BLE},
  {2*BTN_W, TITLE_H,         BTN_W, BTN_H, COL_BT,       "BLUETOOTH", BLUETOOTH},
  {0,       TITLE_H + BTN_H, BTN_W, BTN_H, COL_JAM,      "JAM TIME",  JAMTIME},
  {BTN_W,   TITLE_H + BTN_H, BTN_W, BTN_H, COL_SPECTRUM, "SPECTRUM",  SPECTRUM},
  {2*BTN_W, TITLE_H + BTN_H, BTN_W, BTN_H, COL_NETSCAN,  "NET SCAN",  NETSCAN},
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
    case WIFI:       return "WIFI";
    case BLUETOOTH:  return "BLUETOOTH";
    case BLE:        return "BLE";
    case JAMTIME:    return "JAMTIME";
    case SPECTRUM:   return "SPECTRUM";
    case NETSCAN:    return "NETSCAN";
    default:         return "UNKNOWN";
  }
}

void saveDefaultMode(Mode mode) {
  if (mode == SPECTRUM || mode == NETSCAN) return;
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
  for (int i = 0; i < 6; i++) {
    drawButton(i, buttons[i].mode == currentMode);
  }
}

// Redraw only the buttons (no full screen clear) to avoid flicker
void refreshButtons() {
  for (int i = 0; i < 6; i++) {
    drawButton(i, buttons[i].mode == currentMode);
  }
}

// ---------------------------------------------------------------------------
// Radio config
// ---------------------------------------------------------------------------
// Remap SPI3 (HSPI) signals through the GPIO matrix — no end()/begin(), no DMA teardown.
// spiHSPI is initialized once in setup(); only the physical pin routing changes here.
void selectRadioA() {
  esp_rom_gpio_connect_out_signal(NRF_CLK_A,  spi_periph_signal[HSPI].spiclk_out, false, false);
  esp_rom_gpio_connect_out_signal(NRF_MOSI_A, spi_periph_signal[HSPI].spid_out,   false, false);
  esp_rom_gpio_connect_in_signal (NRF_MISO_A, spi_periph_signal[HSPI].spiq_in,    false);
}

void selectRadioB() {
  esp_rom_gpio_connect_out_signal(NRF_CLK_B,  spi_periph_signal[HSPI].spiclk_out, false, false);
  esp_rom_gpio_connect_out_signal(NRF_MOSI_B, spi_periph_signal[HSPI].spid_out,   false, false);
  esp_rom_gpio_connect_in_signal (NRF_MISO_B, spi_periph_signal[HSPI].spiq_in,    false);
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

// BLE: 40 channels spanning NRF24 ch 2–80 (adv at 2, 26, 80; data across the band).
// Both radios sweep from opposite ends, 1 ch/call each → full band in ~40 calls (~17ms).
// Advertising channels get 6 pkts when hit (vs 3 for data) to also block new connections.
// Prior approach only hit adv channels from Radio A + slow Radio B data sweep (~74ms cycle)
// which missed established connections entirely — this sweeps the full band like jamBluetooth.
void jamBLE() {
  bool isAdvA = (bleSweepA == 2 || bleSweepA == 26 || bleSweepA == 80);
  bool isAdvB = (bleSweepB == 2 || bleSweepB == 26 || bleSweepB == 80);
  if (radioAok) spamChannel(radioA, selectRadioA, bleSweepA, isAdvA ? 6 : 3);
  if (radioBok) spamChannel(radioB, selectRadioB, bleSweepB, isAdvB ? 6 : 3);
  bleSweepA++; if (bleSweepA > 80) bleSweepA = 2;
  bleSweepB--; if (bleSweepB < 2)  bleSweepB = 80;
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
// JAM TIME — Brute Force Full-Band Saturator
// Both radios blast from opposite ends, 12 packets per channel per pass.
// In a 12ft room at PA_MAX this will saturate the entire 2.4 GHz band.
// No scanning, no wasted RX time — 100% transmit duty cycle.
// ---------------------------------------------------------------------------
static uint8_t jamSweepA = 0;
static uint8_t jamSweepB = 125;

void jamAll() {
  if (radioAok) spamChannel(radioA, selectRadioA, jamSweepA, 5);
  if (radioBok) spamChannel(radioB, selectRadioB, jamSweepB, 5);
  jamSweepA = (jamSweepA + 1) % 126;
  jamSweepB = (jamSweepB == 0) ? 125 : jamSweepB - 1;
}

// ---------------------------------------------------------------------------
// SPECTRUM mode — full-screen NRF24 RPD sweep with yellow peak-hold markers
// Back button in title bar: tap x=0-64, y=0-39 to return to main menu.
// ---------------------------------------------------------------------------

static void drawBackButton() {
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
  gfx->drawFastHLine(0, COMBO_CHART_BOT + 1, 480, 0x4208);

  // Network panel header
  gfx->setTextColor(0x8410);
  gfx->setCursor(2,   182); gfx->print("SSID");
  gfx->setCursor(256, 182); gfx->print("CH");
  gfx->setCursor(292, 182); gfx->print("RSSI");
  gfx->setCursor(360, 182); gfx->print("AUTH");
  gfx->drawFastHLine(0, 191, 480, 0x2104);

  // Initial scanning placeholder
  gfx->setTextColor(0x4208);
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
      gfx->fillRect(2 + b * 8, y + 4, 6, 10, (b < bars) ? col : (uint16_t)0x2104);

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
  gfx->setTextColor(0x4208);
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
    case OFF:        delay(50);       break;
    case WIFI:       jamWifi();       break;
    case BLE:        jamBLE();        break;
    case BLUETOOTH:  jamBluetooth();  break;
    case JAMTIME:    jamAll();        break;
    case SPECTRUM:   runSpectrum();   break;
    case NETSCAN:    runNetScan();    break;
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

  // Full-screen modes: only the < HOME button exits
  if (currentMode == SPECTRUM || currentMode == NETSCAN) {
    if (tx < 65 && ty < TITLE_H)
      activateMode(OFF);
    return;
  }

  for (int i = 0; i < 6; i++) {
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

  // Initialize SPI3 (HSPI) once — selectRadioA/B only remap GPIO matrix from here on
  spiHSPI.begin(NRF_CLK_A, NRF_MISO_A, NRF_MOSI_A, -1);

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
