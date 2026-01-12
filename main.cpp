#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <Adafruit_NeoPixel.h>

// Breakout Pin Definitions
#define NFC_SDA 15
#define NFC_SCL 16
#define NFC_IRQ 17
#define LED_PIN 21
#define LED_COUNT 12

TwoWire I2C_NFC = TwoWire(1);
Adafruit_PN532 nfc(NFC_IRQ, 255, &I2C_NFC);
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
    Serial.begin(115200);
    
    // 1. Start LEDs (Dim for stability)
    strip.begin();
    strip.setBrightness(30); 
    strip.fill(strip.Color(0, 0, 50)); // Start Blue
    strip.show();

    delay(1000); // Allow power to settle

    // 2. Initialize the Breakout I2C Bus
    I2C_NFC.begin(NFC_SDA, NFC_SCL, 100000); // 100kHz for stability
    I2C_NFC.setTimeOut(100);                 // Prevent bus hangs

    if (nfc.begin()) {
        nfc.SAMConfig();
        Serial.println("Sandbox Ready: NFC Reader Found!");
        strip.fill(strip.Color(0, 50, 0)); // Turn Green when ready
        strip.show();
    } else {
        Serial.println("NFC Reader NOT found. Check wiring.");
    }
}

void loop() {
    // Add logic for testing only one feature at a time here
}