#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// ---- Custom theme phase bitmask flags ----
#define CTP_PHASE_COMET  0x01   // comet trail sweeps round once
#define CTP_PHASE_FILL   0x02   // ring fills one pixel at a time
#define CTP_PHASE_PULSE  0x04   // ring pulses (2× 600ms) before loop

// ---- Looping pattern played after the phases (for audio duration) ----
enum CustomThemeLoop : uint8_t {
    CTL_NONE           = 0,  // stop after phases complete
    CTL_SPINNING_COMET = 1,  // spinning comet until audio ends
    CTL_GENTLE_PULSE   = 2,  // slow sinusoidal pulse until audio ends
    CTL_RAINBOW_SPIN   = 3,  // cycles through band colours until audio ends
};
#define CUSTOM_THEME_LOOP_COUNT 4

#define CUSTOM_THEME_MAX_COLORS 5   // 1 required + up to 4 extra

// Custom themes are stored under themeId 100..109 in BandRecord.
#define CUSTOM_THEME_ID_BASE 100
#define CUSTOM_THEME_MAX     10

struct CustomTheme {
    uint16_t id;                              // 100..109
    char     name[32];
    uint8_t  phases;                          // bitmask of CTP_PHASE_* flags
    uint8_t  loopPattern;                     // CustomThemeLoop
    uint32_t colors[CUSTOM_THEME_MAX_COLORS]; // 0xRRGGBB; colors[0] required
    uint8_t  colorCount;                      // 1..CUSTOM_THEME_MAX_COLORS
    char     audioFile[64];                   // filename on SD root
};

struct BandRecord {
    uint8_t uid[7];
    uint32_t color;
    uint16_t themeId; // 0 = Default theme
    char name[40];
    char imageUrl[100];
    char type[20];
    char dateBought[12]; // YYYY-MM-DD
    char owner[20];
    char location[20];

    // MagicBandCollectors imported metadata (optional)
    char releaseType[24];   // Limited Release / Open Edition
    char releaseDate[24];   // July 28, 2025
    char releasedAt[120];   // Disneyland, Walt Disney World, ShopDisney
    char bandColorName[32]; // Black
    char iconColorName[32]; // Custom Graphics
    char originalPrice[16]; // $54.99
    char sku[80];           // 400..., 419...
    char mbcListing[64];    // e.g. "2526" or full URL
};

void initWebServer();
bool tryConnectSavedWiFi();
void loadCustomThemesFromPrefs();
int  findCustomTheme(uint16_t themeId); // returns index in customThemes[] or -1

extern CustomTheme customThemes[];
extern int customThemeCount;

#ifdef __cplusplus
extern "C" {
#endif
    void startWebServer();
    void stopWebServer();
    void fn_toggle_wifi(struct _lv_event_t * e);
    void fn_refresh_roller(struct _lv_event_t * e);
    void loadCategoriesFromPrefs();
#ifdef __cplusplus
}
#endif

#endif