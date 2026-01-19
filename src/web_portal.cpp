#include "web_portal.h"
#include <WiFi.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <lvgl.h>
#include "ui/ui.h"

extern BandRecord registeredBands[50];
extern int bandCount;
extern char ownersList[10][20];
extern char locationsList[10][20];
extern AsyncWebServer server;
extern Preferences prefs;
// NEW: Access the timeout variables from main.cpp
extern uint32_t idleTimeout;
extern uint32_t sleepTimeout;

extern "C" void fn_refresh_roller(lv_event_t * e);

uint32_t hexToUint(String hex) {
    if(hex.startsWith("#")) hex = hex.substring(1);
    return (uint32_t) strtol(hex.c_str(), NULL, 16);
}

void initWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:sans-serif; text-align:center; background:#f4f4f9; padding:20px;} ";
        html += ".card{background:white; border-radius:15px; padding:15px; margin:10px auto; max-width:350px; box-shadow: 2px 2px 10px #ccc;} ";
        html += "input, select{margin:5px; padding:8px; border-radius:5px; border:1px solid #ccc; width:80%;} ";
        html += ".btn-search{background:#5765f2; color:white; border:none; padding:8px; border-radius:5px; cursor:pointer; width:80%; margin-bottom:10px;} ";
        html += ".btn-save{background:#2ed573; color:white; border:none; padding:12px; width:80%; border-radius:8px; cursor:pointer;} ";
        html += ".btn-del{background:#ff4757; color:white; border:none; padding:5px 10px; border-radius:5px; margin-top:15px; font-size:0.8em;}</style>";
        
        // SEARCH FILTER SCRIPT
        html += "<script>function filterBands() { var val = document.getElementById('search').value.toLowerCase();";
        html += "var cards = document.getElementsByClassName('band-card');";
        html += "for (var i=0; i<cards.length; i++) { var txt = cards[i].innerText.toLowerCase();";
        html += "cards[i].style.display = txt.includes(val) ? '' : 'none'; }}</script></head><body>";

        if(request->hasParam("msg")) {
            String msg = request->getParam("msg")->value();
            if(msg == "category_updated") {
                html += "<div class='card' style='background:#d4edda;color:#155724;'>Category updated successfully.</div>";
            }
        }

        if(WiFi.status() != WL_CONNECTED) {
            html += "<h1>Hub Setup</h1><div class='card'><h3>Connect WiFi</h3><form action='/setwifi' method='GET'>";
            html += "SSID: <input type='text' name='ssid' required><br>Pass: <input type='password' name='pass'><br>";
            html += "<input type='submit' class='btn-save' value='Save & Restart'></form></div>";
        } else {
            html += "<h1>MagicBand Hub</h1>";
            
            // SEARCH BOX
            html += "<input type='text' id='search' onkeyup='filterBands()' placeholder='Filter by Owner, Location, or Name...' style='width:90%; padding:15px; margin-bottom:20px;'>";

            // OWNERS CARD
            html += "<div class='card'><h3>Owners</h3>";
            for(int i=0; i<10; i++) {
                if(ownersList[i][0] != '\0') {
                    int usedCount = 0;
                    for(int b=0; b<bandCount; b++) {
                        if(strcmp(registeredBands[b].owner, ownersList[i]) == 0) usedCount++;
                    }
                    String warn = usedCount > 0 ? 
                        "This owner is used by " + String(usedCount) + " band(s). They will be set to None. Continue?" :
                        "Delete owner?";
                    html += String(ownersList[i]) +
                        " <a href='/delcat?type=o&id=" + String(i) +
                        "' class='btn-del' onclick='return confirm(\"" + warn + "\")'>Delete</a><br>";
                }
            }
            html += "<hr><form action='/addcat' method='GET'>";
            html += "<input type='hidden' name='type' value='o'>";
            html += "<input type='text' name='val' placeholder='New owner name' required><br>";
            html += "<input type='submit' class='btn-save' value='Add Owner'></form></div>";

            // LOCATIONS CARD
            html += "<div class='card'><h3>Locations</h3>";
            for(int i=0; i<10; i++) {
                if(locationsList[i][0] != '\0') {
                    int usedCount = 0;
                    for(int b=0; b<bandCount; b++) {
                        if(strcmp(registeredBands[b].location, locationsList[i]) == 0) usedCount++;
                    }
                    String warn = usedCount > 0 ?
                        "This location is used by " + String(usedCount) + " band(s). They will be set to None. Continue?" :
                        "Delete location?";
                    html += String(locationsList[i]) +
                        " <a href='/delcat?type=l&id=" + String(i) +
                        "' class='btn-del' onclick='return confirm(\"" + warn + "\")'>Delete</a><br>";
                }
            }
            html += "<hr><form action='/addcat' method='GET'>";
            html += "<input type='hidden' name='type' value='l'>";
            html += "<input type='text' name='val' placeholder='New location name' required><br>";
            html += "<input type='submit' class='btn-save' value='Add Location'></form></div>";

            // HUB SETTINGS
            html += "<div class='card'><h3>Hub Settings</h3><form action='/settings' method='GET'>";
            html += "Standby (Min): <input type='number' name='idle' value='" + String(idleTimeout / 60000) + "' min='1'><br>";
            html += "Sleep (Min): <input type='number' name='sleep' value='" + String(sleepTimeout / 60000) + "' min='1'><br>";
            html += "<input type='submit' class='btn-save' value='Save Settings'></form></div>";

            // BAND CARDS
            for(int i=0; i < bandCount; i++) {
                char hStr[8]; sprintf(hStr, "#%06X", (unsigned int)registeredBands[i].color);
                html += "<div class='card band-card'><h3>" + String(registeredBands[i].name) + "</h3>";
                html += "<p style='color:#666; font-size:0.8em; margin-top:-10px;'>Type: " + String(registeredBands[i].type) + "</p>";

                if(strlen(registeredBands[i].imageUrl) > 5) html += "<img src='" + String(registeredBands[i].imageUrl) + "' style='width:100px; border-radius:8px;'><br>";
                
                html += "<form action='/update' method='GET'><input type='hidden' name='id' value='" + String(i) + "'>";
                html += "Name: <input type='text' name='name' id='n"+String(i)+"' value='" + String(registeredBands[i].name) + "' maxlength='19'><br>";
                html += "Bought: <input type='date' name='date' value='" + String(registeredBands[i].dateBought) + "'><br>";
                
                // OWNER DROPDOWN
                html += "Owner: <select name='owner'><option value=''>None</option>";
                for(int j=0; j<10; j++) {
                    if(ownersList[j][0] != '\0') { // Check if slot is not empty
                        String sel = (String(registeredBands[i].owner) == String(ownersList[j])) ? "selected" : "";
                        html += "<option value='" + String(ownersList[j]) + "' " + sel + ">" + String(ownersList[j]) + "</option>";
                    }
                }
                html += "</select><br>";

                // LOCATION DROPDOWN
                html += "Location: <select name='loc'><option value=''>None</option>";
                for(int k=0; k<10; k++) {
                    if(locationsList[k][0] != '\0') { // Check if slot is not empty
                        String sel = (String(registeredBands[i].location) == String(locationsList[k])) ? "selected" : "";
                        html += "<option value='" + String(locationsList[k]) + "' " + sel + ">" + String(locationsList[k]) + "</option>";
                    }
                }
                html += "</select><br>";

                html += "Img URL: <input type='text' name='img' value='" + String(registeredBands[i].imageUrl) + "' placeholder='jpg link'><br>";
                html += "Color: <input type='color' name='color' value='" + String(hStr) + "' style='width:40px;'><br>";
                html += "<input type='submit' value='Save Changes' class='btn-save'></form>";
                html += "<form action='/delete' method='GET' onsubmit='return confirm(\"Delete?\")'><input type='hidden' name='id' value='" + String(i) + "'><input type='submit' value='Delete Band' class='btn-del'></form></div>";
            }
        }
        html += "</body></html>";
        request->send(200, "text/html", html);
    });

    server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("idle")) idleTimeout = request->getParam("idle")->value().toInt() * 60000;
        if(request->hasParam("sleep")) sleepTimeout = request->getParam("sleep")->value().toInt() * 60000;
        
        prefs.begin("settings", false);
        prefs.putUInt("idle", idleTimeout);
        prefs.putUInt("sleep", sleepTimeout);
        prefs.end();
        
        request->redirect("/");
    });

    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("id")){
            int id = request->getParam("id")->value().toInt();
            if(id < bandCount) {
                if(request->hasParam("name")) strncpy(registeredBands[id].name, request->getParam("name")->value().c_str(), 19);
                if(request->hasParam("date")) strncpy(registeredBands[id].dateBought, request->getParam("date")->value().c_str(), 11);
                if(request->hasParam("owner")) strncpy(registeredBands[id].owner, request->getParam("owner")->value().c_str(), 19);
                if(request->hasParam("loc")) strncpy(registeredBands[id].location, request->getParam("loc")->value().c_str(), 19);
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

    server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("id")){
            int id = request->getParam("id")->value().toInt();
            if(id < bandCount) {
                for(int i = id; i < bandCount - 1; i++) registeredBands[i] = registeredBands[i+1];
                bandCount--;
                prefs.begin("mbands", false);
                prefs.clear(); 
                prefs.putInt("count", bandCount);
                for(int i=0; i<bandCount; i++) prefs.putBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
                prefs.end();
                fn_refresh_roller(NULL);
            }
        }
        request->redirect("/");
    });

    server.on("/setwifi", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("ssid")){
            prefs.begin("wifi", false);
            prefs.putString("ssid", request->getParam("ssid")->value());
            prefs.putString("pass", request->hasParam("pass") ? request->getParam("pass")->value() : "");
            prefs.end();
            request->send(200, "text/html", "Restarting...");
            delay(2000); ESP.restart();
        }
    });

    // ROUTE: Delete Category
    server.on("/delcat", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("type") && request->hasParam("id")){
            String type = request->getParam("type")->value();
            int id = request->getParam("id")->value().toInt();

            // Capture label before removing
            String deletedLabel = "";
            if(type == "o" && id >= 0 && id < 10) deletedLabel = String(ownersList[id]);
            if(type == "l" && id >= 0 && id < 10) deletedLabel = String(locationsList[id]);

            prefs.begin("mbands", false);
            prefs.remove((type + String(id)).c_str());
            prefs.end();

            // Clear category from bands in RAM
            if(deletedLabel.length() > 0) {
                if(type == "o") {
                    for(int b=0; b<bandCount; b++) {
                        if(strcmp(registeredBands[b].owner, deletedLabel.c_str()) == 0) {
                            registeredBands[b].owner[0] = '\0';
                        }
                    }
                } else if(type == "l") {
                    for(int b=0; b<bandCount; b++) {
                        if(strcmp(registeredBands[b].location, deletedLabel.c_str()) == 0) {
                            registeredBands[b].location[0] = '\0';
                        }
                    }
                }
            }

            loadCategoriesFromPrefs();
            fn_refresh_roller(NULL);
            request->redirect("/?msg=category_updated");
        }
    });

    // ROUTE: Add Category (Improved to prevent duplicates)
    server.on("/addcat", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("val") && request->hasParam("type")){
            String val = request->getParam("val")->value();
            String type = request->getParam("type")->value();
            
            prefs.begin("mbands", false);
            bool alreadyExists = false;
            int emptySlot = -1;

            for(int i=0; i<10; i++) {
                String key = type + String(i);

                // If key doesn't exist, treat as empty without calling getString() (prevents NOT_FOUND spam)
                if(!prefs.isKey(key.c_str())) {
                    if(emptySlot == -1) emptySlot = i;
                    continue;
                }

                // Key exists; read it safely
                String current = prefs.getString(key.c_str(), "");

                if (current == val) alreadyExists = true;
                if (current.length() == 0 && emptySlot == -1) emptySlot = i;
            }

            if(!alreadyExists && emptySlot != -1) {
                prefs.putString((type + String(emptySlot)).c_str(), val);
            }
            prefs.end();
            loadCategoriesFromPrefs();
            fn_refresh_roller(NULL);
            request->redirect("/?msg=category_updated");
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