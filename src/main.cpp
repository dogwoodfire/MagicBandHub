#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <ui.h>           
#include <Adafruit_PN532.h>
#include <Adafruit_NeoPixel.h>
#include "CST816S.h"

// --- Hardware Pins (MagicBandHub) ---
#define NFC_SDA   15
#define NFC_SCL   16
#define NFC_IRQ   17
#define LED_PIN   21
#define LED_COUNT 12
#define TFT_BL    2 
#define TOUCH_SDA 6
#define TOUCH_SCL 7
#define TOUCH_RST 13
#define TOUCH_INT 5
#define LCD_RST   14

// --- Hardware Instances ---
TFT_eSPI tft = TFT_eSPI(240, 240); 
TwoWire I2C_NFC = TwoWire(1); 
Adafruit_PN532 nfc(NFC_IRQ, LCD_RST, &I2C_NFC); 
Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- Global Variables ---
extern "C" {
    extern lv_obj_t * ui_Scanner;
    extern lv_obj_t * ui_mickeyScanner;
}

bool isSuccessActive = false;

// --- LVGL Display Flush ---
void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t*)&color_p->full, w * h, true); 
    tft.endWrite();
    lv_disp_flush_ready(disp_drv);
}

// --- Robust Touch Polling ---
bool get_raw_touch(int16_t &x, int16_t &y) {
    Wire.beginTransmission(0x15); Wire.write(0x02);
    if (Wire.endTransmission() != 0) return false;
    Wire.requestFrom(0x15, 6);
    if (Wire.available() == 6) {
        uint8_t pts = Wire.read(); uint8_t xh = Wire.read(); uint8_t xl = Wire.read();
        uint8_t yh = Wire.read(); uint8_t yl = Wire.read(); Wire.read();
        if (pts > 0 && pts < 5) {
            x = ((xh & 0x0F) << 8) | xl; y = ((yh & 0x0F) << 8) | yl;
            return true;
        }
    }
    return false;
}

void my_touchpad_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data) {
    int16_t tx, ty;
    if (get_raw_touch(tx, ty)) {
        data->state = LV_INDEV_STATE_PR; data->point.x = tx; data->point.y = ty;
    } else { data->state = LV_INDEV_STATE_REL; }
}

// --- Unified Success Logic: One Circle -> White Fill -> Pulse ---
void handleSuccess() {
    if (isSuccessActive) return; 
    isSuccessActive = true;
    Serial.println(">>> ACCESS GRANTED <<<");

    // Phase 1: Mickey Recolor
    if (ui_mickeyScanner != NULL) {
        lv_obj_set_style_img_recolor(ui_mickeyScanner, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_img_recolor_opa(ui_mickeyScanner, 255, 0);
        // Force LVGL update so color appears before LED loop takes over
        for(int i=0; i<10; i++) { lv_timer_handler(); delay(5); }
    }

    const int FRAME_DELAY = 25; // Unified timing for Phase 2 and 3

    // Phase 2: Single White Spin Circle (12 frames to complete one lap)
    for (int frame = 0; frame < LED_COUNT; frame++) {
        strip.clear();
        for (int i = 0; i < 5; i++) { // Trailing tail
            int pixel = (frame - i + LED_COUNT) % LED_COUNT;
            int brightness = 255 - (i * 50);
            if (brightness < 0) brightness = 0;
            strip.setPixelColor(pixel, strip.Color(brightness, brightness, brightness));
        }
        strip.show();
        delay(FRAME_DELAY); 
        lv_timer_handler(); // Keep UI alive
    }

    // Phase 3: Rapid White Expansion (Starts immediately at same speed)
    // We don't strip.clear() here so the spin tail "fills in" to solid white
    for (int i = 0; i < LED_COUNT; i++) {
        strip.setPixelColor(i, strip.Color(255, 255, 255)); 
        strip.show();
        delay(FRAME_DELAY); 
        lv_timer_handler();
    }

    // Phase 4: Confirmation Pulse (Green) - Higher speed for "Pop" effect
    for (int pulse = 0; pulse < 2; pulse++) {
        for (int b = 50; b <= 255; b += 15) { // Fade Up
            strip.fill(strip.Color(0, b, 0)); strip.show();
            delay(10); lv_timer_handler();
        }
        for (int b = 255; b >= 50; b -= 15) { // Fade Down
            strip.fill(strip.Color(0, b, 0)); strip.show();
            delay(10); lv_timer_handler();
        }
    }

    // Cleanup
    if (ui_mickeyScanner != NULL) lv_obj_set_style_img_recolor_opa(ui_mickeyScanner, 0, 0);
    strip.clear(); strip.show();
    isSuccessActive = false;
}

void setup() {
    Serial.begin(115200);
    delay(2000);

    // Hardware Reset for Touch
    pinMode(TOUCH_RST, OUTPUT); digitalWrite(TOUCH_RST, LOW); delay(50); digitalWrite(TOUCH_RST, HIGH); delay(100);

    // Init I2C Buses
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    I2C_NFC.begin(NFC_SDA, NFC_SCL, 100000);
    if (nfc.begin()) { nfc.SAMConfig(); }

    // Init LEDs
    strip.begin(); strip.setBrightness(40); strip.show();

    // Init LVGL & Display
    lv_init(); tft.begin(); tft.setRotation(0);
    pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH); 

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf[240 * 24]; 
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, 240 * 24);

    static lv_disp_drv_t disp_drv; lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 240; disp_drv.ver_res = 240;
    disp_drv.flush_cb = my_disp_flush; disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv; lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER; indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    const esp_timer_create_args_t tick_args = { .callback = [](void* arg){ lv_tick_inc(2); }, .name = "lvgl_tick" };
    esp_timer_handle_t tick_timer = NULL; esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 2000);

    ui_init();

    if (ui_mickeyScanner != NULL) {
        lv_obj_add_flag(ui_mickeyScanner, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ui_mickeyScanner, [](lv_event_t * e){ handleSuccess(); }, LV_EVENT_CLICKED, NULL);
    }
}

void loop() {
    lv_timer_handler(); 

    static uint32_t lastScan = 0;
    if (millis() - lastScan > 300) {
        lastScan = millis();
        uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 }; uint8_t uidLen;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50)) {
            handleSuccess();
        }
    }
}