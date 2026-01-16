#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

struct BandRecord {
    uint8_t uid[7];
    uint32_t color;
    char name[20];
    char imageUrl[100];
};

// Logic functions (C++)
void initWebServer();
bool tryConnectSavedWiFi();

// UI Callbacks (C Linkage)
#ifdef __cplusplus
extern "C" {
#endif
    void startWebServer();
    void stopWebServer();
    void fn_toggle_wifi(struct _lv_event_t * e);
    void fn_refresh_roller(struct _lv_event_t * e);
#ifdef __cplusplus
}
#endif

#endif