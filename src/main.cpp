#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include "web_portal.h"
#include <time.h>
#include "esp_sntp.h"
#include <HTTPClient.h>

// --- Headless Mode Configuration ---
#define ENABLE_SCREEN false  // Toggle this true later to add screen support

// --- Hardware Config: Waveshare Audio Board ---
#define LED_PIN 1           // Onboard RGB Ring pin
#define LED_COUNT 7         // 7 LEDs in the ring
#define NFC_SDA 11          // Main I2C SDA
#define NFC_SCL 10          // Main I2C SCL
#define NFC_IRQ 12          // Assign to any free pin (e.g. GPIO 12)
#define LCD_RST 13          // Assign to any free pin (e.g. GPIO 13)

// --- Animation Speed Variables ---
#define SPEED_COMET 30       
#define SPEED_FILL  45       
#define SPEED_PULSE 15       
#define SPEED_MANUAL 60      

TwoWire I2C_NFC = TwoWire(1); 
Adafruit_PN532 nfc(NFC_IRQ, LCD_RST, &I2C_NFC); 
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences prefs;

BandRecord registeredBands[50];
int bandCount = 0;
bool isSuccessActive = false;
bool isWaitingForUID = false;

// -------------------------------------------------------------------------------
// Helper Functions (Modified to be Headless)
// -------------------------------------------------------------------------------

void runWhiteSwirl(int speed) {
    static uint8_t swirlPos = 0;
    strip.clear();
    for(int i=0; i<3; i++) { // Reduced tail for 7 LEDs
        int p = (swirlPos - i + LED_COUNT) % LED_COUNT;
        int b = 255 - (i * 80); if(b < 0) b = 0;
        strip.setPixelColor(p, strip.Color(b, b, b)); 
    }
    strip.show();
    swirlPos = (swirlPos + 1) % LED_COUNT;
}

void handleSuccess(uint32_t color) {
    if(isSuccessActive) return;
    isSuccessActive = true;

    // STAGE 1: White Comet Swirl
    for(int f=0; f < LED_COUNT; f++){
        strip.clear();
        for(int i=0; i<3; i++){
            int p=(f-i+LED_COUNT)%LED_COUNT;
            int b=255-(i*80); if(b<0) b=0;
            strip.setPixelColor(p, strip.Color(b, b, b)); 
        }
        strip.show(); delay(SPEED_COMET);
    }
    // STAGE 2: Progressive Fill
    strip.clear();
    for(int i=0; i < LED_COUNT; i++) {
        strip.setPixelColor(i, strip.Color(255, 255, 255));
        strip.show(); delay(SPEED_FILL);
    }
    // STAGE 3: Pulse Breathing
    for(int pulse = 0; pulse < 2; pulse++){
        for(int b = 30; b <= 255; b += 10){ strip.fill(strip.gamma32(color)); strip.setBrightness(b); strip.show(); delay(SPEED_PULSE); }
        for(int b = 255; b >= 30; b -= 10){ strip.fill(strip.gamma32(color)); strip.setBrightness(b); strip.show(); delay(SPEED_PULSE); }
    }
    strip.clear(); strip.setBrightness(40); strip.show();
    isSuccessActive = false;
}

void setup() {
    setCpuFrequencyMhz(240);
    Serial.begin(115200);

    // 1. Load Bands from NVS
    prefs.begin("mbands", true);
    int rc = prefs.getInt("count", 0); 
    bandCount = (rc < 0 || rc > 50) ? 0 : rc;
    for(int i=0; i<bandCount; i++) {
        prefs.getBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
    }
    prefs.end();

    // 2. Initialize NeoPixels
    strip.begin(); 
    strip.setBrightness(40); 
    strip.show(); // Turn off initially

    // 3. Initialize NFC
    I2C_NFC.begin(NFC_SDA, NFC_SCL, 100000);
    if(nfc.begin()) {
        nfc.SAMConfig();
        Serial.println("NFC Reader Initialized!");
    } else {
        Serial.println("NFC Reader NOT FOUND!");
        strip.fill(strip.Color(255, 0, 0)); strip.show(); // Red ring for error
    }

    // 4. Initialize Network & Web Portal
    initWebServer(); 
    if (tryConnectSavedWiFi()) { 
        startWebServer(); 
        Serial.println("WiFi Connected: " + WiFi.localIP().toString());
        strip.fill(strip.Color(0, 255, 0)); strip.show(); delay(500); strip.clear(); strip.show();
    }
}

void loop() {
    // Process Web Server requests
    handleWebPortal();

    // Handle Scan Animations (Arming via Web UI)
    if ((g_webScanArmed && !g_webScanHasResult) || isWaitingForUID) {
        static uint32_t la = 0;
        if (millis() - la > SPEED_MANUAL) { la = millis(); runWhiteSwirl(SPEED_MANUAL); }
    }

    // NFC Scanning Logic
    static uint32_t lastScanCheck = 0;
    if (millis() - lastScanCheck > 400) {
        lastScanCheck = millis();
        uint8_t uid[7], len;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 50)) {
            
            // Logic to process scan for Web Portal (Arming)
            if(g_webScanArmed) {
                // ... (Logic from your existing main.cpp to stage record)
                g_webScanHasResult = true;
            }

            // Check if band is registered
            int idx = -1;
            for (int i = 0; i < bandCount; i++) {
                if (memcmp(uid, registeredBands[i].uid, 7) == 0) { idx = i; break; }
            }

            if (idx != -1) {
                handleSuccess(registeredBands[idx].color); 
            } else {
                // Unknown band - brief yellow flash
                strip.fill(strip.Color(255, 255, 0)); strip.show(); delay(200); strip.clear(); strip.show();
            }
        }
    }
}