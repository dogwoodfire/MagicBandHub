#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

struct BandRecord {
    uint8_t uid[7];
    uint32_t color;
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