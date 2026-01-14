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

// Helper: Converts web hex (#RRGGBB) to uint32
uint32_t hexToUint(String hex) {
    if(hex.startsWith("#")) hex = hex.substring(1);
    return (uint32_t) strtol(hex.c_str(), NULL, 16);
}

extern "C" {

void initWebServer() {
    // 1. DASHBOARD HANDLER
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:sans-serif; text-align:center; background:#f4f4f9; padding:20px;} ";
        html += ".card{background:white; border-radius:15px; padding:15px; margin:10px auto; max-width:350px; shadow: 2px 2px 10px #ccc;} ";
        html += "input{margin:5px; padding:8px; border-radius:5px; border:1px solid #ccc; width:80%;} ";
        html += ".btn-search{background:#5765f2; color:white; border:none; padding:8px; border-radius:5px; cursor:pointer; width:80%; margin-bottom:10px;} "; // Added search style
        html += ".btn-save{background:#2ed573; color:white; border:none; padding:12px; width:80%; border-radius:8px; cursor:pointer;} ";
        html += ".btn-del{background:#ff4757; color:white; border:none; padding:5px 10px; border-radius:5px; margin-top:15px; font-size:0.8em;}</style></head><body>";

        if(WiFi.status() != WL_CONNECTED) {
            html += "<h1>Hub Setup</h1><div class='card'><h3>Connect to Home WiFi</h3>";
            html += "<form action='/setwifi' method='GET'>";
            html += "SSID: <input type='text' name='ssid' placeholder='WiFi Name'><br>";
            html += "Pass: <input type='password' name='pass' placeholder='Password'><br>";
            html += "<input type='submit' value='Connect' style='background:#007bff; color:white; border:none; padding:10px; width:80%;'></form></div>";
        } else {
            html += "<h1>Band Manager</h1><p>Home WiFi: " + WiFi.SSID() + "</p>";
            if(bandCount == 0) html += "<p>No bands saved. Scan one on the hub first!</p>";
            
            for(int i=0; i < bandCount; i++) {
                char hStr[8]; sprintf(hStr, "#%06X", (unsigned int)registeredBands[i].color);
                html += "<div class='card'><h3>" + String(registeredBands[i].name) + "</h3>";
                if(strlen(registeredBands[i].imageUrl) > 5) html += "<img src='" + String(registeredBands[i].imageUrl) + "' style='width:100px; border-radius:8px;'><br>";
                
                html += "<form action='/update' method='GET'><input type='hidden' name='id' value='" + String(i) + "'>";
                // Added unique ID for the name input
                html += "Name: <input type='text' name='name' id='n"+String(i)+"' value='" + String(registeredBands[i].name) + "' maxlength='19'><br>";
                
                // RESTORED: Search button logic with your requested URL format
                html += "<button type='button' class='btn-search' onclick=\"window.open('https://www.magicbandcollectors.com/checklist/?search=' + encodeURIComponent(document.getElementById('n"+String(i)+"').value))\">Find Image Online</button><br>";
                
                html += "Img URL: <input type='text' name='img' value='" + String(registeredBands[i].imageUrl) + "' placeholder='Paste .jpg link'><br>";
                html += "Color: <input type='color' name='color' value='" + String(hStr) + "' style='width:40px;'><br>";
                html += "<input type='submit' value='Save Changes' class='btn-save'></form>";
                html += "<form action='/delete' method='GET'><input type='hidden' name='id' value='" + String(i) + "'><input type='submit' value='Delete' class='btn-del'></form></div>";
            }
        }
        html += "</body></html>";
        request->send(200, "text/html", html);
    });

    // 2. SET WIFI
    server.on("/setwifi", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("ssid") && request->hasParam("pass")){
            prefs.begin("wifi", false);
            prefs.putString("ssid", request->getParam("ssid")->value());
            prefs.putString("pass", request->getParam("pass")->value());
            prefs.end();
            request->send(200, "text/html", "Hub restarting... Reconnect your phone to home WiFi.");
            delay(2000); ESP.restart();
        }
    });

    // 3. UPDATE/RECOLOR
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
            }
        }
        request->redirect("/");
    });

    // 4. DELETE
    server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("id")){
            int id = request->getParam("id")->value().toInt();
            if(id < bandCount) {
                prefs.begin("mbands", false);
                for(int i = id; i < bandCount - 1; i++) {
                    registeredBands[i] = registeredBands[i+1];
                    prefs.putBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
                }
                bandCount--; prefs.putInt("count", bandCount); prefs.end();
            }
        }
        request->redirect("/");
    });
}

void startWebServer() { 
    server.begin(); 
    if(MDNS.begin("magicband")) {
        Serial.println("CONSOLE: Responder active at http://magicband.local");
    }
}

void stopWebServer() { server.end(); }

bool tryConnectSavedWiFi() {
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();
    if(ssid == "") return false;

    WiFi.begin(ssid.c_str(), pass.c_str());
    int attempt = 0;
    while (WiFi.status() != WL_CONNECTED && attempt < 30) {
        delay(500); 
        Serial.print("."); 
        attempt++;
        lv_timer_handler(); 
    }

    if(WiFi.status() == WL_CONNECTED) {
        Serial.print("\nSUCCESS: Connected to ");
        Serial.println(WiFi.localIP());
        if(ui_StatusLabel) lv_label_set_text(ui_StatusLabel, "Connected to Home WiFi");
        return true;
    }
    return false;
}

}