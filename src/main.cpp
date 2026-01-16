#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <ui.h>           
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
#define LED_PIN 21
#define LED_COUNT 12
#define TFT_BL 2 
#define TOUCH_SDA 6
#define TOUCH_SCL 7
#define TOUCH_RST 13
#define NFC_SDA 15
#define NFC_SCL 16
#define NFC_IRQ 17
#define LCD_RST 14

AsyncWebServer server(80); 
TFT_eSPI tft = TFT_eSPI(240, 240); 
TwoWire I2C_NFC = TwoWire(1); 
Adafruit_PN532 nfc(NFC_IRQ, LCD_RST, &I2C_NFC); 
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences prefs;

BandRecord registeredBands[10];
int bandCount = 0;
BandRecord tempRecord; 
bool isWaitingForUID = false;
bool isSuccessActive = false;
bool isWiFiActive = false;

extern "C" {
    extern lv_obj_t * ui_Scanner, * ui_mickeyScanner, * ui_RecordScreen, * ui_ScanBandPnl1, * ui_ScanBandPnl2, * ui_ScanBandPnl3, * ui_BandRoller, * ui_RegisterConfirmPnl, * ui_NewBandConfirm, * ui_StatusLabel, * ui_EditNameLabel;
}

// Display/Touch Drivers
void my_disp_flush(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* c) {
    uint32_t w=(a->x2-a->x1+1), h=(a->y2-a->y1+1);
    tft.startWrite(); tft.setAddrWindow(a->x1,a->y1,w,h);
    tft.pushColors((uint16_t*)&c->full,w*h,true); tft.endWrite();
    lv_disp_flush_ready(d);
}

bool get_raw_touch(int16_t &x, int16_t &y) {
    Wire.beginTransmission(0x15); Wire.write(0x02);
    if(Wire.endTransmission()!=0) return false;
    Wire.requestFrom(0x15, 6);
    if(Wire.available()==6){
        uint8_t p=Wire.read(), xh=Wire.read(), xl=Wire.read(), yh=Wire.read(), yl=Wire.read(); Wire.read();
        if(p>0 && p<5){ x=((xh&0x0F)<<8)|xl; y=((yh&0x0F)<<8)|yl; return true; }
    }
    return false;
}

void my_touchpad_read(lv_indev_drv_t* d, lv_indev_data_t* data) {
    int16_t tx, ty;
    if(get_raw_touch(tx, ty)){ data->state=LV_INDEV_STATE_PR; data->point.x=tx; data->point.y=ty; }
    else data->state=LV_INDEV_STATE_REL;
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

void handleSuccess(uint32_t color) {
    if(isSuccessActive) return;
    isSuccessActive = true;
    if(ui_mickeyScanner){
        lv_obj_set_style_img_recolor(ui_mickeyScanner, lv_color_hex(color), 0);
        lv_obj_set_style_img_recolor_opa(ui_mickeyScanner, 255, 0);
    }
    // STAGE 1: White Comet Swirl (1 rotation)
    for(int f=0; f < LED_COUNT; f++){
        strip.clear();
        for(int i=0; i<5; i++){
            int p=(f-i+LED_COUNT)%LED_COUNT;
            int b=255-(i*50); if(b<0) b=0;
            strip.setPixelColor(p, strip.Color(b, b, b)); 
        }
        strip.show(); delay(SPEED_COMET); lv_timer_handler();
    }
    // STAGE 2: Progressive Fill (White, 1 to 12)
    strip.clear();
    for(int i=0; i < LED_COUNT; i++) {
        strip.setPixelColor(i, strip.Color(255, 255, 255));
        strip.show(); delay(SPEED_FILL); lv_timer_handler();
    }
    // STAGE 3: Pulse Breathing Fade (Band Color)
    for(int pulse = 0; pulse < 2; pulse++){
        for(int b = 30; b <= 255; b += 10){ strip.fill(strip.gamma32(color)); strip.setBrightness(b); strip.show(); delay(SPEED_PULSE); lv_timer_handler(); }
        for(int b = 255; b >= 30; b -= 10){ strip.fill(strip.gamma32(color)); strip.setBrightness(b); strip.show(); delay(SPEED_PULSE); lv_timer_handler(); }
    }
    if(ui_mickeyScanner) lv_obj_set_style_img_recolor_opa(ui_mickeyScanner, 0, 0);
    strip.clear(); strip.setBrightness(40); strip.show();
    isSuccessActive = false;
}

void reset_record_panels() {
    if(ui_ScanBandPnl1) lv_obj_clear_flag(ui_ScanBandPnl1, LV_OBJ_FLAG_HIDDEN);
    if(ui_ScanBandPnl2) lv_obj_add_flag(ui_ScanBandPnl2, LV_OBJ_FLAG_HIDDEN);
    if(ui_ScanBandPnl3) lv_obj_add_flag(ui_ScanBandPnl3, LV_OBJ_FLAG_HIDDEN);
}

// Callbacks (C Linkage)
extern "C" {
    void ui_event_RegisterFromPopup(lv_event_t * e) {
        if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
            if (bandCount < 10) {
                memcpy(registeredBands[bandCount].uid, tempRecord.uid, 7);
                snprintf(registeredBands[bandCount].name, 20, "MagicBand %d", bandCount + 1);
                registeredBands[bandCount].color = 0x00FF00;
                memset(registeredBands[bandCount].imageUrl, 0, 100);
                bandCount++;
                prefs.begin("mbands", false);
                prefs.putInt("count", bandCount);
                prefs.putBytes(("b" + String(bandCount - 1)).c_str(), &registeredBands[bandCount-1], sizeof(BandRecord));
                prefs.end();
            }
            if(ui_RegisterConfirmPnl) lv_obj_add_flag(ui_RegisterConfirmPnl, LV_OBJ_FLAG_HIDDEN);
            _ui_screen_change(&ui_RecordScreen, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, &ui_RecordScreen_screen_init);
            if(ui_ScanBandPnl1) lv_obj_add_flag(ui_ScanBandPnl1, LV_OBJ_FLAG_HIDDEN);
            if(ui_ScanBandPnl2) lv_obj_add_flag(ui_ScanBandPnl2, LV_OBJ_FLAG_HIDDEN);
            if(ui_ScanBandPnl3) lv_obj_clear_flag(ui_ScanBandPnl3, LV_OBJ_FLAG_HIDDEN);
        }
    }
    void fn_start_registration(lv_event_t * e) {
        isWaitingForUID = true;
        if(ui_ScanBandPnl1) lv_obj_add_flag(ui_ScanBandPnl1, LV_OBJ_FLAG_HIDDEN);
        if(ui_ScanBandPnl2) lv_obj_clear_flag(ui_ScanBandPnl2, LV_OBJ_FLAG_HIDDEN);
    }
    void fn_save_band(lv_event_t * e) {
        _ui_screen_change(&ui_Scanner, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0, &ui_Scanner_screen_init);
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
            WiFi.mode(WIFI_AP); WiFi.softAP("MagicBand-Hub", "password123"); startWebServer(); isWiFiActive = true; 
            if(ui_StatusLabel) lv_label_set_text(ui_StatusLabel, "Hotspot Active");
        } else { 
            stopWebServer(); WiFi.softAPdisconnect(true); isWiFiActive = false; 
            if(ui_StatusLabel) lv_label_set_text(ui_StatusLabel, "WiFi Disabled");
        }
    }
}

void setup() {
    Serial.begin(115200);
    prefs.begin("mbands", true);
    int rc = prefs.getInt("count", 0); bandCount = (rc < 0 || rc > 10) ? 0 : rc;
    for(int i=0; i<bandCount; i++) prefs.getBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
    prefs.end();
    
    pinMode(TOUCH_RST, OUTPUT); digitalWrite(TOUCH_RST, LOW); delay(50); digitalWrite(TOUCH_RST, HIGH);
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    I2C_NFC.begin(NFC_SDA, NFC_SCL, 100000);
    if(nfc.begin()) nfc.SAMConfig();

    strip.begin(); strip.setBrightness(40); strip.show();
    lv_init(); tft.begin(); tft.setRotation(0);
    pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH); 
    
    static lv_disp_draw_buf_t db; static lv_color_t b[240*24]; lv_disp_draw_buf_init(&db, b, NULL, 240*24);
    static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
    dd.hor_res=240; dd.ver_res=240; dd.flush_cb=my_disp_flush; dd.draw_buf=&db; lv_disp_drv_register(&dd);
    static lv_indev_drv_t id; lv_indev_drv_init(&id);
    id.type=LV_INDEV_TYPE_POINTER; id.read_cb=my_touchpad_read; lv_indev_drv_register(&id);

    const esp_timer_create_args_t ta = { .callback = [](void* arg){ lv_tick_inc(2); }, .name="t" };
    esp_timer_handle_t th; esp_timer_create(&ta, &th); esp_timer_start_periodic(th, 2000);

    ui_init(); 
    if(ui_StatusLabel) lv_label_set_text(ui_StatusLabel, "Tap below to set up WiFi");
    if(ui_NewBandConfirm) lv_obj_add_event_cb(ui_NewBandConfirm, ui_event_RegisterFromPopup, LV_EVENT_CLICKED, NULL);

    initWebServer(); 
    if (tryConnectSavedWiFi()) { isWiFiActive = true; startWebServer(); if(ui_StatusLabel) lv_label_set_text(ui_StatusLabel, "Connected"); }
}

void loop() {
    lv_timer_handler(); 
    static int lc = 0; if (bandCount != lc) { fn_refresh_roller(NULL); lc = bandCount; }
    if (isWaitingForUID) { 
        static uint32_t la = 0; 
        if (millis() - la > SPEED_MANUAL) { la = millis(); runWhiteSwirl(SPEED_MANUAL); } 
    }

    static uint32_t ls = 0;
    if (millis() - ls > 400) {
        ls = millis();
        uint8_t uid[7], len;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 50)) {
            int idx = -1;
            for (int i = 0; i < bandCount; i++) if (memcmp(uid, registeredBands[i].uid, 7) == 0) { idx = i; break; }

            if (idx != -1) {
                if(ui_StatusLabel) lv_label_set_text(ui_StatusLabel, registeredBands[idx].name);
                handleSuccess(registeredBands[idx].color); 
            } 
            else if (isWaitingForUID) {
                memcpy(registeredBands[bandCount].uid, uid, 7);
                snprintf(registeredBands[bandCount].name, 20, "MagicBand %d", bandCount + 1);
                registeredBands[bandCount].color = 0x00FF00;
                memset(registeredBands[bandCount].imageUrl, 0, 100);
                bandCount++;
                prefs.begin("mbands", false); prefs.putInt("count", bandCount);
                prefs.putBytes(("b" + String(bandCount - 1)).c_str(), &registeredBands[bandCount-1], sizeof(BandRecord));
                prefs.end();
                isWaitingForUID = false;
                if(ui_ScanBandPnl1) lv_obj_add_flag(ui_ScanBandPnl1, LV_OBJ_FLAG_HIDDEN);
                if(ui_ScanBandPnl2) lv_obj_add_flag(ui_ScanBandPnl2, LV_OBJ_FLAG_HIDDEN);
                if(ui_ScanBandPnl3) lv_obj_clear_flag(ui_ScanBandPnl3, LV_OBJ_FLAG_HIDDEN);
                handleSuccess(0x00FF00); 
            }
            else {
                handleSuccess(0x00FF00); 
                memcpy(tempRecord.uid, uid, 7); 
                if(ui_RegisterConfirmPnl) lv_obj_clear_flag(ui_RegisterConfirmPnl, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}