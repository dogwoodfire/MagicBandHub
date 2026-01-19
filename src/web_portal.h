#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

struct BandRecord {
    uint8_t uid[7];
    uint32_t color;
    char name[20];
    char imageUrl[100];
    char type[20];
    char dateBought[12]; // NEW: Stores YYYY-MM-DD
    char owner[20];      // NEW: Stores selected owner name
    char location[20];   // NEW: Stores selected location name
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