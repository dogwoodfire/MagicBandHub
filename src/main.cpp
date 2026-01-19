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
#include <time.h>
#include "esp_sntp.h"
#include <HTTPClient.h>

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

uint32_t lastActivityTime = 0;
uint32_t idleTimeout = 60000;   // Default 1 min
uint32_t sleepTimeout = 300000; // Default 5 min
bool isScreenOn = true;

BandRecord registeredBands[50];
int bandCount = 0;
char ownersList[10][20]; 
char locationsList[10][20];

// Make the C-linkage function visible to other files
extern "C" void fn_refresh_roller(lv_event_t * e);

// Reload ownersList/locationsList from NVS (safe for missing keys)
void loadCategoriesFromPrefs() {
    prefs.begin("mbands", true);

    for (int i = 0; i < 10; i++) {
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

extern "C" {
    extern lv_obj_t * ui_Scanner, * ui_mickeyScanner, * ui_ScanBandPnl3, * ui_BandRoller, * ui_RegisterConfirmPnl, * ui_NewBandConfirm, * ui_StatusLabel, * ui_EditNameLabel, * ui_StandbyScreen, * ui_clock, * ui_Hotspot;
}

void wakeScreen() {
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
    
    lastActivityTime = millis();
}

// Display/Touch Drivers
void my_disp_flush(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* c) {
    uint32_t w=(a->x2-a->x1+1), h=(a->y2-a->y1+1);
    tft.startWrite(); tft.setAddrWindow(a->x1,a->y1,w,h);
    tft.pushColors((uint16_t*)&c->full,w*h,true); tft.endWrite();
    lv_disp_flush_ready(d);
}

bool get_raw_touch(int16_t &x, int16_t &y) {
    static uint32_t lastFail = 0;
    Wire.beginTransmission(0x15); 
    Wire.write(0x02); // Point to data register
    
    // If the device doesn't respond to the register write, abort immediately
    if(Wire.endTransmission() != 0) {
        return false; 
    }

    // Add a tiny delay (10-50 microseconds) to let the controller breathe
    delayMicroseconds(50); 

    // Attempt the read
    uint8_t bytesReceived = Wire.requestFrom(0x15, 6, true);
    if (bytesReceived != 6 || Wire.available() != 6) {
        Wire.flush(); // ESP32-specific

        // If we just failed very recently, don't hammer the bus
        uint32_t now = millis();
        if (now - lastFail < 200) return false;
        lastFail = now;

        // Attempt a quick bus recovery + re-init of the driver
        pinMode(TOUCH_SCL, OUTPUT);
        for (int i = 0; i < 9; i++) {
            digitalWrite(TOUCH_SCL, HIGH);
            delayMicroseconds(5);
            digitalWrite(TOUCH_SCL, LOW);
            delayMicroseconds(5);
        }
        pinMode(TOUCH_SCL, INPUT_PULLUP);
        delayMicroseconds(50);

        Wire.end();
        Wire.begin(TOUCH_SDA, TOUCH_SCL);
        return false;
    }
    
    if(bytesReceived == 6 && Wire.available() == 6) {
        uint8_t p = Wire.read(); 
        uint8_t xh = Wire.read(); 
        uint8_t xl = Wire.read(); 
        uint8_t yh = Wire.read(); 
        uint8_t yl = Wire.read(); 
        Wire.read(); // Skip checksum/extra byte

        if(p > 0 && p < 5) { 
            x = ((xh & 0x0F) << 8) | xl; 
            y = ((yh & 0x0F) << 8) | yl; 
            return true; 
        }
    }
    return false;
}



void my_touchpad_read(lv_indev_drv_t* d, lv_indev_data_t* data) {
    int16_t tx, ty;
    if(get_raw_touch(tx, ty)){ 
        data->state=LV_INDEV_STATE_PR; data->point.x=tx; data->point.y=ty; 
        wakeScreen(); // ADD THIS LINE
    }
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

    if(ui_ScanBandPnl3) lv_obj_add_flag(ui_ScanBandPnl3, LV_OBJ_FLAG_HIDDEN);
}

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


void autoSetTimezone() {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    // Query timezone info based on public IP
    http.begin("http://worldtimeapi.org/api/ip");
    int httpCode = http.GET();

    if (httpCode == 200) {
        String payload = http.getString();
        // Extract the offset (e.g., "+01:00" or "-05:00")
        int offsetIdx = payload.indexOf("\"utc_offset\":\"") + 14;
        String offsetStr = payload.substring(offsetIdx, offsetIdx + 6);
        
        // Convert "+HH:MM" to total seconds
        int hours = offsetStr.substring(1, 3).toInt();
        int mins = offsetStr.substring(4, 6).toInt();
        long totalOffset = (hours * 3600) + (mins * 60);
        if (offsetStr.startsWith("-")) totalOffset = -totalOffset;

        // Apply the detected offset (NTP sync)
        configTime(totalOffset, 0, "pool.ntp.org", "time.nist.gov");
        Serial.printf("Timezone Auto-Set: %s (Offset: %ld sec)\n", offsetStr.c_str(), totalOffset);
    } else {
        Serial.println("Timezone API failed, using default UTC.");
        configTime(0, 0, "pool.ntp.org");
    }
    http.end();
}

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
                
                strncpy(registeredBands[bandCount].name, candidateName, 19);
                // ------------------------------------

                // Hardware Detection for the new record
                String hStr = ""; 
                for(int i=0; i<7; i++) { if(tempRecord.uid[i]<0x10) hStr+="0"; hStr+=String(tempRecord.uid[i],HEX); }
                hStr.toUpperCase();
                String style = hStr.endsWith("90") ? "MagicBand+" : (hStr.endsWith("80") ? "MagicBand 2.0" : "Other");
                strncpy(registeredBands[bandCount].type, style.c_str(), 19);

                registeredBands[bandCount].color = 0x00FF00;
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
            WiFi.mode(WIFI_AP); WiFi.softAP("MagicBand-Hub", "password123"); startWebServer(); isWiFiActive = true; 
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
}



void setup() {
    setCpuFrequencyMhz(240);
    Serial.begin(115200);
    prefs.begin("mbands", true);
    int rc = prefs.getInt("count", 0); bandCount = (rc < 0 || rc > 50) ? 0 : rc;
    for(int i=0; i<bandCount; i++) prefs.getBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
    
    prefs.end();
    // Load category lists
    loadCategoriesFromPrefs();
    
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

    prefs.begin("settings", true);
    idleTimeout = prefs.getUInt("idle", 60000);
    sleepTimeout = prefs.getUInt("sleep", 300000);
    prefs.end();

    ui_init(); 
    if(ui_StatusLabel) lv_label_set_text(ui_StatusLabel, "Tap below to set up WiFi");
    if(ui_NewBandConfirm) lv_obj_add_event_cb(ui_NewBandConfirm, ui_event_RegisterFromPopup, LV_EVENT_CLICKED, NULL);

    initWebServer(); 
    if (tryConnectSavedWiFi()) { 
        isWiFiActive = true; 
        startWebServer(); 
        if(ui_StatusLabel) {
            String msg = "Connected to: " + WiFi.SSID();
            lv_label_set_text(ui_StatusLabel, msg.c_str());
        }
        autoSetTimezone(); 
    } else {
        if(ui_StatusLabel) lv_label_set_text(ui_StatusLabel, "Tap Bellow to Set Up Wifi");
    }
}

void loop() {
    lv_timer_handler(); 
    handleStandby();
    updateClock();

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
            wakeScreen();
            lastActivityTime = millis(); // Reset timer on every scan
            if(!isScreenOn) {
                digitalWrite(TFT_BL, HIGH);
                isScreenOn = true;
            }
            String hexUID = "";
            for (uint8_t i = 0; i < len; i++) { if (uid[i] < 0x10) hexUID += "0"; hexUID += String(uid[i], HEX); if (i < len - 1) hexUID += ":"; }
            hexUID.toUpperCase();

            String style = hexUID.endsWith("90") ? "MagicBand+" : (hexUID.endsWith("80") ? "MagicBand 2.0" : "MagicBand 1.0 / Other");
            Serial.printf("\n--- NFC SCAN: %s ---\nHARDWARE: %s\n", hexUID.c_str(), style.c_str());

            int idx = -1;
            for (int i = 0; i < bandCount; i++) if (memcmp(uid, registeredBands[i].uid, 7) == 0) { idx = i; break; }

            if (idx != -1) {
                // Only update label if we are on the main scanner screen
                handleSuccess(registeredBands[idx].color); 
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
                strncpy(registeredBands[bandCount].name, candidateName, 19);
                // ------------------------------------

                strncpy(registeredBands[bandCount].type, style.c_str(), 19);
                registeredBands[bandCount].color = 0x00FF00;
                memset(registeredBands[bandCount].imageUrl, 0, 100);
                bandCount++;
                prefs.begin("mbands", false); prefs.putInt("count", bandCount);
                prefs.putBytes(("b" + String(bandCount - 1)).c_str(), &registeredBands[bandCount-1], sizeof(BandRecord));
                prefs.end();
                isWaitingForUID = false;
                
                // Show Success Panel child of Scanner screen
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