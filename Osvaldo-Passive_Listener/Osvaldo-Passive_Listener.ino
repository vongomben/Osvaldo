/*
  Meshtastic Serial-only passive listener
  - Connects via UART to a Meshtastic node (Protobuf API)
  - Prints any received text messages to USB Serial
  - Does NOT send anything
*/

#include <Meshtastic.h>

// --- USB serial for logs ---
#define USB_BAUD        9600

// --- UART to Meshtastic device ---
#define SERIAL_RX_PIN   44        // GPIO44 (RX from Meshtastic)
#define SERIAL_TX_PIN   43        // GPIO43 (TX to Meshtastic)
#define MESH_BAUD       9600      // Match your node setting

// --- Helper: strip non-printable for cleaner logs ---
static void print_clean(const char* s) {
  while (*s) {
    char c = *s++;
    if ((c >= 32 && c <= 126) || c == '\n' || c == '\r' || c == '\t') {
      Serial.print(c);
    } else {
      Serial.print('?');
    }
  }
}

// --- Text message RX callback ---
void text_message_callback(uint32_t from, uint32_t to, uint8_t channel, const char* text) {
  Serial.println("=== RX TEXT MESSAGE ===");
  Serial.print("From: ");    Serial.println(from);
  Serial.print("To: ");      Serial.println(to);
  Serial.print("Channel: "); Serial.println(channel);
  Serial.print("Text: \"");  print_clean(text); Serial.println("\"");
  Serial.println("=======================");
}

void setup() {
  Serial.begin(USB_BAUD);
  delay(50);
  Serial.println();
  Serial.println("=== Meshtastic Serial-only passive listener ===");
  Serial.print("UART RX="); Serial.print(SERIAL_RX_PIN);
  Serial.print(" TX=");     Serial.print(SERIAL_TX_PIN);
  Serial.print(" Baud=");   Serial.println(MESH_BAUD);

  // Init UART link
  mt_serial_init(SERIAL_RX_PIN, SERIAL_TX_PIN, MESH_BAUD);

  // Optional: set to true if you want to see heartbeats etc.
  mt_set_debug(false);

  // Register callback for text messages
  set_text_message_callback(text_message_callback);

  Serial.println("Setup complete. Listening for incoming text messages...");
}

void loop() {
  uint32_t now = millis();
  mt_loop(now);  // drive state machine, triggers callbacks on RX
}
