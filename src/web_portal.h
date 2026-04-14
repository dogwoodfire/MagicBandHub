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
    // Modes 4-13: mirrors g_playerLightshow values 1-10 (loopPattern - 3 = playerLightshow)
    CTL_LS_RAINBOW     = 4,  // player lightshow: rainbow spin
    CTL_LS_PULSE       = 5,  // player lightshow: gentle pulse
    CTL_LS_COLOUR_CYCLE= 6,  // player lightshow: colour cycle
    CTL_LS_MAIN_STREET = 7,  // player lightshow: Main Street USA
    CTL_LS_ADVENTURE   = 8,  // player lightshow: Adventureland
    CTL_LS_FRONTIER    = 9,  // player lightshow: Frontierland
    CTL_LS_LIBERTY     = 10, // player lightshow: Liberty Square
    CTL_LS_FANTASY     = 11, // player lightshow: Fantasyland
    CTL_LS_TOMORROW    = 12, // player lightshow: Tomorrowland
    CTL_LS_HAUNTED     = 13, // player lightshow: Haunted Mansion
};
#define CUSTOM_THEME_LOOP_COUNT 14

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
    uint8_t  lsUseThemeColors;               // 1 = lightshow uses this theme's colour palette
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
    uint8_t checkedOut;     // 1 = packed/checked-out, 0 = at home
};

void initWebServer();
bool tryConnectSavedWiFi();
void loadCustomThemesFromPrefs();
int  findCustomTheme(uint16_t themeId); // returns index in customThemes[] or -1

extern CustomTheme customThemes[];
extern int customThemeCount;

// ---- Music Player shared state ----
struct PlayerTrack { char name[64]; };
extern PlayerTrack g_playlist[];
extern int         g_playlistCount;
extern int         g_playerIndex;
extern bool        g_playerActive;
extern bool        g_playerPaused;
extern uint8_t     g_playerLightshow;
extern uint8_t     g_playerLsSpeed;
extern bool        g_playerRepeat;
extern bool        g_playerRepeatOne;
// Colour override for player lightshow — set when a theme with lsUseThemeColors=1 triggers
extern bool        g_playerLsUseThemeColors;
extern uint32_t    g_playerLsThemeColors[5];  // NeoPixel-packed WRGB
extern uint8_t     g_playerLsThemeColorCount;

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