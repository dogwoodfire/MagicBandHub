#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Shared structure - 131 bytes total
struct BandRecord {
    uint8_t uid[7];    // Chip serial number
    uint32_t color;    // LED ring color
    char name[20];      // Nickname
    char imageUrl[100]; // Remote image link
};

#ifdef __cplusplus
extern "C" {
#endif

void initWebServer();
void startWebServer();
void stopWebServer();
bool tryConnectSavedWiFi(); // Connection manager

#ifdef __cplusplus
}
#endif

#endif