#ifndef WEB_PORTAL_H
#define WEB_PORTAL_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Shared struct
struct BandRecord {
    uint8_t uid[7];
    uint32_t color;
    char name[20];
    char imageUrl[100]; // New field for the image link
};

#ifdef __cplusplus
extern "C" {
#endif

void initWebServer();
void startWebServer();
void stopWebServer();

#ifdef __cplusplus
}
#endif

#endif