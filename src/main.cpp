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

struct _lv_event_t; 

// --- Hardware Pins (Waveshare Audio Board) ---
#define LED_PIN 38       
#define LED_COUNT 7      
#define NFC_SDA 11       
#define NFC_SCL 10       
#define NFC_IRQ 6        
#define NFC_RST 7        

// Animation Speeds
#define SPEED_COMET 40       
#define SPEED_FILL  50       
#define SPEED_PULSE 20       

TwoWire I2C_NFC = TwoWire(1); 
Adafruit_PN532 nfc(NFC_IRQ, NFC_RST, &I2C_NFC); 

// Initialized as RGB. Note: If colors still look wrong (e.g., Red and Green swapped), 
// change NEO_RGB to NEO_GRB.
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);

Preferences prefs;
AsyncWebServer server(80); 

BandRecord registeredBands[50];
int bandCount = 0;
char ownersList[10][20]; 
char locationsList[10][20];
uint32_t lastActivityTime = 0;
uint32_t idleTimeout = 60000;   
uint32_t sleepTimeout = 300000; 
bool isWiFiActive = false;
bool isSuccessActive = false;
bool isWaitingForUID = false;
bool isScreenOn = true;         
bool mdnsStarted = false;

// Web Scan Globals
volatile bool g_webScanArmed = false;        
volatile bool g_webScanHasResult = false;    
volatile bool g_webScanIsKnown = false;      
volatile int  g_webScanKnownIndex = -1;      
volatile uint8_t g_webScanUidLen = 0;        
volatile uint8_t g_webScanUid[10] = {0};     
char g_webScanUidStr[32] = {0};              
char g_webScanTypeStr[24] = {0};             
volatile bool g_webPendingNew = false;
BandRecord g_webPendingRecord;               

// ---------------------------------------------------------------------------------------
// Linker / Web Portal Bridge
// ---------------------------------------------------------------------------------------
extern "C" {
    void loadCategoriesFromPrefs() {
        prefs.begin("mbands", false); 
        for (int i = 0; i < 10; i++) {
            if (prefs.isKey(("o" + String(i)).c_str())) {
                strncpy(ownersList[i], prefs.getString(("o" + String(i)).c_str(), "").c_str(), 19);
            } else { ownersList[i][0] = '\0'; }
            if (prefs.isKey(("l" + String(i)).c_str())) {
                strncpy(locationsList[i], prefs.getString(("l" + String(i)).c_str(), "").c_str(), 19);
            } else { locationsList[i][0] = '\0'; }
        }
        prefs.end();
    }

    void formatUidString(const uint8_t *uid, uint8_t len, char *out, size_t outSize) {
        if(!out || outSize < 4) return;
        out[0] = '\0';
        for(uint8_t i=0; i<len && (i*3+2) < outSize; i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X", uid[i]);
            strncat(out, buf, outSize - strlen(out) - 1);
            if(i < len-1) strncat(out, ":", outSize - strlen(out) - 1);
        }
    }

    void computeBandTypeFromUidStr(const char *uidStr, char *out, size_t outSize) {
        if(!out || outSize == 0) return;
        String s = String(uidStr); s.toUpperCase();
        String style = s.endsWith("90") ? "MagicBand+" : (s.endsWith("80") ? "MagicBand 2.0" : "MagicBand 1.0 / Other");
        strncpy(out, style.c_str(), outSize - 1);
    }

    void fn_refresh_roller(struct _lv_event_t * e) { }
    void web_arm_scan() { g_webScanArmed = true; g_webScanHasResult = false; g_webPendingNew = false; }
    void web_cancel_scan() { g_webScanArmed = false; }
    bool web_has_scan_result() { return g_webScanHasResult; }
    bool web_scan_result_is_known() { return g_webScanIsKnown; }
    int  web_scan_known_index() { return g_webScanKnownIndex; }
    bool web_pending_new_band() { return g_webPendingNew; }
    void web_scan_uid_string(char *out, size_t outSize) { strncpy(out, g_webScanUidStr, outSize); }
    void web_scan_type_string(char *out, size_t outSize) { strncpy(out, g_webScanTypeStr, outSize); }

    int web_confirm_save_pending_new(bool yes) {
        if(!g_webPendingNew || !yes) { g_webPendingNew = false; g_webScanArmed = false; return -1; }
        if(bandCount >= 50) return -1;
        registeredBands[bandCount] = g_webPendingRecord;
        int idx = bandCount; bandCount++;
        prefs.begin("mbands", false);
        prefs.putInt("count", bandCount);
        prefs.putBytes(("b" + String(idx)).c_str(), &registeredBands[idx], sizeof(BandRecord));
        prefs.end();
        g_webPendingNew = false; g_webScanArmed = false;
        return idx;
    }
}

// --- IO Expander: Enable Power Rails ---
void initIOExpander() {
    Wire.beginTransmission(0x20); Wire.write(0x06); Wire.write(0x00); Wire.endTransmission(); 
    Wire.beginTransmission(0x20); Wire.write(0x07); Wire.write(0x00); Wire.endTransmission(); 
    Wire.beginTransmission(0x20); Wire.write(0x02); Wire.write(0xFF); Wire.endTransmission(); 
    Wire.beginTransmission(0x20); Wire.write(0x03); Wire.write(0xFF); Wire.endTransmission(); 
}

// --- Enhanced Success Animation ---
void handleSuccess(uint32_t color) {
    if(isSuccessActive) return;
    isSuccessActive = true;
    
    // 1. Boost Brightness to Max
    strip.setBrightness(255);

    // STAGE 1: White Comet Swirl (2 rotations)
    for(int rotation = 0; rotation < 2; rotation++) {
        for(int f=0; f < LED_COUNT; f++){
            strip.clear();
            for(int i=0; i<4; i++){
                int p=(f-i+LED_COUNT)%LED_COUNT;
                int b=255-(i*60); if(b<0) b=0;
                strip.setPixelColor(p, strip.Color(b, b, b)); 
            }
            strip.show(); delay(SPEED_COMET);
        }
    }

    // STAGE 2: Progressive Color Fill
    strip.clear();
    for(int i=0; i < LED_COUNT; i++) {
        strip.setPixelColor(i, strip.gamma32(color));
        strip.show(); delay(SPEED_FILL);
    }

    // STAGE 3: Pulse Breathing
    for(int pulse = 0; pulse < 2; pulse++){
        for(int b = 50; b <= 255; b += 10){ 
            strip.fill(strip.gamma32(color)); 
            strip.setBrightness(b); 
            strip.show(); delay(SPEED_PULSE); 
        }
        for(int b = 255; b >= 50; b -= 10){ 
            strip.fill(strip.gamma32(color)); 
            strip.setBrightness(b); 
            strip.show(); delay(SPEED_PULSE); 
        }
    }

    // Reset to idle state
    strip.clear(); 
    strip.setBrightness(40); // Restore lower standby brightness
    strip.show();
    isSuccessActive = false;
}

void setup() {
    Serial.begin(115200);
    Wire.begin(11, 10); 
    initIOExpander();   

    strip.begin();
    strip.setBrightness(40);
    strip.fill(strip.Color(0, 0, 150)); // Start Blue
    strip.show();

    uint32_t startWait = millis();
    while(!Serial && (millis() - startWait < 3000));
    Serial.println("\n\n--- MAGIC BAND HUB: BOOTING ---");

    loadCategoriesFromPrefs();
    prefs.begin("mbands", false);
    int rc = prefs.getInt("count", 0); bandCount = (rc < 0 || rc > 50) ? 0 : rc;
    for(int i=0; i<bandCount; i++) {
        if(prefs.isKey(("b" + String(i)).c_str()))
            prefs.getBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
    }
    prefs.end();
    
    LittleFS.begin(true);

    I2C_NFC.begin(NFC_SDA, NFC_SCL, 100000);
    if(nfc.begin()) nfc.SAMConfig();

    initWebServer(); 
    if (tryConnectSavedWiFi()) { 
        isWiFiActive = true; 
        startWebServer(); 
        strip.fill(strip.Color(0, 150, 0)); // Green
    } else {
        WiFi.mode(WIFI_AP);
        WiFi.softAP("MagicBandHub-Setup");
        startWebServer();
        strip.fill(strip.Color(150, 80, 0)); // Orange
    }
    strip.show();
    delay(1000);
    strip.clear();
    strip.show();
}

void loop() {
    if (WiFi.status() == WL_CONNECTED && !mdnsStarted) {
        if (MDNS.begin("magicband")) {
            MDNS.addService("http", "tcp", 80);
            mdnsStarted = true;
        }
    }

    // Web Armed Animation (Simple white rotate)
    if (g_webScanArmed && !g_webScanHasResult) {
        static uint32_t lastAnim = 0;
        if (millis() - lastAnim > 100) {
            lastAnim = millis();
            static uint8_t pos = 0;
            strip.clear();
            strip.setPixelColor(pos, strip.Color(200, 200, 200));
            strip.show();
            pos = (pos + 1) % LED_COUNT;
        }
    }

    static uint32_t lastScanCheck = 0;
    if (millis() - lastScanCheck > 300) {
        lastScanCheck = millis();
        uint8_t uid[7], len;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 50)) {
            
            formatUidString(uid, len, g_webScanUidStr, sizeof(g_webScanUidStr));
            computeBandTypeFromUidStr(g_webScanUidStr, g_webScanTypeStr, sizeof(g_webScanTypeStr));

            int idx = -1;
            for (int i = 0; i < bandCount; i++) if (memcmp(uid, registeredBands[i].uid, 7) == 0) { idx = i; break; }

            if(g_webScanArmed) {
                g_webScanUidLen = len; memcpy((void*)g_webScanUid, uid, len);
                g_webScanKnownIndex = idx; g_webScanIsKnown = (idx != -1); g_webScanHasResult = true;
                if(!g_webScanIsKnown) {
                    memset(&g_webPendingRecord, 0, sizeof(g_webPendingRecord));
                    memcpy(g_webPendingRecord.uid, uid, 7);
                    strncpy(g_webPendingRecord.name, "New Band", 19);
                    strncpy(g_webPendingRecord.type, g_webScanTypeStr, 19);
                    g_webPendingRecord.color = 0x00FF00;
                    g_webPendingNew = true;
                }
            }

            if (idx != -1) handleSuccess(registeredBands[idx].color); 
            else { 
                strip.setBrightness(200);
                strip.fill(strip.Color(150, 100, 0)); strip.show(); delay(250); 
                strip.clear(); strip.setBrightness(40); strip.show();
            }
        }
    }
}