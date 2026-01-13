#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <ui.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_PN532.h>

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
#define LCD_RST   14

// --- Global Objects ---
TFT_eSPI tft = TFT_eSPI(240, 240);
Adafruit_NeoPixel pixels(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
TwoWire WireNFC = TwoWire(1); 
Adafruit_PN532 nfc(NFC_IRQ, LCD_RST, &WireNFC); 

// Correct external variable for the MagicBandHub project
extern "C" {
    extern lv_obj_t * ui_mickeyScanner;
}

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[240 * 24];

void example_increase_lvgl_tick(void *arg) { lv_tick_inc(2); }
void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1); uint32_t h = (area->y2 - area->y1 + 1);
    tft.startWrite(); tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t*)&color_p->full, w * h, true); tft.endWrite();
    lv_disp_flush_ready(disp_drv);
}

// Robust Touch Polling
bool get_raw_touch(int16_t &x, int16_t &y) {
    Wire.beginTransmission(0x15); Wire.write(0x02);
    if (Wire.endTransmission() != 0) return false;
    Wire.requestFrom(0x15, 6);
    if (Wire.available() == 6) {
        uint8_t pts = Wire.read(); uint8_t xh = Wire.read(); uint8_t xl = Wire.read();
        uint8_t yh = Wire.read(); uint8_t yl = Wire.read();
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

// Function to trigger the visual success effect
void trigger_success_effect() {
    if (ui_mickeyScanner != NULL) {
        // Turn Mickey Green
        lv_obj_set_style_img_recolor(ui_mickeyScanner, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_img_recolor_opa(ui_mickeyScanner, 255, 0);
        
        // LED Green Chase
        for(int i=0; i<LED_COUNT; i++) {
            pixels.setPixelColor(i, pixels.Color(0, 255, 0));
            pixels.show();
            delay(15);
        }
        delay(500);
        
        // Reset Visuals
        lv_obj_set_style_img_recolor_opa(ui_mickeyScanner, 0, 0);
        pixels.clear(); pixels.show();
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    pixels.begin(); pixels.setBrightness(50);
    for(int i=0; i<LED_COUNT; i++) pixels.setPixelColor(i, pixels.Color(0, 0, 100));
    pixels.show();

    pinMode(TOUCH_RST, OUTPUT); digitalWrite(TOUCH_RST, LOW); delay(50); digitalWrite(TOUCH_RST, HIGH);
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    WireNFC.begin(NFC_SDA, NFC_SCL);
    nfc.begin();
    if (nfc.getFirmwareVersion()) { nfc.SAMConfig(); }

    tft.begin(); tft.setRotation(0);
    pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, 240 * 24);
    static lv_disp_drv_t disp_drv; lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 240; disp_drv.ver_res = 240;
    disp_drv.flush_cb = my_disp_flush; disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    static lv_indev_drv_t indev_drv; lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER; indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    const esp_timer_create_args_t tick_args = { .callback = &example_increase_lvgl_tick, .name = "lv_tick" };
    esp_timer_handle_t tick_timer = NULL;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 2000);

    ui_init(); // Initializes ui_mickeyScanner

    // Enable touch interaction for the Mickey Scanner object
    if (ui_mickeyScanner != NULL) {
        lv_obj_add_flag(ui_mickeyScanner, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ui_mickeyScanner, [](lv_event_t * e){
            Serial.println("TOUCH: Mickey Scanner Tapped!");
            trigger_success_effect();
        }, LV_EVENT_CLICKED, NULL);
    }
    
    pixels.clear(); pixels.show();
}

void loop() {
    lv_timer_handler();

    static uint32_t lastNFC = 0;
    if (millis() - lastNFC > 300) {
        lastNFC = millis();
        uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
        uint8_t uidLength;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50)) {
            Serial.println("NFC: MagicBand Found!");
            trigger_success_effect();
        }
    }
}