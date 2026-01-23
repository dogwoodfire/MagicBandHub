#ifndef SCREEN_ENABLED
#define SCREEN_ENABLED 0
#endif
#include <Arduino.h>
#include <Wire.h>
#if SCREEN_ENABLED
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <ui.h>
#else
// Forward declare LVGL event type so we can keep stub signatures without pulling LVGL in.
typedef struct _lv_event_t lv_event_t;
#endif
#include <Adafruit_PN532.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include "web_portal.h"

// --- Animation Speed Variables (Increase to SLOW DOWN) ---
#define SPEED_COMET 30       
#define SPEED_FILL  45       
#define SPEED_PULSE 15       
#define SPEED_DISCOVERY 45   
#define SPEED_MANUAL 60      

// Hardware Config
// NOTE: Most pins are provided via PlatformIO build_flags (-D ...). These are safe fallbacks.
#ifndef LED_PIN
// Default for the Freenove ESP32-S3 Lite wiring (your new board)
#define LED_PIN 4
#endif
#ifndef LED_COUNT
#define LED_COUNT 12
#endif

// PN532 (I2C)
#ifndef NFC_SDA
// Default for the Freenove ESP32-S3 Lite wiring (your new board)
#define NFC_SDA 8
#endif
#ifndef NFC_SCL
#define NFC_SCL 9
#endif
#ifndef NFC_IRQ
#define NFC_IRQ 10
#endif
#ifndef NFC_RST
#define NFC_RST 11
#endif

// Optional screen/touch pins (only used when SCREEN_ENABLED=1)
#if SCREEN_ENABLED
#ifndef TFT_BL
#define TFT_BL 2
#endif
#ifndef TOUCH_SDA
#define TOUCH_SDA 6
#endif
#ifndef TOUCH_SCL
#define TOUCH_SCL 7
#endif
#ifndef TOUCH_RST
#define TOUCH_RST 13
#endif
#endif

// --- Storage limits (keep unchanged for now; used throughout for consistency) ---
#define MAX_BANDS 50
#define MAX_OWNERS 10
#define MAX_LOCATIONS 10

AsyncWebServer server(80); 
#if SCREEN_ENABLED
TFT_eSPI tft = TFT_eSPI(240, 240); 
#endif
TwoWire I2C_NFC = TwoWire(1); 
Adafruit_PN532 nfc(NFC_IRQ, NFC_RST, &I2C_NFC); 
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences prefs;

uint32_t lastActivityTime = 0;
uint32_t idleTimeout = 60000;   // Default 1 min
uint32_t sleepTimeout = 300000; // Default 5 min
bool isScreenOn = true;

BandRecord registeredBands[MAX_BANDS];
int bandCount = 0;
char ownersList[MAX_OWNERS][20];
char locationsList[MAX_LOCATIONS][20];

// Make the C-linkage function visible to other files
#if SCREEN_ENABLED
extern "C" void fn_refresh_roller(lv_event_t * e);
#else
extern "C" void fn_refresh_roller(lv_event_t * e) { (void)e; }
#endif

// Reload ownersList/locationsList from NVS (safe for missing keys)
void loadCategoriesFromPrefs() {
    prefs.begin("mbands", true);

    for (int i = 0; i < MAX_OWNERS; i++) {
        memset(ownersList[i], 0, sizeof(ownersList[i]));
        memset(locationsList[i], 0, sizeof(locationsList[i]));

        String oKey = "o" + String(i);
        String lKey = "l" + String(i);

        if (prefs.isKey(oKey.c_str())) {
            String val = prefs.getString(oKey.c_str(), "");
            strncpy(ownersList[i], val.c_str(), sizeof(ownersList[i]) - 1);
            ownersList[i][sizeof(ownersList[i]) - 1] = '\0';
        }

        if (prefs.isKey(lKey.c_str())) {
            String val = prefs.getString(lKey.c_str(), "");
            strncpy(locationsList[i], val.c_str(), sizeof(locationsList[i]) - 1);
            locationsList[i][sizeof(locationsList[i]) - 1] = '\0';
        }
    }

    prefs.end();
}


BandRecord tempRecord; 
bool isWaitingForUID = false;
bool isSuccessActive = false;
bool isWiFiActive = false;

// ---------------- Web-controlled scan/register (screen-less mode) ----------------
volatile bool g_webScanArmed = false;        // web requested a scan
volatile bool g_webScanHasResult = false;    // a tag was seen while armed
volatile bool g_webScanIsKnown = false;      // result matched a registered band
volatile int  g_webScanKnownIndex = -1;      // index if known, else -1
volatile bool g_webScanResultConsumed = false; // success animation already triggered for current web scan result
volatile uint8_t g_webScanUidLen = 0;        // UID length
volatile uint8_t g_webScanUid[10] = {0};     // UID bytes (up to 10)
char g_webScanUidStr[32] = {0};              // "04:AA:..." string
char g_webScanTypeStr[24] = {0};             // "MagicBand+" etc.

// When a new band is awaiting user confirmation in the web UI, we keep it here.
volatile bool g_webPendingNew = false;
BandRecord g_webPendingRecord;               // staged record (not yet saved)
// -------------------------------------------------------------------------------

#if SCREEN_ENABLED
extern "C" {
    extern lv_obj_t * ui_Scanner, * ui_mickeyScanner, * ui_ScanBandPnl3, * ui_BandRoller, * ui_RegisterConfirmPnl, * ui_NewBandConfirm, * ui_StatusLabel, * ui_EditNameLabel, * ui_StandbyScreen, * ui_clock, * ui_Hotspot;
}
#endif

void wakeScreen() {
#if SCREEN_ENABLED
    // 1. Switch screen while the lights are still OFF
    if(lv_scr_act() == ui_StandbyScreen) {
        _ui_screen_change(&ui_Scanner, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Scanner_screen_init);
        lv_timer_handler(); // Force the screen update to happen immediately
    }

    // 2. Now turn the backlight on
    if(!isScreenOn) {
        digitalWrite(TFT_BL, HIGH);
        isScreenOn = true;
    }
#endif

    lastActivityTime = millis();
}


void runWhiteSwirl(int speed) {
    static uint8_t swirlPos = 0;
    strip.clear();
    for(int i=0; i<5; i++) {
        int p = (swirlPos - i + LED_COUNT) % LED_COUNT;
        int b = 255 - (i * 50); if(b < 0) b = 0;
        strip.setPixelColor(p, strip.Color(b, b, b)); 
    }
    strip.show();
    swirlPos = (swirlPos + 1) % LED_COUNT;
}



// ---------------- Non-blocking success animation (state machine) ----------------
static uint8_t  g_successStage = 0;     // 0=swirl,1=fill,2=pulse,3=done
static uint8_t  g_successStep  = 0;     // step within stage
static uint32_t g_successNextMs = 0;    // next tick time
static uint32_t g_successColor = 0;     // final color used for pulse stage
static int16_t  g_pulseBrightness = 30; // 30..255
static int8_t   g_pulseDir = 1;         // +1 / -1
static uint8_t  g_pulseCycles = 0;      // completed cycles

static uint16_t g_successThemeId = 0;   // stored theme for pattern selection
static uint32_t g_successBaseColor = 0; // original per-band color (theme 0)

static uint32_t applyThemeToColor(uint32_t color, uint16_t themeId) {
    // Theme 0 = use the per-band color as-is.
    if(themeId == 0) return color;

    switch(themeId) {
        case 1: // Classic Green
            return 0x00FF00;
        case 2: // Comet Blue
            return 0x2E86FF;
        case 3: // Pulse Purple
            return 0x7D3CFF;
        case 4: // Rainbow (keep band color for now; true rainbow can be added later)
            return color;
        case 5: // Spooky Orange
            return 0xFF5000;
        case 6: // MNSSHP (pattern handled in tickSuccess; use purple for any UI recolor)
            return 0x7D3CFF;
        default:
            return color;
    }
}

static uint32_t wheelColor(uint8_t wheelPos) {
    // Standard NeoPixel color wheel (0-255)
    wheelPos = 255 - wheelPos;
    if(wheelPos < 85) {
        return strip.Color(255 - wheelPos * 3, 0, wheelPos * 3);
    }
    if(wheelPos < 170) {
        wheelPos -= 85;
        return strip.Color(0, wheelPos * 3, 255 - wheelPos * 3);
    }
    wheelPos -= 170;
    return strip.Color(wheelPos * 3, 255 - wheelPos * 3, 0);
}

static uint32_t blendRgb(uint32_t c1, uint32_t c2, uint8_t t) {
    // Linear blend of two 0xRRGGBB colors. t=0 => c1, t=255 => c2
    uint8_t r1 = (c1 >> 16) & 0xFF;
    uint8_t g1 = (c1 >> 8) & 0xFF;
    uint8_t b1 = (c1 >> 0) & 0xFF;

    uint8_t r2 = (c2 >> 16) & 0xFF;
    uint8_t g2 = (c2 >> 8) & 0xFF;
    uint8_t b2 = (c2 >> 0) & 0xFF;

    uint8_t r = (uint8_t)(((uint16_t)r1 * (255 - t) + (uint16_t)r2 * t) / 255);
    uint8_t g = (uint8_t)(((uint16_t)g1 * (255 - t) + (uint16_t)g2 * t) / 255);
    uint8_t b = (uint8_t)(((uint16_t)b1 * (255 - t) + (uint16_t)b2 * t) / 255);

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void drawSuccessSwirlAt(uint8_t pos) {
    // Matches the old blocking STAGE 1 comet/swirl look
    strip.clear();
    for(int i = 0; i < 5; i++) {
        int p = (pos - i + LED_COUNT) % LED_COUNT;
        int b = 255 - (i * 50);
        if(b < 0) b = 0;
        strip.setPixelColor(p, strip.Color(b, b, b));
    }
    strip.show();
#if SCREEN_ENABLED
    lv_timer_handler();
#endif
}

static void startSuccess(uint32_t color, uint16_t themeId) {
    if(isSuccessActive) return;

    isSuccessActive = true;
    g_successStage = 0;
    g_successStep = 0;
    g_successNextMs = millis();
    g_pulseBrightness = 30;
    g_pulseDir = 1;
    g_pulseCycles = 0;

    g_successThemeId = themeId;
    g_successBaseColor = color;
    g_successColor = applyThemeToColor(color, themeId);

    // Ensure we don't leave the ring in the last "scan swirl" frame.
    strip.setBrightness(40);
    strip.clear();
    strip.show();

#if SCREEN_ENABLED
    if(ui_mickeyScanner) {
        lv_obj_set_style_img_recolor(ui_mickeyScanner, lv_color_hex(g_successColor), 0);
        lv_obj_set_style_img_recolor_opa(ui_mickeyScanner, 255, 0);
        lv_timer_handler();
    }
#endif
}

static void tickSuccess() {
    if(!isSuccessActive) return;

    uint32_t now = millis();
    if((int32_t)(now - g_successNextMs) < 0) return;

    switch(g_successStage) {
        case 0: {
            // STAGE 1: White comet/swirl (1 rotation)
            drawSuccessSwirlAt(g_successStep);
            g_successStep++;
            g_successNextMs = now + SPEED_COMET;
            if(g_successStep >= LED_COUNT) {
                g_successStage = 1;
                g_successStep = 0;
                strip.clear();
                strip.show();
            }
            break;
        }

        case 1: {
            // STAGE 2: Progressive fill (white, 1..12)
            strip.clear();
            for(uint8_t i = 0; i <= g_successStep && i < LED_COUNT; i++) {
                strip.setPixelColor(i, strip.Color(255, 255, 255));
            }
            strip.show();
#if SCREEN_ENABLED
            lv_timer_handler();
#endif
            g_successStep++;
            g_successNextMs = now + SPEED_FILL;
            if(g_successStep >= LED_COUNT) {
                g_successStage = 2;
                g_successStep = 0;
                g_pulseBrightness = 30;
                g_pulseDir = 1;
                g_pulseCycles = 0;

                // Ensure starting brightness for theme patterns
                strip.setBrightness(40);
            }
            break;
        }

        case 2: {
            // STAGE 3: Theme-specific success pattern (non-blocking)
            // - Themes 1/2/3/0: classic breathing pulse using g_successColor
            // - Theme 4: true rainbow chase around the ring
            // - Theme 5: spooky orange flicker with purple sparkles

            if(g_successThemeId == 4) {
                // Rainbow chase: 3 full color-wheel rotations
                uint8_t offset = g_successStep;
                for(uint8_t i = 0; i < LED_COUNT; i++) {
                    // Spread wheel evenly across LEDs, then add moving offset
                    uint8_t wp = (uint8_t)((i * (256 / LED_COUNT)) + offset);
                    strip.setPixelColor(i, strip.gamma32(wheelColor(wp)));
                }
                strip.setBrightness(80);
                strip.show();
#if SCREEN_ENABLED
                lv_timer_handler();
#endif

                uint8_t prev = g_successStep;
                g_successStep = (uint8_t)(g_successStep + 8);
                if(g_successStep < prev) {
                    // overflow => completed one full loop
                    g_pulseCycles++;
                }

                g_successNextMs = now + 25;
                if(g_pulseCycles >= 3) {
                    g_successStage = 3;
                    g_successNextMs = now;
                }
                break;
            }

            if(g_successThemeId == 5) {
                // Spooky pulse: slow purple breathing over orange base (no strobe)
                // Uses a triangle-wave blend to avoid harsh flashing.

                // Triangle wave 0..255..0 over 64 steps
                uint8_t phase = (uint8_t)(g_successStep & 0x3F); // 0..63
                uint8_t tri = (phase < 32) ? (uint8_t)(phase * 8) : (uint8_t)((63 - phase) * 8);

                // Limit purple influence so it stays "spooky glow" rather than full purple
                uint8_t purpleAmt = (uint8_t)((uint16_t)tri * 170 / 255); // 0..170

                uint32_t baseOrange = 0xFF5000;
                uint32_t spookyPurple = 0x7D3CFF;
                uint32_t mixed = blendRgb(baseOrange, spookyPurple, purpleAmt);

                // Fill ring with blended color
                uint32_t mixedGamma = strip.gamma32(mixed);
                for(uint8_t i = 0; i < LED_COUNT; i++) {
                    strip.setPixelColor(i, mixedGamma);
                }

                // Occasional gentle "ember" sparkle (rare, not a flash)
                if(random(0, 100) < 10) {
                    uint8_t p = (uint8_t)random(0, LED_COUNT);
                    uint32_t ember = strip.gamma32(blendRgb(mixed, spookyPurple, 200));
                    strip.setPixelColor(p, ember);
                }

                // Keep brightness steady-ish; tiny wobble tied to the pulse
                uint8_t b = (uint8_t)(55 + (tri / 6)); // ~55..97
                strip.setBrightness(b);
                strip.show();
#if SCREEN_ENABLED
                lv_timer_handler();
#endif

                g_successNextMs = now + 70; // slower update for smooth breathing
                g_successStep++;

                // ~4.5 seconds total (64 steps x 70ms)
                if(g_successStep >= 64) {
                    g_successStage = 3;
                    g_successNextMs = now;
                }
                break;
            }

            if(g_successThemeId == 6) {
                // MNSSHP: 6 equal segments of 2 LEDs each, alternating Purple/Green,
                // rotating slowly anticlockwise for 5 seconds.

                const uint32_t PURPLE = 0x7D3CFF;
                const uint32_t GREEN  = 0x00FF00;

                // Rotate anticlockwise by shifting pattern indices opposite the LED index.
                uint8_t rot = g_successStep; // 0..11

                for(uint8_t led = 0; led < LED_COUNT; led++) {
                    // Pattern position after rotation (anticlockwise)
                    uint8_t p = (uint8_t)((led + rot) % LED_COUNT);

                    // 6 equal segments: each segment is 2 LEDs (0..1, 2..3, 4..5, 6..7, 8..9, 10..11)
                    uint8_t seg = p / 2; // 0..5

                    // Alternate colors by segment: even=purple, odd=green
                    uint32_t c = (seg % 2 == 0) ? PURPLE : GREEN;
                    strip.setPixelColor(led, strip.gamma32(c));
                }

                strip.setBrightness(85);
                strip.show();
#if SCREEN_ENABLED
                lv_timer_handler();
#endif

                g_successNextMs = now + 200; // slow rotation

                // Advance rotation (one LED step per tick)
                g_successStep = (uint8_t)((g_successStep + 1) % LED_COUNT);

                // Count ticks for duration: 25 ticks * 200ms = 5 seconds
                g_pulseCycles++;
                if(g_pulseCycles >= 25) {
                    g_successStage = 3;
                    g_successNextMs = now;
                }
                break;
            }

            // Default pulse (themes 0/1/2/3 and anything else): breathing fade using g_successColor
            strip.fill(strip.gamma32(g_successColor));
            strip.setBrightness((uint8_t)g_pulseBrightness);
            strip.show();
#if SCREEN_ENABLED
            lv_timer_handler();
#endif

            g_pulseBrightness += (g_pulseDir * 10);
            if(g_pulseBrightness >= 255) {
                g_pulseBrightness = 255;
                g_pulseDir = -1;
            } else if(g_pulseBrightness <= 30) {
                g_pulseBrightness = 30;
                g_pulseDir = 1;
                g_pulseCycles++;
            }

            g_successNextMs = now + SPEED_PULSE;
            if(g_pulseCycles >= 2) {
                g_successStage = 3;
                g_successNextMs = now; // finish immediately next tick
            }
            break;
        }

        default: {
            // DONE: reset UI + LEDs
#if SCREEN_ENABLED
            if(ui_mickeyScanner) lv_obj_set_style_img_recolor_opa(ui_mickeyScanner, 0, 0);
#endif
            strip.clear();
            strip.setBrightness(40);
            strip.show();

            isSuccessActive = false;
            break;
        }
    }
}
// ------------------------------------------------------------------------------

void handleSuccess(uint32_t color, uint16_t themeId) {
    // Backwards-compatible wrapper: start the non-blocking animation.
    startSuccess(color, themeId);
}

void reset_record_panels() {
#if SCREEN_ENABLED
    if(ui_ScanBandPnl3) lv_obj_add_flag(ui_ScanBandPnl3, LV_OBJ_FLAG_HIDDEN);
#endif
}

#if SCREEN_ENABLED
void updateClock() {
    if (lv_scr_act() != ui_StandbyScreen || ui_clock == NULL) return;

    static uint32_t lastClockUpdate = 0;
    if (millis() - lastClockUpdate < 1000) return;
    lastClockUpdate = millis();

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        lv_label_set_text(ui_clock, "--:--");
        return;
    }

    char timeBuf[6]; // Buffer for "HH:MM\0"
    // Use "%H:%M" for 24-hour or "%I:%M" for 12-hour
    strftime(timeBuf, sizeof(timeBuf), "%H:%M", &timeinfo);
    lv_label_set_text(ui_clock, timeBuf);
}
#endif



extern "C" void formatUidString(const uint8_t *uid, size_t len, char *out, size_t outSize) {
    if(!out || outSize < 4) return;
    out[0] = '\0';
    for(size_t i = 0; i < len && (i * 3 + 2) < outSize; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X", uid[i]);
        strncat(out, buf, outSize - strlen(out) - 1);
        if(i < len - 1) strncat(out, ":", outSize - strlen(out) - 1);
    }
}

static void computeBandTypeFromUidStr(const char *uidStr, char *out, size_t outSize) {
    if(!out || outSize == 0) return;
    String s = String(uidStr);
    s.toUpperCase();
    String style = s.endsWith("90") ? "MagicBand+" : (s.endsWith("80") ? "MagicBand 2.0" : "MagicBand 1.0 / Other");
    strncpy(out, style.c_str(), outSize - 1);
    out[outSize - 1] = '\0';
}

// ---------------- Web-controlled scan/register (C-callable for web_portal.cpp) ----------------
extern "C" void web_arm_scan() {
    g_webScanArmed = true;
    g_webScanHasResult = false;
    g_webScanResultConsumed = false;
    g_webScanIsKnown = false;
    g_webScanKnownIndex = -1;
    g_webScanUidLen = 0;
    memset((void*)g_webScanUid, 0, sizeof(g_webScanUid));
    g_webScanUidStr[0] = '\0';
    g_webScanTypeStr[0] = '\0';
    g_webPendingNew = false;
    memset(&g_webPendingRecord, 0, sizeof(g_webPendingRecord));

    // Reset LED state so every web-armed scan starts clean.
    isSuccessActive = false;
    g_successStage = 0;
    g_successStep = 0;
    g_pulseCycles = 0;
    // Make it super obvious that the firmware received the web "arm scan" request.
    // If you don't see this flash, it's not an animation issue — it's LED/pin/power.

    // Ensure we start from a known cleared state after the flash.
    strip.setBrightness(40);
    strip.clear();
    strip.show();
}

extern "C" void web_cancel_scan() {
    g_webScanArmed = false;
    g_webPendingNew = false;
    g_webScanResultConsumed = false;
    // Cancel any in-progress success animation and clear LEDs.
    isSuccessActive = false;
    g_successStage = 0;
    g_successStep = 0;
    g_pulseCycles = 0;
    strip.setBrightness(40);
    strip.clear();
    strip.show();
}

extern "C" bool web_has_scan_result() {
    return g_webScanHasResult;
}

extern "C" bool web_scan_result_is_known() {
    return g_webScanIsKnown;
}

extern "C" int web_scan_known_index() {
    return g_webScanKnownIndex;
}

extern "C" void web_scan_uid_string(char *out, size_t outSize) {
    if(!out || outSize == 0) return;
    strncpy(out, g_webScanUidStr, outSize - 1);
    out[outSize - 1] = '\0';
}

extern "C" void web_scan_type_string(char *out, size_t outSize) {
    if(!out || outSize == 0) return;
    strncpy(out, g_webScanTypeStr, outSize - 1);
    out[outSize - 1] = '\0';
}

extern "C" bool web_pending_new_band() {
    return g_webPendingNew;
}

// If user confirms saving the pending new band, persist it and return the new index; otherwise return -1.
extern "C" int web_confirm_save_pending_new(bool yes) {
    if(!g_webPendingNew) {
        g_webScanArmed = false;
        return -1;
    }
    if(!yes) {
        g_webPendingNew = false;
        g_webScanArmed = false;
        return -1;
    }

    if(bandCount >= MAX_BANDS) {
        g_webPendingNew = false;
        g_webScanArmed = false;
        return -1;
    }

    registeredBands[bandCount] = g_webPendingRecord;
    int newIndex = bandCount;
    bandCount++;

    prefs.begin("mbands", false);
    prefs.putInt("count", bandCount);
    prefs.putBytes(("b" + String(newIndex)).c_str(), &registeredBands[newIndex], sizeof(BandRecord));
    prefs.end();

    fn_refresh_roller(NULL);

    g_webPendingNew = false;
    g_webScanArmed = false;

    return newIndex;
}
// ---------------------------------------------------------------------------------------------

#if SCREEN_ENABLED
// Callbacks (C Linkage)
extern "C" {
    void ui_event_RegisterFromPopup(lv_event_t * e) {
        if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
            if (bandCount < 10) {
                memcpy(registeredBands[bandCount].uid, tempRecord.uid, 7);
                
                // --- NEW: Dynamic Name Generation ---
                int nextNum = 1;
                bool found;
                char candidateName[20];
                
                // Loop until we find a name that isn't already taken
                do {
                    found = false;
                    snprintf(candidateName, 20, "MagicBand %d", nextNum);
                    for (int i = 0; i < bandCount; i++) {
                        if (strcmp(registeredBands[i].name, candidateName) == 0) {
                            found = true;
                            nextNum++;
                            break;
                        }
                    }
                } while (found);
                
                strncpy(registeredBands[bandCount].name, candidateName, sizeof(registeredBands[bandCount].name) - 1);
                registeredBands[bandCount].name[sizeof(registeredBands[bandCount].name) - 1] = '\0';
                // ------------------------------------

                // Hardware Detection for the new record
                String hStr = ""; 
                for(int i=0; i<7; i++) { if(tempRecord.uid[i]<0x10) hStr+="0"; hStr+=String(tempRecord.uid[i],HEX); }
                hStr.toUpperCase();
                String style = hStr.endsWith("90") ? "MagicBand+" : (hStr.endsWith("80") ? "MagicBand 2.0" : "Other");
                strncpy(registeredBands[bandCount].type, style.c_str(), 19);

                registeredBands[bandCount].color = 0x00FF00;
                registeredBands[bandCount].themeId = 0;
                memset(registeredBands[bandCount].imageUrl, 0, 100);
                bandCount++;
                prefs.begin("mbands", false); prefs.putInt("count", bandCount);
                prefs.putBytes(("b" + String(bandCount - 1)).c_str(), &registeredBands[bandCount-1], sizeof(BandRecord));
                prefs.end();
                fn_refresh_roller(NULL);
            }

            // 2. UI UPDATES (NOW ON SAME SCREEN)
            if(ui_RegisterConfirmPnl) lv_obj_add_flag(ui_RegisterConfirmPnl, LV_OBJ_FLAG_HIDDEN); // Hide popup
            if(ui_ScanBandPnl3) lv_obj_clear_flag(ui_ScanBandPnl3, LV_OBJ_FLAG_HIDDEN); // Show success on Scanner screen
        }
    }

    void fn_save_band(lv_event_t * e) {
    reset_record_panels();
    }
    void fn_refresh_roller(lv_event_t * e) {
        if (!ui_BandRoller) return;
        String options = "";
        for (int i = 0; i < bandCount; i++) options += String(registeredBands[i].name) + "\n";
        lv_roller_set_options(ui_BandRoller, options.length() > 0 ? options.c_str() : "No Bands", LV_ROLLER_MODE_NORMAL);
    }
    void fn_prepare_edit_panel(lv_event_t * e) {
        int index = lv_roller_get_selected(ui_BandRoller);
        if (bandCount > 0 && ui_EditNameLabel) lv_label_set_text(ui_EditNameLabel, registeredBands[index].name);
    }
    void fn_delete_selected_band(lv_event_t * e) {
        int index = lv_roller_get_selected(ui_BandRoller);
        if (index >= bandCount) return;
        for (int i = index; i < bandCount - 1; i++) registeredBands[i] = registeredBands[i + 1];
        bandCount--;
        prefs.begin("mbands", false); prefs.clear(); prefs.putInt("count", bandCount);
        for(int i=0; i<bandCount; i++) prefs.putBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
        prefs.end();
        fn_refresh_roller(NULL);
    }
    void fn_toggle_wifi(lv_event_t * e) {
        if (!isWiFiActive) { 
            WiFi.mode(WIFI_AP); WiFi.softAP("MagicBand-Hub", "password123"); WiFi.setSleep(false); startWebServer(); isWiFiActive = true; 
            if(ui_StatusLabel) lv_label_set_text(ui_StatusLabel, "Hotspot Active");
        } else { 
            stopWebServer(); WiFi.softAPdisconnect(true); isWiFiActive = false; 
            if(ui_StatusLabel) lv_label_set_text(ui_StatusLabel, "WiFi Disabled");
        }
    }
    void handleStandby() {
        uint32_t elapsed = millis() - lastActivityTime;

        // Transition to Clock Screen
        if (elapsed > idleTimeout && lv_scr_act() != ui_StandbyScreen) {
            _ui_screen_change(&ui_StandbyScreen, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, &ui_StandbyScreen_screen_init);
        }

        // Turn off Screen
        if (elapsed > sleepTimeout && isScreenOn) {
            digitalWrite(TFT_BL, LOW);
            isScreenOn = false;
        } 
    }
#endif



void setup() {
    setCpuFrequencyMhz(240);
    Serial.begin(115200);
    Serial.printf("[PINS] LED=%d count=%d  NFC SDA=%d SCL=%d IRQ=%d RST=%d\n",
              (int)LED_PIN, (int)LED_COUNT, (int)NFC_SDA, (int)NFC_SCL, (int)NFC_IRQ, (int)NFC_RST);
    Serial.printf("[LED] init: LED_PIN=%d LED_COUNT=%d\n", (int)LED_PIN, (int)LED_COUNT);
    // Seed RNG for theme patterns (spooky sparkles, etc.)
    randomSeed((uint32_t)esp_random());
    // ----- Load bands from NVS (robust: tolerate missing keys / holes) -----
    prefs.begin("mbands", true);

    int storedCount = prefs.getInt("count", 0);
    if (storedCount < 0 || storedCount > MAX_BANDS) storedCount = 0;

    int loaded = 0;
    for (int i = 0; i < MAX_BANDS; i++) {
        String key = "b" + String(i);

        // Skip missing keys (prevents NOT_FOUND spam)
        if (!prefs.isKey(key.c_str())) continue;

        BandRecord tmp;
        memset(&tmp, 0, sizeof(BandRecord));

        size_t got = prefs.getBytes(key.c_str(), &tmp, sizeof(BandRecord));
        if (got != sizeof(BandRecord)) continue;

        // Basic validity check: must have a non-zero UID
        bool uidNonZero = false;
        for (int u = 0; u < 7; u++) {
            if (tmp.uid[u] != 0) { uidNonZero = true; break; }
        }
        if (!uidNonZero) continue;

        // Accept record and compact into the front of the array
        registeredBands[loaded] = tmp;
        loaded++;
        if (loaded >= MAX_BANDS) break;
    }

    bandCount = loaded;

    // If NVS count/keys are inconsistent, rewrite a compacted set to avoid future holes.
    if (bandCount != storedCount) {
        prefs.end();
        prefs.begin("mbands", false);
        prefs.clear();
        prefs.putInt("count", bandCount);
        for (int i = 0; i < bandCount; i++) {
            prefs.putBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
        }
        prefs.end();
        prefs.begin("mbands", true);
    }

    // Boot diagnostics: prove what's loaded at boot
    Serial.printf("[BOOT] bandCount=%d\n", bandCount);
    for (int i = 0; i < bandCount; i++) {
        char uidStr[32] = {0};
        formatUidString(registeredBands[i].uid, 7, uidStr, sizeof(uidStr));
        Serial.printf("[BOOT] b%d uid=%s name='%s' owner='%s' loc='%s'\n",
                    i, uidStr,
                    registeredBands[i].name,
                    registeredBands[i].owner,
                    registeredBands[i].location);
    }

    prefs.end();
    // ----- End load -----
    // Load category lists
    loadCategoriesFromPrefs();
    
    #if SCREEN_ENABLED
    // --- I2C BUS RECOVERY (prevents SDA lockups) ---
    pinMode(TOUCH_SDA, INPUT_PULLUP);
    pinMode(TOUCH_SCL, OUTPUT);

    for (int i = 0; i < 9; i++) {
        digitalWrite(TOUCH_SCL, HIGH);
        delayMicroseconds(5);
        digitalWrite(TOUCH_SCL, LOW);
        delayMicroseconds(5);
    }

    pinMode(TOUCH_SCL, INPUT_PULLUP);
    delay(10);

    // --- TOUCH RESET (safe sequencing) ---
    pinMode(TOUCH_RST, OUTPUT);
    digitalWrite(TOUCH_RST, LOW);
    delay(20);

    // Ensure bus is idle before releasing reset
    pinMode(TOUCH_SDA, INPUT_PULLUP);
    pinMode(TOUCH_SCL, INPUT_PULLUP);
    delay(5);

    digitalWrite(TOUCH_RST, HIGH);
    delay(100);

    // --- START I2C BUSES ---
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    Wire.setClock(100000);
    Wire.setTimeOut(20); // ms
    #endif

    // --- START NFC I2C (always-on, screenless friendly) ---
    I2C_NFC.begin(NFC_SDA, NFC_SCL, 100000);
    I2C_NFC.setTimeOut(20);

    // Ensure PN532 reset pin is in a known state before init.
    pinMode(NFC_RST, OUTPUT);
    digitalWrite(NFC_RST, HIGH);
    delay(5);

        if(nfc.begin()) {
            nfc.SAMConfig();
        }

        strip.begin();
    strip.setBrightness(40);
    strip.show();
    Serial.printf("[LED] ready on GPIO%d (%d px)\n", (int)LED_PIN, (int)LED_COUNT);

#if SCREEN_ENABLED
    lv_init(); tft.begin(); tft.setRotation(0);
    pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH); 
    
    static lv_disp_draw_buf_t db; static lv_color_t b[240*24]; lv_disp_draw_buf_init(&db, b, NULL, 240*24);
    static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
    dd.hor_res=240; dd.ver_res=240; dd.flush_cb=my_disp_flush; dd.draw_buf=&db; lv_disp_drv_register(&dd);
    static lv_indev_drv_t id; lv_indev_drv_init(&id);
    id.type=LV_INDEV_TYPE_POINTER; id.read_cb=my_touchpad_read; lv_indev_drv_register(&id);

    const esp_timer_create_args_t ta = { .callback = [](void* arg){ lv_tick_inc(2); }, .name="t" };
    esp_timer_handle_t th; esp_timer_create(&ta, &th); esp_timer_start_periodic(th, 2000);

    prefs.begin("settings", true);
    idleTimeout = prefs.getUInt("idle", 60000);
    sleepTimeout = prefs.getUInt("sleep", 300000);
    prefs.end();

    ui_init(); 
    if(ui_StatusLabel) lv_label_set_text(ui_StatusLabel, "Tap below to set up WiFi");
    if(ui_NewBandConfirm) lv_obj_add_event_cb(ui_NewBandConfirm, ui_event_RegisterFromPopup, LV_EVENT_CLICKED, NULL);
#endif

    initWebServer();

    // Ensure WiFi stack is up before we attempt saved WiFi.
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    delay(50);

    Serial.printf("[WIFI] mode=%d status=%d\n", (int)WiFi.getMode(), (int)WiFi.status());
    Serial.println("[WIFI] attempting tryConnectSavedWiFi()...");

    if (tryConnectSavedWiFi()) {
        isWiFiActive = true;

        Serial.printf("[WIFI] connected: SSID='%s'\n", WiFi.SSID().c_str());
        Serial.print("[WIFI] IP: ");
        Serial.println(WiFi.localIP());
        Serial.print("[WIFI] MAC: ");
        Serial.println(WiFi.macAddress());

        startWebServer();
        Serial.println("[WEB] server started (STA). Look for [MDNS] http://magicband.local/" );

#if SCREEN_ENABLED
        if(ui_StatusLabel) {
            String msg = "Connected to: " + WiFi.SSID();
            lv_label_set_text(ui_StatusLabel, msg.c_str());
        }
#endif

    } else {
        // Fall back to SoftAP so the portal is always reachable.
        Serial.printf("[WIFI] STA connect failed; status=%d. Starting SoftAP...\n", (int)WiFi.status());

        WiFi.mode(WIFI_AP);
        WiFi.setSleep(false);

        const char* apSsid = "MagicBand-Hub";
        const char* apPass = "password123";
        bool apOk = WiFi.softAP(apSsid, apPass);

        Serial.printf("[AP] softAP=%d SSID='%s'\n", apOk ? 1 : 0, apSsid);
        Serial.print("[AP] IP: ");
        Serial.println(WiFi.softAPIP());

        isWiFiActive = apOk;
        startWebServer();
        Serial.println("[WEB] server started (AP). Use http://192.168.4.1/" );

#if SCREEN_ENABLED
        if(ui_StatusLabel) lv_label_set_text(ui_StatusLabel, "Hotspot Active");
#endif
    }
}

void loop() {
    #if SCREEN_ENABLED
    lv_timer_handler(); 
    handleStandby();
    updateClock();
    #endif

    // Non-blocking success animation tick (runs in screenless + screen builds)
    tickSuccess();


#if SCREEN_ENABLED
    static lv_obj_t * last_scr = NULL;
    if(lv_scr_act() != last_scr) {
        last_scr = lv_scr_act();
        if(last_scr == ui_Hotspot && ui_StatusLabel) {
            if(WiFi.status() == WL_CONNECTED) {
                String msg = "Connected to: " + WiFi.SSID();
                lv_label_set_text(ui_StatusLabel, msg.c_str());
            } else {
                lv_label_set_text(ui_StatusLabel, "Tap Bellow to Set Up Wifi");
            }
        }
    }
#endif
    
    static int lc = 0; if (bandCount != lc) { fn_refresh_roller(NULL); lc = bandCount; }
    if (!isSuccessActive && isWaitingForUID) {
        static uint32_t la = 0;
        if (millis() - la > SPEED_MANUAL) { la = millis(); runWhiteSwirl(SPEED_MANUAL); }
    }

    // Web-driven scan animation (screen-less register)
    if (!isSuccessActive && g_webScanArmed && !g_webScanHasResult) {
        static uint32_t la2 = 0;
        if (millis() - la2 > SPEED_MANUAL) {
            la2 = millis();
            runWhiteSwirl(SPEED_MANUAL);
        }
    }

    // Safety net: if the web scan has a result and LEDs are idle, trigger success animation ONCE.
    // This prevents the ring from freezing on the last "scan swirl" frame, without looping forever.
    if (!isSuccessActive && g_webScanArmed && g_webScanHasResult && !g_webScanResultConsumed) {
        g_webScanResultConsumed = true;
        if (g_webScanIsKnown && g_webScanKnownIndex >= 0 && g_webScanKnownIndex < bandCount) {
            handleSuccess(registeredBands[g_webScanKnownIndex].color, registeredBands[g_webScanKnownIndex].themeId);
        } else {
            handleSuccess(0x00FF00, 0);
        }
    }


    static uint32_t ls = 0;
    if (millis() - ls > 400) {
        ls = millis();

        // If we're already in the middle of the success animation, ignore any new scans.
        // This prevents double-triggers when the same band is held on the reader.
        if (isSuccessActive) {
            return;
        }

        uint8_t uid[7], len;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 50)) {
            wakeScreen();
#if SCREEN_ENABLED
            lastActivityTime = millis(); // Reset timer on every scan
            if(!isScreenOn) {
                digitalWrite(TFT_BL, HIGH);
                isScreenOn = true;
            }
#endif
            String hexUID = "";
            for (uint8_t i = 0; i < len; i++) { if (uid[i] < 0x10) hexUID += "0"; hexUID += String(uid[i], HEX); if (i < len - 1) hexUID += ":"; }
            hexUID.toUpperCase();

            String style = hexUID.endsWith("90") ? "MagicBand+" : (hexUID.endsWith("80") ? "MagicBand 2.0" : "MagicBand 1.0 / Other");
            Serial.printf("\n--- NFC SCAN: %s ---\nHARDWARE: %s\n", hexUID.c_str(), style.c_str());

            // If the web portal armed a scan, capture the result for the web UI
            if(g_webScanArmed) {
                g_webScanUidLen = len;
                memset((void*)g_webScanUid, 0, sizeof(g_webScanUid));
                for(uint8_t i=0; i<len && i<sizeof(g_webScanUid); i++) g_webScanUid[i] = uid[i];

                formatUidString(uid, len, g_webScanUidStr, sizeof(g_webScanUidStr));
                computeBandTypeFromUidStr(g_webScanUidStr, g_webScanTypeStr, sizeof(g_webScanTypeStr));

                int kidx = -1;
                for (int i = 0; i < bandCount; i++) {
                    if (memcmp(uid, registeredBands[i].uid, 7) == 0) { kidx = i; break; }
                }

                g_webScanKnownIndex = kidx;
                g_webScanIsKnown = (kidx != -1);
                g_webScanHasResult = true;
                // We will mark this consumed at the moment we actually trigger the success animation.
                // (Prevents the "safety net" block from replaying the animation a second time.)
                g_webScanResultConsumed = false;

                if(!g_webScanIsKnown) {
                    // Stage a new record (do not commit until user confirms)
                    memset(&g_webPendingRecord, 0, sizeof(g_webPendingRecord));
                    memcpy(g_webPendingRecord.uid, uid, 7);

                    // Dynamic name generation: MagicBand N
                    int nextNum = 1;
                    bool found;
                    char candidateName[20];
                    do {
                        found = false;
                        snprintf(candidateName, 20, "MagicBand %d", nextNum);
                        for (int i = 0; i < bandCount; i++) {
                            if (strcmp(registeredBands[i].name, candidateName) == 0) { found = true; nextNum++; break; }
                        }
                    } while (found);

                    strncpy(g_webPendingRecord.name, candidateName, sizeof(g_webPendingRecord.name) - 1);
                    g_webPendingRecord.name[sizeof(g_webPendingRecord.name) - 1] = '\0';

                    strncpy(g_webPendingRecord.type, style.c_str(), sizeof(g_webPendingRecord.type) - 1);
                    g_webPendingRecord.type[sizeof(g_webPendingRecord.type) - 1] = '\0';

                    g_webPendingRecord.color = 0x00FF00;
                    g_webPendingRecord.themeId = 0;
                    g_webPendingNew = true;
                }
            }

            int idx = -1;
            for (int i = 0; i < bandCount; i++) if (memcmp(uid, registeredBands[i].uid, 7) == 0) { idx = i; break; }

            if (idx != -1) {
                // If this scan came from the web-armed flow, ensure we only animate once.
                if (g_webScanArmed) g_webScanResultConsumed = true;

                // Only update label if we are on the main scanner screen
                handleSuccess(registeredBands[idx].color, registeredBands[idx].themeId);
            }
            else if (isWaitingForUID) {
                // Manual registration via button
                memcpy(registeredBands[bandCount].uid, uid, 7);
                
                // --- NEW: Dynamic Name Generation ---
                int nextNum = 1;
                bool found;
                char candidateName[20];
                do {
                    found = false;
                    snprintf(candidateName, 20, "MagicBand %d", nextNum);
                    for (int i = 0; i < bandCount; i++) {
                        if (strcmp(registeredBands[i].name, candidateName) == 0) {
                            found = true;
                            nextNum++;
                            break;
                        }
                    }
                } while (found);
                strncpy(registeredBands[bandCount].name, candidateName, sizeof(registeredBands[bandCount].name) - 1);
                registeredBands[bandCount].name[sizeof(registeredBands[bandCount].name) - 1] = '\0';
                // ------------------------------------

                strncpy(registeredBands[bandCount].type, style.c_str(), 19);
                registeredBands[bandCount].color = 0x00FF00;
                registeredBands[bandCount].themeId = 0;
                memset(registeredBands[bandCount].imageUrl, 0, 100);
                bandCount++;
                prefs.begin("mbands", false); prefs.putInt("count", bandCount);
                prefs.putBytes(("b" + String(bandCount - 1)).c_str(), &registeredBands[bandCount-1], sizeof(BandRecord));
                prefs.end();
                isWaitingForUID = false;
                
                // Show Success Panel child of Scanner screen
#if SCREEN_ENABLED
                if(ui_ScanBandPnl3) lv_obj_clear_flag(ui_ScanBandPnl3, LV_OBJ_FLAG_HIDDEN);
#endif
                if (g_webScanArmed) g_webScanResultConsumed = true;
                handleSuccess(0x00FF00, 0); 
            }
            else {
                if (g_webScanArmed) g_webScanResultConsumed = true;
                handleSuccess(0x00FF00, 0);
#if SCREEN_ENABLED
                memcpy(tempRecord.uid, uid, 7); 
                if(ui_RegisterConfirmPnl) lv_obj_clear_flag(ui_RegisterConfirmPnl, LV_OBJ_FLAG_HIDDEN);
#endif
            }
        }
    }
}