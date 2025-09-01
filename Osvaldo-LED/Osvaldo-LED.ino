// LED Self-Test for XIAO ESP32S3 + two WS2812 strips
// - Drives two strips on pins D1 and D0
// - Shows a color sanity cycle (R, G, B, W, OFF) and a moving rainbow
// - No Meshtastic nor printer libraries required

#include <FastLED.h>

// ---------- Hardware setup ----------
#define LED_TYPE      WS2812
#define COLOR_ORDER   GRB
#define DATA_PIN_1    D1         // strip #1 data pin
#define DATA_PIN_2    D0         // strip #2 data pin
#define NUM_LEDS      64         // adjust to your strip length
#define BRIGHTNESS    96
#define FPS           120

CRGB leds1[NUM_LEDS];
CRGB leds2[NUM_LEDS];

// ---------- Test timing ----------
const uint16_t STEP_MS = 1000;   // duration for each solid color step
const uint32_t RAINBOW_MS = 8000;// how long to run the rainbow phase

// ---------- State ----------
enum Phase { SOLID_TEST, RAINBOW_TEST, DONE };
Phase phase = SOLID_TEST;
uint32_t phaseStart = 0;
uint8_t  colorStep = 0;          // 0=Red, 1=Green, 2=Blue, 3=White, 4=Off
uint8_t  gHue = 0;

void solidFillBoth(const CRGB &c) {
  fill_solid(leds1, NUM_LEDS, c);
  fill_solid(leds2, NUM_LEDS, c);
  FastLED.show();
}

void setup() {
  // Optional: Serial for logs (won't block if no USB is attached)
  Serial.begin(115200);
  if (Serial) Serial.println("LED Self-Test starting...");

  FastLED.addLeds<LED_TYPE, DATA_PIN_1, COLOR_ORDER>(leds1, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.addLeds<LED_TYPE, DATA_PIN_2, COLOR_ORDER>(leds2, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear(true);

  phase = SOLID_TEST;
  phaseStart = millis();
  colorStep = 0;
}

void loop() {
  uint32_t now = millis();

  switch (phase) {
    case SOLID_TEST: {
      // Cycle through: Red -> Green -> Blue -> White -> Off
      if (now - phaseStart >= STEP_MS) {
        phaseStart = now;
        colorStep++;
      }
      if (colorStep > 4) {
        // Move to rainbow phase
        phase = RAINBOW_TEST;
        phaseStart = now;
        gHue = 0;
        if (Serial) Serial.println("Rainbow phase...");
      } else {
        CRGB c = CRGB::Black;
        if      (colorStep == 0) c = CRGB::Red;
        else if (colorStep == 1) c = CRGB::Green;
        else if (colorStep == 2) c = CRGB::Blue;
        else if (colorStep == 3) c = CRGB::White;
        else                     c = CRGB::Black; // step 4
        solidFillBoth(c);
      }
    } break;

    case RAINBOW_TEST: {
      // Animated rainbow on both strips
      gHue += 1; // animation speed
      fill_rainbow(leds1, NUM_LEDS, gHue, 7);
      fill_rainbow(leds2, NUM_LEDS, gHue, 7);
      FastLED.show();
      FastLED.delay(1000 / FPS);

      if (now - phaseStart >= RAINBOW_MS) {
        phase = DONE;
        FastLED.clear(true);
        if (Serial) Serial.println("LED Self-Test done.");
      }
    } break;

    case DONE:
      // Idle (kept off). You can loop or restart if desired.
      FastLED.delay(1000 / FPS);
      break;
  }
}
