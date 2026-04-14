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
    ANIMATION_STYLE_BOO = 3,
    ANIMATION_STYLE_CUSTOM = 4,
};
AnimationStyle g_successAnimationStyle = ANIMATION_STYLE_SPOOKY;
uint32_t g_successAnimationPhaseStart = 0;
// Custom theme runtime state
uint32_t g_customThemeColors[CUSTOM_THEME_MAX_COLORS]; // NeoPixel-packed
uint8_t  g_customThemeColorCount = 0;
uint8_t  g_customThemePhases = 0;   // bitmask of CTP_PHASE_*
uint8_t  g_customThemeLoop = 0;     // CustomThemeLoop value

// ---- Music Player state (struct PlayerTrack defined in web_portal.h) ----
PlayerTrack g_playlist[64];
int         g_playlistCount = 0;
int         g_playerIndex   = -1; // currently playing index
bool        g_playerActive  = false;
bool        g_playerPaused  = false;

// Lightshow mode: 0=off 1=rainbow 2=pulse 3=colour-cycle
uint8_t g_playerLightshow = 0;
// Lightshow speed: 0=sedate 1=medium 2=lively
uint8_t g_playerLsSpeed = 0;
// Repeat / loop playlist
bool    g_playerRepeat    = true;
bool    g_playerRepeatOne = false; // repeat current track instead of advancing
// Player lightshow colour override (set when a theme with lsUseThemeColors plays)
bool     g_playerLsUseThemeColors = false;
uint32_t g_playerLsThemeColors[5] = {0};
uint8_t  g_playerLsThemeColorCount = 0;
static uint32_t g_playerLsFrame = 0;
static uint8_t  g_playerLsStep  = 0;

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

// Phase 1: comet with a fading tail sweeps clockwise once.
static void renderCometTrail(uint8_t headIndex) {
    strip.clear();
    // tail lengths: head = full, -1 = 60%, -2 = 25%, -3 = 8%
    const uint8_t tail[] = {255, 153, 64, 20};
    for (uint8_t t = 0; t < 4 && t < LED_COUNT; t++) {
        uint8_t idx = (headIndex + LED_COUNT - t) % LED_COUNT;
        strip.setPixelColor(idx, strip.Color(0, tail[t], 0));
    }
    strip.show();
}

// Phase 2: fill ring one pixel at a time (cumulative).
static void renderFillStep(uint8_t filledCount, uint8_t r = 0, uint8_t g = 255, uint8_t b = 0) {
    strip.clear();
    for (uint8_t i = 0; i < filledCount && i < LED_COUNT; i++) {
        strip.setPixelColor(i, strip.Color(r, g, b));
    }
    strip.show();
}

// Phase 3: pulse — sinusoidal brightness on a solid green ring.
static void renderPulse(uint32_t phaseElapsed) {
    // Two full pulses in 1200 ms → period = 600 ms each.
    const float angle = (float)phaseElapsed / 600.0f * 2.0f * 3.14159f;
    // sin goes -1..+1; map to brightness 40..255.
    float s = sinf(angle);
    uint8_t brightness = (uint8_t)(40.0f + (215.0f * (s * 0.5f + 0.5f)));
    strip.setBrightness(brightness);
    strip.fill(strip.Color(0, 255, 0));
    strip.show();
}

static void renderCometPulse(uint32_t baseColor, uint32_t elapsedMs) {
    const uint32_t orange = strip.Color(255, 110, 0);
    const uint32_t purple = strip.Color(120, 0, 180);
    const uint32_t green  = strip.Color(0, 220, 70);
    const uint32_t mint   = strip.Color(80, 255, 170);

    uint32_t primary, accent;
    if (g_successAnimationStyle == ANIMATION_STYLE_CUSTOM) {
        primary = (g_customThemeColorCount > 0) ? g_customThemeColors[0] : g_successAnimationColor;
        accent  = (g_customThemeColorCount > 1) ? g_customThemeColors[1] : primary;
    } else {
        primary = (g_successAnimationStyle == ANIMATION_STYLE_GREEN) ? green : orange;
        accent  = (g_successAnimationStyle == ANIMATION_STYLE_GREEN) ? mint : purple;
    }

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
        const uint32_t elapsed = now - g_successAnimationStart;

        // --- Phase 1: Comet trail sweeps clockwise once ---
        // One full rotation = LED_COUNT steps at 80 ms each.
        const uint32_t cometStepMs = 80;
        const uint32_t cometDuration = (uint32_t)LED_COUNT * cometStepMs;

        if (elapsed < cometDuration) {
            if (now - g_successAnimationLastFrame >= 16) {
                g_successAnimationLastFrame = now;
                strip.setBrightness(255);
                uint8_t head = (uint8_t)(elapsed / cometStepMs);
                if (head >= LED_COUNT) head = LED_COUNT - 1;
                renderCometTrail(head);
            }
            return;
        }

        // --- Phase 2: Rapid fill one-by-one ---
        // Each pixel lights in 50 ms; total = LED_COUNT * 50 ms.
        const uint32_t fillStepMs = 50;
        const uint32_t fillStart = cometDuration;
        const uint32_t fillDuration = (uint32_t)LED_COUNT * fillStepMs;

        if (elapsed < fillStart + fillDuration) {
            if (now - g_successAnimationLastFrame >= 16) {
                g_successAnimationLastFrame = now;
                strip.setBrightness(255);
                uint8_t filled = (uint8_t)((elapsed - fillStart) / fillStepMs) + 1;
                if (filled > LED_COUNT) filled = LED_COUNT;
                renderFillStep(filled);
            }
            return;
        }

        // --- Phase 3: Start chime + two green pulses, then stop ---
        if (g_successAnimationPhaseStart == 0) {
            g_successAnimationPhaseStart = now;
            Play_Default_Band_Chime();
            Serial.println("Default band chime: audio started, pulsing green.");
        }

        const uint32_t pulseElapsed = now - g_successAnimationPhaseStart;
        // Two full pulses over 1200 ms.
        const uint32_t pulseDuration = 1200;

        if (pulseElapsed >= pulseDuration) {
            stopSuccessAnimation();
            return;
        }

        if (now - g_successAnimationLastFrame >= 16) {
            g_successAnimationLastFrame = now;
            renderPulse(pulseElapsed);
        }
        return;
    }

    if (g_successAnimationStyle == ANIMATION_STYLE_BOO) {
        const uint32_t elapsed = now - g_successAnimationStart;

        // --- Phase 1: Rapid orange fill ---
        const uint32_t fillStepMs = 50;
        const uint32_t fillDuration = (uint32_t)LED_COUNT * fillStepMs;

        if (elapsed < fillDuration) {
            if (now - g_successAnimationLastFrame >= 16) {
                g_successAnimationLastFrame = now;
                strip.setBrightness(255);
                uint8_t filled = (uint8_t)(elapsed / fillStepMs) + 1;
                if (filled > LED_COUNT) filled = LED_COUNT;
                renderFillStep(filled, 255, 110, 0);
            }
            return;
        }

        // --- Phase 2: Pulse solid orange twice (1200 ms) ---
        const uint32_t pulseStart = fillDuration;
        const uint32_t pulseDuration = 1200;

        if (elapsed < pulseStart + pulseDuration) {
            if (now - g_successAnimationLastFrame >= 16) {
                g_successAnimationLastFrame = now;
                const uint32_t pulseElapsed = elapsed - pulseStart;
                const float angle = (float)pulseElapsed / 600.0f * 2.0f * 3.14159f;
                float s = sinf(angle);
                uint8_t brightness = (uint8_t)(40.0f + (215.0f * (s * 0.5f + 0.5f)));
                strip.setBrightness(brightness);
                strip.fill(strip.Color(255, 110, 0));
                strip.show();
            }
            return;
        }

        // --- Phase 3: Start audio + orange/purple comet until song ends ---
        if (g_successAnimationPhaseStart == 0) {
            g_successAnimationPhaseStart = now;
            Play_Music_theme(7);
            Serial.println("Boo To You: audio started, orange/purple comet running.");
        }

        if (now - g_successAnimationLastFrame >= 16) {
            g_successAnimationLastFrame = now;
            strip.setBrightness(255);
            AnimationStyle saved = g_successAnimationStyle;
            g_successAnimationStyle = ANIMATION_STYLE_SPOOKY;
            renderCometPulse(g_successAnimationColor, now - g_successAnimationPhaseStart);
            g_successAnimationStyle = saved;
        }

        const uint32_t cometElapsed = now - g_successAnimationPhaseStart;
        const bool audioEnded = !audio.isRunning();
        const bool minRuntimeMet = cometElapsed >= 500;
        const bool timedOut = cometElapsed >= 30000;
        if ((audioEnded && minRuntimeMet) || timedOut) {
            stopSuccessAnimation();
        }
        return;
    }

    // ---- Custom theme animation dispatcher ----
    if (g_successAnimationStyle == ANIMATION_STYLE_CUSTOM) {
        const uint32_t elapsed = now - g_successAnimationStart;

        // Compute phase boundaries based on which phases are enabled.
        const uint32_t cometDur = (g_customThemePhases & CTP_PHASE_COMET) ? (uint32_t)LED_COUNT * 80 : 0;
        const uint32_t fillDur  = (g_customThemePhases & CTP_PHASE_FILL)  ? (uint32_t)LED_COUNT * 50 : 0;
        const uint32_t pulseDur = (g_customThemePhases & CTP_PHASE_PULSE) ? 1200U : 0;
        const uint32_t cometEnd = cometDur;
        const uint32_t fillEnd  = cometEnd + fillDur;
        const uint32_t pulseEnd = fillEnd  + pulseDur;

        // Primary colour for opening phases
        const uint32_t c0 = (g_customThemeColorCount > 0) ? g_customThemeColors[0] : strip.Color(0, 255, 0);
        const uint8_t r0 = (c0 >> 16) & 0xFF;
        const uint8_t g0 = (c0 >>  8) & 0xFF;
        const uint8_t b0 =  c0        & 0xFF;

        // --- Comet Trail phase ---
        if ((g_customThemePhases & CTP_PHASE_COMET) && elapsed < cometEnd) {
            if (now - g_successAnimationLastFrame >= 16) {
                g_successAnimationLastFrame = now;
                strip.setBrightness(255);
                uint8_t head = (uint8_t)(elapsed / 80);
                if (head >= LED_COUNT) head = LED_COUNT - 1;
                strip.clear();
                const uint8_t levels[] = {255, 153, 64, 20};
                for (uint8_t t = 0; t < 4 && t < LED_COUNT; t++) {
                    uint8_t idx = (head + LED_COUNT - t) % LED_COUNT;
                    strip.setPixelColor(idx, strip.Color(scaleChan(r0, levels[t]),
                                                         scaleChan(g0, levels[t]),
                                                         scaleChan(b0, levels[t])));
                }
                strip.show();
            }
            return;
        }

        // --- Fill phase ---
        if ((g_customThemePhases & CTP_PHASE_FILL) && elapsed < fillEnd) {
            if (now - g_successAnimationLastFrame >= 16) {
                g_successAnimationLastFrame = now;
                strip.setBrightness(255);
                const uint32_t fillElapsed = elapsed - cometEnd;
                uint8_t filled = (uint8_t)(fillElapsed / 50) + 1;
                if (filled > LED_COUNT) filled = LED_COUNT;
                strip.clear();
                for (uint8_t pi = 0; pi < filled; pi++) {
                    uint8_t ci = (g_customThemeColorCount > 1) ? (pi % g_customThemeColorCount) : 0;
                    strip.setPixelColor(pi, g_customThemeColors[ci]);
                }
                strip.show();
            }
            return;
        }

        // --- Pulse phase ---
        if ((g_customThemePhases & CTP_PHASE_PULSE) && elapsed < pulseEnd) {
            if (now - g_successAnimationLastFrame >= 16) {
                g_successAnimationLastFrame = now;
                const uint32_t pulseElapsed = elapsed - fillEnd;
                const float angle = (float)pulseElapsed / 600.0f * 2.0f * 3.14159f;
                uint8_t br = (uint8_t)(40.0f + 215.0f * (sinf(angle) * 0.5f + 0.5f));
                strip.setBrightness(br);
                strip.fill(c0);
                strip.show();
            }
            return;
        }

        // --- Loop phase (runs until audio ends or 30s timeout) ---
        if (g_successAnimationPhaseStart == 0) g_successAnimationPhaseStart = now;
        const uint32_t loopElapsed = now - g_successAnimationPhaseStart;

        if (g_customThemeLoop == CTL_NONE) {
            stopSuccessAnimation();
            return;
        }

        // Player-lightshow modes (4+): once the opening phases complete, hand off.
        // g_playerLightshow was already set in startCustomThemeAnimation(); the player
        // renderer will take over since g_playerActive is true and isSuccessActive will be false.
        if (g_customThemeLoop >= CTL_LS_RAINBOW) {
            stopSuccessAnimation();
            return;
        }

        if (g_customThemeLoop == CTL_GENTLE_PULSE) {
            if (now - g_successAnimationLastFrame >= 16) {
                g_successAnimationLastFrame = now;
                // Cross-fade between colours over a 4 s cycle each
                const uint32_t cyclePeriod = 4000;
                const uint32_t cyclePos = loopElapsed % cyclePeriod;
                const uint8_t ci0 = (g_customThemeColorCount > 1)
                    ? (uint8_t)((loopElapsed / cyclePeriod) % g_customThemeColorCount) : 0;
                const uint8_t ci1 = (ci0 + 1) % (g_customThemeColorCount > 0 ? g_customThemeColorCount : 1);
                const float t = (float)cyclePos / (float)cyclePeriod;
                const uint32_t c0 = g_customThemeColors[ci0];
                const uint32_t c1 = (g_customThemeColorCount > 1) ? g_customThemeColors[ci1] : c0;
                const uint8_t lr = (uint8_t)((1.0f - t) * ((c0 >> 16) & 0xFF) + t * ((c1 >> 16) & 0xFF));
                const uint8_t lg = (uint8_t)((1.0f - t) * ((c0 >>  8) & 0xFF) + t * ((c1 >>  8) & 0xFF));
                const uint8_t lb = (uint8_t)((1.0f - t) * ( c0        & 0xFF) + t * ( c1        & 0xFF));
                const float angle = (float)(loopElapsed % 2000) / 2000.0f * 2.0f * 3.14159f;
                uint8_t br = (uint8_t)(15.0f + 185.0f * (sinf(angle) * 0.5f + 0.5f));
                strip.setBrightness(br);
                strip.fill(strip.Color(lr, lg, lb));
                strip.show();
            }
        } else {
            // CTL_SPINNING_COMET or CTL_RAINBOW_SPIN
            if (now - g_successAnimationLastFrame >= 16) {
                g_successAnimationLastFrame = now;
                strip.setBrightness(255);
                if (g_customThemeLoop == CTL_RAINBOW_SPIN && g_customThemeColorCount > 1) {
                    uint8_t ci0 = (uint8_t)((loopElapsed / 800) % g_customThemeColorCount);
                    uint8_t ci1 = (ci0 + 1) % g_customThemeColorCount;
                    uint32_t s0 = g_customThemeColors[0], s1 = g_customThemeColors[1];
                    g_customThemeColors[0] = g_customThemeColors[ci0];
                    g_customThemeColors[1] = g_customThemeColors[ci1];
                    renderCometPulse(g_customThemeColors[0], loopElapsed);
                    g_customThemeColors[0] = s0;
                    g_customThemeColors[1] = s1;
                } else {
                    renderCometPulse(g_successAnimationColor, loopElapsed);
                }
            }
        }

        const bool audioEnded2 = !audio.isRunning();
        if (audioEnded2 && loopElapsed >= 250) {
            stopSuccessAnimation();
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

// Dispatch a custom theme animation; audio must already be triggered before calling.
static void startCustomThemeAnimation(const CustomTheme& ct) {
    g_customThemePhases = ct.phases;
    g_customThemeLoop   = ct.loopPattern;
    // Convert 0xRRGGBB web colours to NeoPixel-packed format and store in array.
    g_customThemeColorCount = (ct.colorCount < 1) ? 1
                            : (ct.colorCount > CUSTOM_THEME_MAX_COLORS) ? CUSTOM_THEME_MAX_COLORS
                            : ct.colorCount;
    for (uint8_t ci = 0; ci < g_customThemeColorCount; ci++) {
        uint32_t c = ct.colors[ci];
        g_customThemeColors[ci] = strip.Color((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    }
    // g_successAnimationColor holds the primary for backward compat with renderCometPulse.
    startScanAnimation(ANIMATION_STYLE_CUSTOM, g_customThemeColors[0]);

    // For player-lightshow loop modes (4+): pre-arm the lightshow and colour override.
    // The loop phase will call stopSuccessAnimation() and hand off to the player renderer.
    if (ct.loopPattern >= CTL_LS_RAINBOW) {
        g_playerLightshow = ct.loopPattern - 3; // maps 4→1, 5→2, … 13→10
    }
    // Colour override — copy theme palette so the player lightshow can use it
    g_playerLsUseThemeColors  = (ct.lsUseThemeColors != 0);
    g_playerLsThemeColorCount = g_customThemeColorCount;
    for (uint8_t ci = 0; ci < g_customThemeColorCount; ci++) {
        g_playerLsThemeColors[ci] = g_customThemeColors[ci];
    }
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
    Wire.write(0x03); Wire.write(0x00); // Port1 LOW — PA disabled until after audio/LED init
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
        startScanAnimation(ANIMATION_STYLE_BOO, color);
        Serial.println("Boo To You animation started.");
        return;
    }

    if (themeId >= CUSTOM_THEME_ID_BASE) {
        int cidx = findCustomTheme(themeId);
        if (cidx >= 0) {
            if (strlen(customThemes[cidx].audioFile) > 0) {
                char path[72];
                snprintf(path, sizeof(path), "/%s", customThemes[cidx].audioFile);
                bool played = Play_Music_file(path);
                if (played) {
                    // Sync player state so the /player page reflects this track
                    strncpy(g_playlist[0].name, customThemes[cidx].audioFile, sizeof(g_playlist[0].name) - 1);
                    g_playlistCount = 1;
                    g_playerIndex   = 0;
                    g_playerActive  = true;
                    g_playerPaused  = false;
                    g_playerRepeat  = false; // play once (user can toggle on /player)
                } else {
                    // SD file missing or card absent — fall back to default chime (LittleFS if needed)
                    Play_Default_Band_Chime();
                }
            } else {
                Play_Default_Band_Chime();
            }
            startCustomThemeAnimation(customThemes[cidx]);
            Serial.printf("Custom theme '%s' (id=%u) animation started.\n",
                          customThemes[cidx].name, themeId);
            return;
        }
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
    delay(150); // let NeoPixel inrush settle before enabling speaker amp
    // Enable PA: PCA9555 Port1 = 0xFF (EXIO8 high)
    Wire.beginTransmission(0x20); Wire.write(0x03); Wire.write(0xFF); Wire.endTransmission();

    loadCategoriesFromPrefs();
    prefs.begin("mbands", false);
    int rc = prefs.getInt("count", 0); bandCount = (rc < 0 || rc > 50) ? 0 : rc;
    for(int i=0; i<bandCount; i++) prefs.getBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
    prefs.end();
    loadCustomThemesFromPrefs();
    
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

    // Priority 2: Button Polling
    // Key 1 (bit 1 / 0x02) = Volume Up
    // Key 2 (bit 2 / 0x04) = Stop music
    // Key 3 (bit 3 / 0x08) = Volume Down
    static uint32_t lastBtn = 0;
    static uint8_t  lastBtnState = 0xFF;
    if (millis() - lastBtn > 150) {
        lastBtn = millis();

        Wire.beginTransmission(0x20);
        Wire.write(0x01);
        if (Wire.endTransmission() == 0) {
            Wire.requestFrom(0x20, 1);
            if (Wire.available()) {
                uint8_t input = Wire.read();
                uint8_t pressed = (~input) & ~lastBtnState; // newly-pressed edges only
                lastBtnState = ~input;

                if (pressed & 0x04) {
                    Serial.println("Button 2: Stop music");
                    Music_stop();
                    stopSuccessAnimation();
                }
                if (pressed & 0x02) {
                    uint8_t v = (g_volume < 21) ? g_volume + 1 : 21;
                    Serial.printf("Button 1: Volume Up -> %u\n", v);
                    Music_set_volume(v);
                }
                if (pressed & 0x08) {
                    uint8_t v = (g_volume > 0) ? g_volume - 1 : 0;
                    Serial.printf("Button 3: Volume Down -> %u\n", v);
                    Music_set_volume(v);
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

    // ---- Player lightshow (only when player is active and no band animation running) ----
    if (g_playerActive && !g_playerPaused && !isSuccessActive && !g_webScanArmed && g_playerLightshow > 0) {
        uint32_t now = millis();
        static const uint32_t kLsInterval[] = {200, 75, 25}; // sedate/medium/lively (ms between frames)
        uint8_t spd = g_playerLsSpeed < 3 ? g_playerLsSpeed : 1;
        if (now - g_playerLsFrame > kLsInterval[spd]) {
            g_playerLsFrame = now;
            g_playerLsStep++;
            if (g_playerLightshow == 1) {
                // Rainbow spin — by definition uses full spectrum, theme colours not applied
                strip.setBrightness(80);
                for (int i = 0; i < LED_COUNT; i++) {
                    uint16_t hue = ((uint32_t)g_playerLsStep * 65536 / LED_COUNT + (uint32_t)i * 65536 / LED_COUNT) & 0xFFFF;
                    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(hue, 255, 200)));
                }
                strip.show();
            } else if (g_playerLightshow == 2) {
                // Gentle pulse
                static const uint32_t kPulsePeriod[] = {6000, 3000, 1500};
                float t = (float)(now % kPulsePeriod[spd]) / (float)kPulsePeriod[spd];
                uint8_t b = (uint8_t)(40.0f + 70.0f * sinf(t * 6.2832f));
                strip.setBrightness(b);
                uint32_t pulseCol = (g_playerLsUseThemeColors && g_playerLsThemeColorCount > 0)
                    ? g_playerLsThemeColors[0] : strip.Color(255, 255, 255);
                strip.fill(pulseCol);
                strip.show();
            } else if (g_playerLightshow == 3) {
                // Colour cycle
                uint8_t palSize;
                const uint32_t* pal;
                static const uint32_t kCols[] = {0xFF0000, 0xFF8800, 0xFFFF00, 0x00FF00, 0x0000FF, 0xAA00FF};
                if (g_playerLsUseThemeColors && g_playerLsThemeColorCount > 0) {
                    pal = g_playerLsThemeColors; palSize = g_playerLsThemeColorCount;
                } else {
                    pal = kCols; palSize = 6;
                }
                uint8_t ci = (g_playerLsStep / 32) % palSize;
                uint32_t c = pal[ci];
                float t2 = (float)(g_playerLsStep % 32) / 32.0f;
                uint8_t b2 = (uint8_t)(80.0f + 96.0f * sinf(t2 * 3.1416f));
                strip.setBrightness(b2);
                strip.fill(c);
                strip.show();
            } else if (g_playerLightshow == 4) {
                // Main Street USA — warm golden twinkle; theme override uses primary color
                strip.setBrightness(90);
                uint32_t baseCol = (g_playerLsUseThemeColors && g_playerLsThemeColorCount > 0)
                    ? g_playerLsThemeColors[0] : strip.Color(255, 160, 20);
                uint8_t br4 = (baseCol >> 16) & 0xFF, bg4 = (baseCol >> 8) & 0xFF, bb4 = baseCol & 0xFF;
                for (int i = 0; i < LED_COUNT; i++) {
                    uint8_t phase = (uint8_t)((g_playerLsStep + i * 7) & 0xFF);
                    uint8_t bright = (uint8_t)(80 + 70 * sinf(phase * 0.02454f));
                    strip.setPixelColor(i, strip.Color(
                        (bright * br4) / 200, (bright * bg4) / 200, (bright * bb4) / 200));
                }
                strip.show();
            } else if (g_playerLightshow == 5) {
                // Adventureland — comet; theme override swaps palette
                static const uint32_t kAdv[] = {0x00AA22, 0x007700, 0xFF6600, 0xCC4400, 0x004400};
                uint8_t palSize5 = 5;
                const uint32_t* pal5 = kAdv;
                if (g_playerLsUseThemeColors && g_playerLsThemeColorCount > 0) {
                    pal5 = g_playerLsThemeColors; palSize5 = g_playerLsThemeColorCount;
                }
                uint8_t head = g_playerLsStep % LED_COUNT;
                strip.setBrightness(100);
                strip.clear();
                for (int t = 0; t < 5; t++) {
                    int idx = (head + LED_COUNT - t) % LED_COUNT;
                    uint32_t c5 = pal5[t % palSize5];
                    uint8_t r5 = ((c5>>16)&0xFF) >> (t/2);
                    uint8_t g5 = ((c5>>8)&0xFF)  >> (t/2);
                    uint8_t b5 = (c5&0xFF)        >> (t/2);
                    strip.setPixelColor(idx, strip.Color(r5,g5,b5));
                }
                strip.show();
            } else if (g_playerLightshow == 6) {
                // Frontierland — flicker; theme override uses primary colour
                strip.setBrightness(110);
                uint32_t baseCol6 = (g_playerLsUseThemeColors && g_playerLsThemeColorCount > 0)
                    ? g_playerLsThemeColors[0] : strip.Color(255, 50, 0);
                uint8_t br6 = (baseCol6 >> 16) & 0xFF, bg6 = (baseCol6 >> 8) & 0xFF, bb6 = baseCol6 & 0xFF;
                for (int i = 0; i < LED_COUNT; i++) {
                    uint8_t fl = (uint8_t)(100 + 80 * sinf((g_playerLsStep + i * 13) * 0.031f)
                                               + 40 * sinf((g_playerLsStep + i * 5)  * 0.071f));
                    strip.setPixelColor(i, strip.Color(
                        (fl * br6) / 255, (fl * bg6) / 255, (fl * bb6) / 255));
                }
                strip.show();
            } else if (g_playerLightshow == 7) {
                // Liberty Square — ripple; theme override replaces palette
                static const uint32_t kLib[] = {0xFF0000, 0xFFFFFF, 0x0000CC};
                const uint32_t* pal7 = kLib; uint8_t palSize7 = 3;
                if (g_playerLsUseThemeColors && g_playerLsThemeColorCount > 0) {
                    pal7 = g_playerLsThemeColors; palSize7 = g_playerLsThemeColorCount;
                }
                strip.setBrightness(90);
                for (int i = 0; i < LED_COUNT; i++) {
                    uint8_t ci7 = ((i + (g_playerLsStep / 8)) / 2) % palSize7;
                    strip.setPixelColor(i, pal7[ci7]);
                }
                strip.show();
            } else if (g_playerLightshow == 8) {
                // Fantasyland — pastel HSV spin; theme override shifts hue center to match color[0]
                strip.setBrightness(80);
                uint16_t hueCenter8 = 46500; // default pink-purple
                if (g_playerLsUseThemeColors && g_playerLsThemeColorCount > 0) {
                    // Approximate hue from NeoPixel color
                    uint8_t tr = (g_playerLsThemeColors[0] >> 16) & 0xFF;
                    uint8_t tg = (g_playerLsThemeColors[0] >>  8) & 0xFF;
                    uint8_t tb =  g_playerLsThemeColors[0]        & 0xFF;
                    // Quick dominant-channel hue estimate (0..65535)
                    if (tr >= tg && tr >= tb)      hueCenter8 = (uint16_t)(((long)tg - tb) * 65536 / (6 * (tr - (tg < tb ? tg : tb) + 1)) + 65536) % 65536;
                    else if (tg >= tr && tg >= tb) hueCenter8 = (uint16_t)(21845 + (long)(tb - tr) * 65536 / (6 * (tg - (tr < tb ? tr : tb) + 1)));
                    else                           hueCenter8 = (uint16_t)(43690 + (long)(tr - tg) * 65536 / (6 * (tb - (tr < tg ? tr : tg) + 1)));
                }
                for (int i = 0; i < LED_COUNT; i++) {
                    uint16_t hue8 = (uint16_t)(((uint32_t)g_playerLsStep * 320 + i * 4000) & 0xFFFF);
                    uint16_t h8 = (uint16_t)(hueCenter8 + (hue8 % 11000) - 5500) & 0xFFFF;
                    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(h8, 180, 220)));
                }
                strip.show();
            } else if (g_playerLightshow == 9) {
                // Tomorrowland — electric spin
                strip.setBrightness(110);
                for (int i = 0; i < LED_COUNT; i++) {
                    uint16_t hue9 = (uint16_t)(((uint32_t)g_playerLsStep * 800 + (uint32_t)i * 65536 / LED_COUNT) & 0xFFFF);
                    uint16_t h9 = 40000 + (hue9 % 12000);
                    uint8_t sat9 = (i % 4 == 0) ? 60 : 255;
                    strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(h9, sat9, 230)));
                }
                strip.show();
            } else if (g_playerLightshow == 10) {
                // Haunted Mansion — ghost comet; theme override changes ghost colour
                strip.setBrightness(60);
                uint8_t ghostR = 20, ghostG = 180, ghostB = 40; // default sickly green
                if (g_playerLsUseThemeColors && g_playerLsThemeColorCount > 0) {
                    uint32_t gc = g_playerLsThemeColors[0];
                    ghostR = (gc >> 16) & 0xFF; ghostG = (gc >> 8) & 0xFF; ghostB = gc & 0xFF;
                }
                uint8_t head10 = (g_playerLsStep / 2) % LED_COUNT;
                for (int i = 0; i < LED_COUNT; i++) {
                    uint8_t base_r = 20, base_g = 0, base_b = 18;
                    int dist = (i - head10 + LED_COUNT) % LED_COUNT;
                    if (dist < 6) {
                        uint8_t fade10 = (uint8_t)(180 >> (dist / 1));
                        base_r = (uint8_t)((uint32_t)fade10 * ghostR / 180);
                        base_g = (uint8_t)((uint32_t)fade10 * ghostG / 180);
                        base_b = (uint8_t)((uint32_t)fade10 * ghostB / 180);
                    }
                    if ((g_playerLsStep & 7) == 0 && (i * 17 + g_playerLsStep) % 11 == 0) {
                        base_r = 80; base_g = 80; base_b = 100;
                    }
                    strip.setPixelColor(i, strip.Color(base_r, base_g, base_b));
                }
                strip.show();
            }
        }
    } else if (!g_playerActive && g_playerLightshow > 0) {
        // Player stopped — clear LEDs once
        static bool lsClearedOnStop = false;
        if (!lsClearedOnStop && !isSuccessActive) {
            strip.clear(); strip.show();
            lsClearedOnStop = true;
        }
        if (g_playerActive) lsClearedOnStop = false; // reset for next play
    } else {
        // Reset clear-flag whenever player becomes active again
        static bool _lsActive = false;
        if (g_playerActive && !_lsActive) { _lsActive = true; }
        if (!g_playerActive) { _lsActive = false; }
    }

    // Auto-advance to next track.
    // Debounce: wait 500ms after isRunning() first goes false before starting the next
    // track. The audio library's internal I2S/DMA teardown is asynchronous; calling
    // connecttoFS() immediately after isRunning() becomes false causes a LoadProhibited
    // crash because internal tasks are still reading the old buffer pointers.
    {
        static uint32_t trackEndedAt = 0;
        const bool trackDone = g_playerActive && !g_playerPaused
                               && !audio.isRunning() && g_playlistCount > 0
                               && g_playerMuteUntil == 0;
        if (!trackDone) {
            trackEndedAt = 0; // reset while playing / inactive
        } else {
            if (trackEndedAt == 0) {
                trackEndedAt = millis(); // note when it ended — start debounce
            } else if (millis() - trackEndedAt >= 500) {
                trackEndedAt = 0;
                int next;
                if (g_playerRepeatOne) {
                    next = g_playerIndex; // replay same track
                } else {
                    next = g_playerIndex + 1;
                    if (next >= g_playlistCount) {
                        if (g_playerRepeat) {
                            next = 0;
                        } else {
                            g_playerActive = false;
                            if (g_playerLightshow > 0) { strip.clear(); strip.show(); }
                            goto skipAutoAdvance;
                        }
                    }
                }
                g_playerIndex = next;
                { char path[80]; snprintf(path, sizeof(path), "/%s", g_playlist[g_playerIndex].name);
                  if (!Play_Music_file(path)) { g_playerActive = false; } }
                skipAutoAdvance:;
            }
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
                // When armed via web, skip the full theme animation for known bands —
                // just do a brief white flash so the user gets tactile feedback.
                if(g_webScanIsKnown) {
                    strip.fill(strip.Color(200, 200, 200));
                    strip.setBrightness(180);
                    strip.show();
                    delay(200);
                    strip.clear();
                    strip.show();
                    return;
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