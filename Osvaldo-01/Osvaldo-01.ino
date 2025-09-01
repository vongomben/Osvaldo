// Meshita Sept 2025
// Osvaldo 01
// Printer v 0.10.2 (off-grid + boot/double rainbow)
// Meshtastic -> Thermal Printer (wrapping + upsideDown fix, no-CR) + LED rainbow with fade-out
//
// CHangelog: 1 rainbow at startup; 2 rainbows when Meshtastic radio answers (connected_callback)

#include <FastLED.h>
#include <Meshtastic.h>
#include "Adafruit_Thermal.h"
#include <esp_system.h>   // esp_reset_reason()

// ====== CONFIG ======
const bool   UPSIDE_DOWN         = true;   // true: upsideDownOn() & inverted lines
const uint8_t INTER_MESSAGE_FEED = 3;      // blank lines between messages
const uint8_t PRN_COLS           = 32;     // columns per row (font ‘S’ typically 32)

// Power
const uint16_t POWER_GOOD_MS     = 600;    // waiting to stabilize the bucks before printing
const uint16_t PRN_AFTER_UART_MS = 150;    // pause after Serial2.begin() before printer.begin()

// Diagnostic reset
#define DIAG_PRINT_TO_PRINTER 0   // 0=Serial only; 1=also print to printer (after init)
RTC_DATA_ATTR uint32_t bootCounter = 0;    // boot count as long as it remains powered

// ====== LED Stuff ======
#define DATA_PIN      D1
#define DATA_PIN2     D0
#define LED_TYPE      WS2812
#define COLOR_ORDER   GRB
#define NUM_LEDS      64

CRGB leds[NUM_LEDS];
CRGB leds2[NUM_LEDS];

#define BRIGHTNESS         96
#define FRAMES_PER_SECOND  120

// ====== Meshtastic UART (XIAO ESP32S3) ======
#define SERIAL_RX_PIN 44   // GPIO44 (RX from Meshtastic device)
#define SERIAL_TX_PIN 43   // GPIO43 (TX to Meshtastic device)
#define MESH_BAUD     9600

// ====== Thermal printer UART (Serial2) ======
// Note: Serial2.begin(baud, config, RX, TX) — the correct order is (RX, TX)!
#define PRN_RX   D4   // to   printer TX
#define PRN_TX   D5   // to printer RX
#define PRN_BAUD 19200

// ====== Periodic sending (optional) ======
#define SEND_PERIOD 300  // s
uint32_t next_send_time = 0;
bool not_yet_connected = true;

// ====== LED status / Effects ======
enum LedState { IDLE, RAINBOW, FADEOUT };
LedState ledState = IDLE;

bool startRainbowPending = false;
uint32_t rainbowStart = 0;
uint32_t fadeStart = 0;

const uint32_t rainbowDuration = 3000;  // ms
const uint32_t fadeDuration    = 1000;  // ms
uint8_t gHue = 0;
const uint8_t baseBrightness = BRIGHTNESS;

// ====== Printer ======
Adafruit_Thermal printer(&Serial2);

// ====== Coda stampa ======
const int PRINT_QUEUE_SIZE = 8;
String printQueue[PRINT_QUEUE_SIZE];
int qHead = 0, qTail = 0;

bool enqueuePrint(const String& s) {
  int next = (qHead + 1) % PRINT_QUEUE_SIZE;
  if (next == qTail) return false; // coda piena
  printQueue[qHead] = s;
  qHead = next;
  return true;
}
bool dequeuePrint(String &out) {
  if (qTail == qHead) return false; // vuota
  out = printQueue[qTail];
  qTail = (qTail + 1) % PRINT_QUEUE_SIZE;
  return true;
}

// --- helper: remove only spaces/tabs at the end of the line ---
static String rtrim(const String& in) {
  int end = in.length() - 1;
  while (end >= 0) {
    char c = in[end];
    if (c == ' ' || c == '\t') end--;
    else break;
  }
  if (end < 0) return String();
  return in.substring(0, end + 1);
}

// --- helper: normalizes common characters and terminators ---
static void normalizeCommon(String &s) {
  s.replace("\r\n", "\n");
  s.replace("\r", "\n");
  s.replace("\xC2\xA0", " ");   // NBSP -> space
  s.replace("\xE2\x80\x99", "'"); // ’
  s.replace("\xE2\x80\x98", "'"); // ‘
  s.replace("\xE2\x80\x9C", "\""); // “
  s.replace("\xE2\x80\x9D", "\""); // ”
}

// --- optional: UTF-8 accents -> “safe” ASCII (comment if you want to keep accents) ---
static String asciiFallback(String s) {
  s.replace("\xC3\xA0", "a"); s.replace("\xC3\xA8", "e"); s.replace("\xC3\xAC", "i");
  s.replace("\xC3\xB2", "o"); s.replace("\xC3\xB9", "u"); s.replace("\xC3\xA9", "e");
  s.replace("\xC3\x80", "A"); s.replace("\xC3\x88", "E"); s.replace("\xC3\x8C", "I");
  s.replace("\xC3\x92", "O"); s.replace("\xC3\x99", "U"); s.replace("\xC3\x89", "E");
  return s;
}

// --- helper: wrap a “logical” row with fixed columns, respecting the words ---
static void wrapLineToCols(const String& src, uint8_t cols, String outLines[], int &count, int maxLines) {
  int i = 0, N = src.length();
  while (i < N && count < maxLines) {
    while (i < N && src[i] == ' ') i++;
    int end = i + cols;
    if (end >= N) { outLines[count++] = rtrim(src.substring(i)); break; }
    int j = end;
    while (j > i && src[j] != ' ' && src[j] != '\t') j--;
    if (j == i) { outLines[count++] = rtrim(src.substring(i, end)); i = end; }
    else        { outLines[count++] = rtrim(src.substring(i, j));  i = j + 1; }
  }
}

// --- robust multi-line printing with PRN_COLS wrapping and upsideDown compensation ---
void printerPrintParagraph(String s) {
  normalizeCommon(s);
  // Comment if you want to keep the accents real:
  s = asciiFallback(s);

  const int MAX_LINES = 160;
  String logical[MAX_LINES];
  int ln = 0, start = 0;
  while (ln < MAX_LINES && start <= s.length()) {
    int nl = s.indexOf('\n', start);
    if (nl == -1) { logical[ln++] = rtrim(s.substring(start)); break; }
    logical[ln++] = rtrim(s.substring(start, nl));
    start = nl + 1;
  }
  if (ln > 0 && logical[ln - 1].length() == 0) ln--;

  String wrapped[MAX_LINES];
  int wn = 0;
  for (int i = 0; i < ln && wn < MAX_LINES; ++i) {
    if (logical[i].length() == 0) { wrapped[wn++] = ""; continue; }
    wrapLineToCols(logical[i], PRN_COLS, wrapped, wn, MAX_LINES);
  }

  String out; out.reserve(s.length() + wn + 8);
  if (UPSIDE_DOWN) { for (int i = wn - 1; i >= 0; --i) { out += wrapped[i]; if (i > 0) out += '\n'; } }
  else             { for (int i = 0; i < wn; ++i)     { out += wrapped[i]; if (i < wn - 1) out += '\n'; } }

  if (out.length() > 0) { printer.print(out); printer.write('\n'); }
  printer.feed(INTER_MESSAGE_FEED);
  Serial2.flush();
  delay(20);
}

// ====== Diagnostic reset ======
static const char* resetReasonToStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT_PIN";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    default:                return "UNKNOWN";
  }
}

void diagPrintReset(const char* where) {
  esp_reset_reason_t r = esp_reset_reason();
  bootCounter++;
  String msg = String("[DIAG] ") + where + " reset=" + resetReasonToStr(r) + " boot#=" + String(bootCounter);
  if (Serial) { Serial.println(msg); }
#if DIAG_PRINT_TO_PRINTER
  // Stampa solo se la stampante è già inizializzata e alimentazione stabile
  printer.println(msg); printer.feed(1);
#endif
}

// ====== Callback Meshtastic ======
void connected_callback(mt_node_t *node, mt_nr_progress_t progress) {
  if (not_yet_connected && Serial) Serial.println("✅ Connected to Meshtastic device!");
  not_yet_connected = false;
}

void text_message_callback(uint32_t from, uint32_t to, uint8_t channel, const char* text) {
  if (Serial) {
    Serial.print("📩 RX Meshtastic  From: "); Serial.print(from);
    Serial.print("  To: "); Serial.print(to);
    Serial.print("  Ch: "); Serial.print(channel);
    Serial.print("  Text: "); Serial.println(text);
  }
  startRainbowPending = true; // avvia LED
  String line = String(text);
  if (!enqueuePrint(line) && Serial) Serial.println("⚠️ Print queue full, dropping message.");
}

// ====== Setup ======
void setup() {
  Serial.begin(115200);
  // NO while(!Serial):  do not block in off-grid mode
  diagPrintReset("BOOT"); // log reason for reset

  // Meshtastic
  mt_serial_init(SERIAL_RX_PIN, SERIAL_TX_PIN, MESH_BAUD);
  mt_set_debug(false); // less spam
  mt_request_node_report(connected_callback);
  set_text_message_callback(text_message_callback);

  // LED
  delay(3000); // safety delay for LED
  FastLED.addLeds<LED_TYPE, DATA_PIN,  COLOR_ORDER>(leds,  NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.addLeds<LED_TYPE, DATA_PIN2, COLOR_ORDER>(leds2, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(baseBrightness);
  FastLED.clear(); FastLED.show();

  // POWER-GOOD for regulators/lines before touching the printer
  delay(POWER_GOOD_MS);

  // Printer UART (ordine: RX,TX)
  Serial2.begin(PRN_BAUD, SERIAL_8N1, PRN_RX, PRN_TX);
  delay(PRN_AFTER_UART_MS);

  // Initialize printer (softer timing to reduce peaks)
  printer.begin();
  if (UPSIDE_DOWN) printer.upsideDownOn(); else printer.upsideDownOff();
  printer.wake();
  printer.setDefault();
  printer.setSize('S');    // max coloumns
  printer.justify('L');    // left align
  printer.setTimes(60, 6); // <-- REDUCED PEAKS compared to 80,2
  printer.println(F("Printer ready."));
  printer.feed(1);

#if DIAG_PRINT_TO_PRINTER
  // The power supply should now be stable: print the reset reason.
  diagPrintReset("POST-PRN");
#endif

  if (Serial) Serial.println("Ready.");
}

// ====== Rainbow effect on both strips ======
void rainbow() {
  fill_rainbow(leds,  NUM_LEDS, gHue, 7);
  fill_rainbow(leds2, NUM_LEDS, gHue, 7);
}

// ====== Main Loop ======
void loop() {
  uint32_t now = millis();

  // Process Meshtastic (triggers callbacks)
  bool can_send = mt_loop(now);

  // Start LED on new received message
  if (startRainbowPending) {
    startRainbowPending = false;
    ledState = RAINBOW;
    rainbowStart = now;
    gHue = 0;
    FastLED.setBrightness(baseBrightness);
    if (Serial) Serial.println("🌈 Rainbow START");
  }

  // LED Management
  switch (ledState) {
    case IDLE: break;
    case RAINBOW:
      gHue += 1;  rainbow();  FastLED.show();  FastLED.delay(1000 / FRAMES_PER_SECOND);
      if (now - rainbowStart >= rainbowDuration) { ledState = FADEOUT; fadeStart = now; if (Serial) Serial.println("🌗 Fade-out START"); }
      break;
    case FADEOUT: {
      uint32_t t = now - fadeStart;
      if (t >= fadeDuration) { FastLED.clear(); FastLED.setBrightness(baseBrightness); FastLED.show(); ledState = IDLE; if (Serial) Serial.println("🟤 Fade-out STOP (off)"); }
      else {
        float progress = (float)t / (float)fadeDuration;
        uint8_t b = (uint8_t)((1.0f - progress) * baseBrightness); if (b == 0) b = 1;
        FastLED.setBrightness(b); FastLED.show(); FastLED.delay(1000 / FRAMES_PER_SECOND);
      }
    } break;
  }

  // Print: empty the queue (one per iteration so as not to block the LEDs)
  String toPrint;
  if (dequeuePrint(toPrint)) {
    printer.wake();
    printer.setDefault();
    printer.setSize('S');
    printer.justify('L');
    printerPrintParagraph(toPrint);
    if (Serial) { Serial.print("🖨️ Printed: "); Serial.println(toPrint); }
  }

  // (optional) periodic broadcast sending
 // if (can_send && now >= next_send_time) {
 //   mt_send_text("Hello from Xiao ESP32!", BROADCAST_ADDR, 0);
 //   next_send_time = now + SEND_PERIOD * 1000;
 // }
}
