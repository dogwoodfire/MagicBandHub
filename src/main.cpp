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

// Audio and SD Support
#include "Audio_ES8311.h"
#include "SD_Card.h"

struct _lv_event_t; 

// --- Hardware Pins ---
#define LED_PIN 38       
#define LED_COUNT 7      
#define NFC_IRQ 6        
#define NFC_RST 7        
#define I2C_SDA 11
#define I2C_SCL 10

// --- Globals ---
Adafruit_PN532 nfc(NFC_IRQ, NFC_RST, &Wire); 
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_RGB + NEO_KHZ800);
Audio audio; 

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

void clearWebScanData() {
    g_webScanArmed = false;
    g_webScanHasResult = false;
    g_webScanIsKnown = false;
    g_webScanKnownIndex = -1;
    g_webScanUidLen = 0;
    memset((void*)g_webScanUid, 0, sizeof(g_webScanUid));
    g_webScanUidStr[0] = '\0';
    g_webScanTypeStr[0] = '\0';
    g_webPendingNew = false;
    memset(&g_webPendingRecord, 0, sizeof(g_webPendingRecord));
}

// ---------------------------------------------------------------------------------------
// Linker Bridge
// ---------------------------------------------------------------------------------------
extern "C" {
    void loadCategoriesFromPrefs() {
        prefs.begin("mbands", false); 
        for (int i = 0; i < 10; i++) {
            strncpy(ownersList[i], prefs.getString(("o" + String(i)).c_str(), "").c_str(), 19);
            strncpy(locationsList[i], prefs.getString(("l" + String(i)).c_str(), "").c_str(), 19);
        }
        prefs.end();
    }
    void formatUidString(const uint8_t *uid, uint8_t len, char *out, size_t outSize) {
        if(!out || outSize < 4) return;
        out[0] = '\0';
        for(uint8_t i=0; i<len && (i*3+2) < outSize; i++) {
            char buf[4]; snprintf(buf, sizeof(buf), "%02X", uid[i]);
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
    void web_arm_scan() { 
        clearWebScanData(); 
        Serial.println("Registration ARMED"); 
        g_webScanArmed = true; 
    }
    void web_cancel_scan() { clearWebScanData(); }
    bool web_has_scan_result() { return g_webScanHasResult; }
    bool web_scan_result_is_known() { return g_webScanIsKnown; }
    int  web_scan_known_index() { return g_webScanKnownIndex; }
    bool web_pending_new_band() { return g_webPendingNew; }
    void web_scan_uid_string(char *out, size_t outSize) { strncpy(out, g_webScanUidStr, outSize); }
    void web_scan_type_string(char *out, size_t outSize) { strncpy(out, g_webScanTypeStr, outSize); }

    int web_confirm_save_pending_new(bool yes) {
        if(!yes) { clearWebScanData(); return -1; }
        if(bandCount >= 50) { clearWebScanData(); return -1; }
        registeredBands[bandCount] = g_webPendingRecord;
        int idx = bandCount; bandCount++;
        prefs.begin("mbands", false);
        prefs.putInt("count", bandCount);
        prefs.putBytes(("b" + String(idx)).c_str(), &registeredBands[idx], sizeof(BandRecord));
        prefs.end();
        clearWebScanData(); 
        return idx;
    }
}

// --- Audio Diagnostics Callbacks ---
void audio_info(const char *info){ Serial.print("AUDIO_INFO: "); Serial.println(info); }
void audio_eof_mp3(const char *info){ Serial.println("AUDIO: End of file reached"); }

void initIOExpander() {
    Wire.beginTransmission(0x20); Wire.write(0x06); Wire.write(0x00); Wire.endTransmission(); 
    Wire.beginTransmission(0x20); Wire.write(0x07); Wire.write(0xFE); Wire.endTransmission(); 
    Wire.beginTransmission(0x20); Wire.write(0x02); Wire.write(0xFF); Wire.endTransmission(); 
    Wire.beginTransmission(0x20); Wire.write(0x03); Wire.write(0xFF); Wire.endTransmission(); 
    Serial.println("Hardware: Speaker PA (EXIO8) Enabled.");
}

void handleSuccess(uint32_t color) {
    if(isSuccessActive) return;
    isSuccessActive = true;
    
    Play_Music_test();

    strip.setBrightness(255); 
    for(int r=0; r<2; r++) {
        for(int f=0; f<LED_COUNT; f++) {
            // CRITICAL: Prevent audio timeout during LED delays
            audio.loop(); 

            strip.clear();
            for(int i=0; i<4; i++) {
                int p=(f-i+LED_COUNT)%LED_COUNT;
                strip.setPixelColor(p, strip.Color(255-(i*60), 255-(i*60), 255-(i*60))); 
            }
            strip.show(); delay(35);
        }
    }
    
    for(int p=0; p<2; p++) {
        for(int b=60; b<=255; b+=15) { 
            audio.loop(); // Keep audio pumping
            strip.fill(strip.gamma32(color)); strip.setBrightness(b); strip.show(); delay(20); 
        }
        for(int b=255; b>=60; b-=15) { 
            audio.loop(); // Keep audio pumping
            strip.fill(strip.gamma32(color)); strip.setBrightness(b); strip.show(); delay(20); 
        }
    }

    strip.clear(); strip.setBrightness(40); strip.show();
    delay(200); 
    isSuccessActive = false;
}

void setup() {
    Serial.begin(115200);
    delay(2000); // Give Serial Monitor time to connect
    Serial.println("\n\n=== MAGIC BAND HUB BOOTING ===");

    if(!LittleFS.begin(true)) Serial.println("LittleFS Mount Failed");

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setTimeOut(250); 
    initIOExpander();   
    
    // FIXED: Only call SD_Init once
    SD_Init();
    Audio_Init();

    strip.begin();
    strip.setBrightness(40);
    strip.fill(strip.Color(0, 0, 150)); strip.show();

    loadCategoriesFromPrefs();
    prefs.begin("mbands", false);
    int rc = prefs.getInt("count", 0); bandCount = (rc < 0 || rc > 50) ? 0 : rc;
    for(int i=0; i<bandCount; i++) prefs.getBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
    prefs.end();
    
    if(nfc.begin()) nfc.SAMConfig();

    initWebServer(); 
    if (tryConnectSavedWiFi()) { 
        isWiFiActive = true; startWebServer(); 
        strip.fill(strip.Color(0, 150, 0)); 
    } else {
        WiFi.mode(WIFI_AP); WiFi.softAP("MagicBandHub-Setup");
        startWebServer();
        strip.fill(strip.Color(150, 100, 0)); 
    }
    strip.show(); delay(800); strip.clear(); strip.show();
}

void loop() {
    Audio_Loop();
    audio.loop();

    if (WiFi.status() == WL_CONNECTED && !mdnsStarted) {
        if (MDNS.begin("magicband")) {
            MDNS.addService("http", "tcp", 80);
            mdnsStarted = true;
        }
    }

    // Priority 2: Button Polling (Middle Button triggers Audio Test)
    static uint32_t lastBtn = 0;
    if (millis() - lastBtn > 150) {
        lastBtn = millis();
        
        Wire.beginTransmission(0x20);
        Wire.write(0x01); // Read Port 1
        if (Wire.endTransmission() == 0) {
            Wire.requestFrom(0x20, 1);
            if (Wire.available()) {
                uint8_t input = Wire.read();
                // Button 2 is bit 2 (IO10)
                if (!(input & 0x04)) { 
                    Serial.println("Button: Middle Pressed -> Beep Test");
                    Play_Raw_Hardware_Test();
                }
            }
        }
    }

    if (g_webScanArmed && !g_webScanHasResult && !isSuccessActive) {
        static uint32_t lastAnim = 0;
        if (millis() - lastAnim > 120) {
            lastAnim = millis();
            static uint8_t pos = 0; strip.clear();
            strip.setPixelColor(pos, strip.Color(200, 200, 200)); strip.show();
            pos = (pos + 1) % LED_COUNT;
        }
    }

    static uint32_t lastNFC = 0;
    if (millis() - lastNFC > 150 && !isSuccessActive) {
        lastNFC = millis();
        uint8_t uid[7], len;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 40)) {
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
                    g_webPendingRecord.color = 0x00FF00; g_webPendingNew = true;
                }
            }
            if (idx != -1) handleSuccess(registeredBands[idx].color); 
            else { 
                strip.setBrightness(200); strip.fill(strip.Color(150, 100, 0)); strip.show(); 
                delay(300); strip.clear(); strip.setBrightness(40); strip.show(); 
            }
        }
    }
}