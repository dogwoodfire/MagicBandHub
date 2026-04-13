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
#include <Audio.h> 

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
Audio audio(false, 3, I2S_NUM_1); 

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
bool g_successAnimationActive = false;
uint32_t g_successAnimationColor = 0;
uint32_t g_successAnimationStart = 0;
uint32_t g_successAnimationLastFrame = 0;
enum AnimationStyle : uint8_t {
    ANIMATION_STYLE_SPOOKY = 0,
    ANIMATION_STYLE_GREEN = 1,
    ANIMATION_STYLE_DEFAULT_CHIME = 2,
};
AnimationStyle g_successAnimationStyle = ANIMATION_STYLE_SPOOKY;
uint32_t g_successAnimationPhaseStart = 0;

static void audioFriendlyDelay(uint32_t ms) {
    const uint32_t start = millis();
    while (millis() - start < ms) {
        Audio_Loop();
        delay(2);
    }
}

static uint8_t scaleChan(uint8_t v, uint8_t level) {
    return (uint8_t)(((uint16_t)v * (uint16_t)level) / 255U);
}

static void renderSolidGreen(uint8_t brightness) {
    strip.setBrightness(brightness);
    strip.fill(strip.Color(0, 255, 0));
    strip.show();
}

static void renderDefaultChimeStep(uint8_t headIndex) {
    strip.clear();
    strip.setPixelColor(headIndex, strip.Color(0, 255, 0));
    strip.setPixelColor((headIndex + LED_COUNT - 1) % LED_COUNT, strip.Color(0, 64, 0));
    strip.setPixelColor((headIndex + 1) % LED_COUNT, strip.Color(0, 96, 0));
    strip.show();
}

static void renderCometPulse(uint32_t baseColor, uint32_t elapsedMs) {
    const uint32_t orange = strip.Color(255, 110, 0);
    const uint32_t purple = strip.Color(120, 0, 180);
    const uint32_t green  = strip.Color(0, 220, 70);
    const uint32_t mint   = strip.Color(80, 255, 170);

    const uint32_t primary = (g_successAnimationStyle == ANIMATION_STYLE_GREEN) ? green : orange;
    const uint32_t accent  = (g_successAnimationStyle == ANIMATION_STYLE_GREEN) ? mint : purple;

    // Fast rotation with a jaunty two-step bounce in brightness.
    const uint8_t head = (elapsedMs / 90U) % LED_COUNT;
    const uint8_t bounce = ((elapsedMs / 180U) % 2 == 0) ? 255 : 170;

    strip.clear();
    for (uint8_t i = 0; i < LED_COUNT; i++) {
        const uint8_t offset = (i + LED_COUNT - head) % LED_COUNT;
        uint32_t color = 0;
        uint8_t level = 0;

        if (offset == 0) {
            color = primary;
            level = bounce;
        } else if (offset == 1 || offset == LED_COUNT - 1) {
            color = accent;
            level = 180;
        } else if (offset == 2 || offset == LED_COUNT - 2) {
            color = primary;
            level = 90;
        } else if ((i + (elapsedMs / 220U)) % 3 == 0) {
            color = accent;
            level = 45;
        } else {
            color = primary;
            level = 18;
        }

        const uint8_t r = (color >> 16) & 0xFF;
        const uint8_t g = (color >> 8) & 0xFF;
        const uint8_t b = color & 0xFF;
        strip.setPixelColor(i, strip.Color(scaleChan(r, level), scaleChan(g, level), scaleChan(b, level)));
    }
    strip.show();
}

static void stopSuccessAnimation() {
    g_successAnimationActive = false;
    isSuccessActive = false;
    strip.clear();
    strip.setBrightness(40);
    strip.show();
}

static void updateSuccessAnimation() {
    if (!g_successAnimationActive) {
        return;
    }

    const uint32_t now = millis();
    if (g_successAnimationStyle == ANIMATION_STYLE_DEFAULT_CHIME) {
        const uint32_t rotateElapsed = now - g_successAnimationStart;
        const uint32_t stepMs = 120;
        const uint8_t totalSteps = LED_COUNT * 2;
        const uint32_t step = rotateElapsed / stepMs;

        if (step < totalSteps) {
            if (now - g_successAnimationLastFrame >= 16) {
                g_successAnimationLastFrame = now;
                strip.setBrightness(255);
                renderDefaultChimeStep(step % LED_COUNT);
            }
            return;
        }

        if (g_successAnimationPhaseStart == 0) {
            g_successAnimationPhaseStart = now;
            renderSolidGreen(255);
            Play_Default_Band_Chime();
            Serial.println("Default band chime sequence reached audio sync point.");
        }

        const uint32_t fadeElapsed = now - g_successAnimationPhaseStart;
        if (fadeElapsed >= 1500) {
            stopSuccessAnimation();
            return;
        }

        if (now - g_successAnimationLastFrame >= 16) {
            g_successAnimationLastFrame = now;
            const uint8_t brightness = (uint8_t)(((1500 - fadeElapsed) * 255U) / 1500U);
            renderSolidGreen(brightness);
        }
        return;
    }

    if (now - g_successAnimationLastFrame >= 16) {
        g_successAnimationLastFrame = now;
        strip.setBrightness(255);
        renderCometPulse(g_successAnimationColor, now - g_successAnimationStart);
    }

    const uint32_t elapsed = now - g_successAnimationStart;
    const bool minRuntimeMet = elapsed >= 250;
    const bool audioEnded = !audio.isRunning();
    const bool timedOut = elapsed >= 30000;
    if ((audioEnded && minRuntimeMet) || timedOut) {
        stopSuccessAnimation();
    }
}

static void startScanAnimation(AnimationStyle style, uint32_t color) {
    g_successAnimationStyle = style;
    g_successAnimationColor = color;
    g_successAnimationStart = millis();
    g_successAnimationLastFrame = 0;
    g_successAnimationPhaseStart = 0;
    g_successAnimationActive = true;
    isSuccessActive = true;
}

static void startDefaultBandChimeSequence() {
    startScanAnimation(ANIMATION_STYLE_DEFAULT_CHIME, 0x00FF00);
}

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
    Wire.beginTransmission(0x20);
    Wire.write(0x06); Wire.write(0x00); // Port0 output
    Wire.endTransmission();

    Wire.beginTransmission(0x20);
    Wire.write(0x07); Wire.write(0x00); // Port1 output
    Wire.endTransmission();

    Wire.beginTransmission(0x20);
    Wire.write(0x02); Wire.write(0xFF); // Port0 outputs high
    Wire.endTransmission();

    Wire.beginTransmission(0x20);
    Wire.write(0x03); Wire.write(0xFF); // Factory flow sets EXIO8 high when enabling PA
    Wire.endTransmission();

    Serial.println("IO Expander: Port1=0xFF (EXIO8 high)");
}

// void initIOExpander() {
//     Wire.beginTransmission(0x20); Wire.write(0x06); Wire.write(0x00); Wire.endTransmission(); 
//     Wire.beginTransmission(0x20); Wire.write(0x07); Wire.write(0xFE); Wire.endTransmission(); 
//     Wire.beginTransmission(0x20); Wire.write(0x02); Wire.write(0xFF); Wire.endTransmission(); 
//     Wire.beginTransmission(0x20); Wire.write(0x03); Wire.write(0xFF); Wire.endTransmission(); 
//     Serial.println("Hardware: Speaker PA (EXIO8) Enabled.");
// }

void handleSuccess(uint32_t color, uint16_t themeId) {
    if(isSuccessActive) return;

    if (themeId == 7) {
        Play_Music_theme(themeId);
        startScanAnimation(ANIMATION_STYLE_SPOOKY, color);
        Serial.println("Theme-specific success animation started; running until audio playback ends.");
        return;
    }

    startDefaultBandChimeSequence();
    Serial.println("Default band chime sequence started.");
}

void handleUnknownBand() {
    if (isSuccessActive) return;

    startDefaultBandChimeSequence();
    Serial.println("Unknown band default chime sequence started.");
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
    audio.setBufsize(4096, 65536);
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
    updateSuccessAnimation();
    //audio.loop();

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
    static uint32_t lastScanTime = 0;
    static uint8_t lastScanUid[7] = {0};
    static uint8_t lastScanLen = 0;
    if (millis() - lastNFC > 150 && !isSuccessActive) {
        lastNFC = millis();
        uint8_t uid[7], len;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 40)) {
            bool sameAsLast = (len == lastScanLen) && (memcmp(uid, lastScanUid, len) == 0);
            if (sameAsLast && (millis() - lastScanTime < 2000)) {
                return;
            }
            memcpy(lastScanUid, uid, len);
            lastScanLen = len;
            lastScanTime = millis();

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
            if (idx != -1) {
                Serial.printf("Known band idx=%d name=%s themeId=%u\n", idx, registeredBands[idx].name, (unsigned)registeredBands[idx].themeId);
                handleSuccess(registeredBands[idx].color, registeredBands[idx].themeId);
            }
            else {
                handleUnknownBand();
            }
        }
    }
}