#include "web_portal.h"
#include <WiFi.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <lvgl.h>
#include "ui/ui.h"

extern BandRecord registeredBands[10];
extern int bandCount;
extern AsyncWebServer server;
extern Preferences prefs;

extern "C" void fn_refresh_roller(lv_event_t * e);

uint32_t hexToUint(String hex) {
    if(hex.startsWith("#")) hex = hex.substring(1);
    return (uint32_t) strtol(hex.c_str(), NULL, 16);
}

void initWebServer() {
    // 1. DASHBOARD HANDLER
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:sans-serif; text-align:center; background:#f4f4f9; padding:20px;} ";
        html += ".card{background:white; border-radius:15px; padding:15px; margin:10px auto; max-width:350px; shadow: 2px 2px 10px #ccc;} ";
        html += "input{margin:5px; padding:8px; border-radius:5px; border:1px solid #ccc; width:80%;} ";
        html += ".btn-search{background:#5765f2; color:white; border:none; padding:8px; border-radius:5px; cursor:pointer; width:80%; margin-bottom:10px;} ";
        html += ".btn-save{background:#2ed573; color:white; border:none; padding:12px; width:80%; border-radius:8px; cursor:pointer;} ";
        html += ".btn-del{background:#ff4757; color:white; border:none; padding:5px 10px; border-radius:5px; margin-top:15px; font-size:0.8em;}</style></head><body>";

        if(WiFi.status() != WL_CONNECTED) {
            html += "<h1>Hub Setup</h1><div class='card'><h3>Connect to Home WiFi</h3>";
            html += "<form action='/setwifi' method='GET'>";
            html += "SSID: <input type='text' name='ssid' placeholder='WiFi Name' required><br>";
            html += "Pass: <input type='password' name='pass' placeholder='Password'><br><br>";
            html += "<input type='submit' class='btn-save' value='Save & Connect Hub'></form></div>";
        } else {
            html += "<h1>MagicBand Manager</h1><p>Connected to: " + WiFi.SSID() + "</p>";
            if(bandCount == 0) html += "<p>No bands saved. Scan one on the hub!</p>";
            for(int i=0; i < bandCount; i++) {
                char hStr[8]; sprintf(hStr, "#%06X", (unsigned int)registeredBands[i].color);
                html += "<div class='card'><h3>" + String(registeredBands[i].name) + "</h3>";
                if(strlen(registeredBands[i].imageUrl) > 5) {
                    html += "<img src='" + String(registeredBands[i].imageUrl) + "' style='width:100px; border-radius:8px;'><br>";
                }
                html += "<form action='/update' method='GET'><input type='hidden' name='id' value='" + String(i) + "'>";
                html += "Name: <input type='text' name='name' id='n"+String(i)+"' value='" + String(registeredBands[i].name) + "' maxlength='19'><br>";
                html += "<button type='button' class='btn-search' onclick=\"window.open('https://www.magicbandcollectors.com/checklist/?search=' + encodeURIComponent(document.getElementById('n"+String(i)+"').value))\">Find Image Online</button><br>";
                html += "Img URL: <input type='text' name='img' value='" + String(registeredBands[i].imageUrl) + "' placeholder='Paste .jpg link'><br>";
                html += "Color: <input type='color' name='color' value='" + String(hStr) + "' style='width:40px;'><br>";
                html += "<input type='submit' value='Save Changes' class='btn-save'></form>";
                html += "<form action='/delete' method='GET' onsubmit='return confirm(\"Delete Band?\")'><input type='hidden' name='id' value='" + String(i) + "'><input type='submit' value='Delete Band' class='btn-del'></form></div>";
            }
        }
        html += "</body></html>";
        request->send(200, "text/html", html);
    });

    // 2. UPDATE HANDLER
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("id")){
            int id = request->getParam("id")->value().toInt();
            if(id < bandCount) {
                if(request->hasParam("name")) strncpy(registeredBands[id].name, request->getParam("name")->value().c_str(), 19);
                if(request->hasParam("color")) registeredBands[id].color = hexToUint(request->getParam("color")->value());
                if(request->hasParam("img")) strncpy(registeredBands[id].imageUrl, request->getParam("img")->value().c_str(), 99);
                prefs.begin("mbands", false);
                prefs.putBytes(("b" + String(id)).c_str(), &registeredBands[id], sizeof(BandRecord));
                prefs.end();
                fn_refresh_roller(NULL); 
            }
        }
        request->redirect("/");
    });

    // 3. DELETE HANDLER (CLEARS ALL URLS AND GHOST DATA)
    server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("id")){
            int id = request->getParam("id")->value().toInt();
            if(id < bandCount) {
                for(int i = id; i < bandCount - 1; i++) registeredBands[i] = registeredBands[i+1];
                bandCount--;
                prefs.begin("mbands", false);
                prefs.clear(); // Wipes memory to remove associated IMG urls
                prefs.putInt("count", bandCount);
                for(int i=0; i<bandCount; i++) prefs.putBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
                prefs.end();
                fn_refresh_roller(NULL);
            }
        }
        request->redirect("/");
    });

    // 4. SET WIFI HANDLER
    server.on("/setwifi", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("ssid")){
            prefs.begin("wifi", false);
            prefs.putString("ssid", request->getParam("ssid")->value());
            prefs.putString("pass", request->hasParam("pass") ? request->getParam("pass")->value() : "");
            prefs.end();
            request->send(200, "text/html", "WiFi details saved. Hub is restarting...");
            delay(2000); ESP.restart();
        }
    });
}

bool tryConnectSavedWiFi() {
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();
    if(ssid == "") return false;
    WiFi.begin(ssid.c_str(), pass.c_str());
    int attempt = 0;
    while (WiFi.status() != WL_CONNECTED && attempt < 30) { delay(500); attempt++; lv_timer_handler(); }
    return (WiFi.status() == WL_CONNECTED);
}

extern "C" {
    void startWebServer() { server.begin(); MDNS.begin("magicband"); }
    void stopWebServer() { server.end(); }
}