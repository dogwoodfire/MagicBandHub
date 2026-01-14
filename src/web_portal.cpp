#include "web_portal.h"
#include <WiFi.h>
#include <Preferences.h>

extern BandRecord registeredBands[10];
extern int bandCount;
extern AsyncWebServer server;
extern Preferences prefs;

// Helper: Converts #RRGGBB hex string from web to uint32
uint32_t hexToUint(String hex) {
    if(hex.startsWith("#")) hex = hex.substring(1);
    return (uint32_t) strtol(hex.c_str(), NULL, 16);
}

void initWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:sans-serif; text-align:center; background:#f4f4f9; padding:20px;} ";
        html += ".band{background:white; border-radius:15px; padding:15px; margin:10px auto; max-width:350px; box-shadow:0 4px 6px rgba(0,0,0,0.1);} ";
        html += "input{margin:5px; padding:8px; border-radius:5px; border:1px solid #ccc; width:80%;} ";
        html += ".btn-search{background:#5765f2; color:white; border:none; padding:8px; border-radius:5px; cursor:pointer; width:80%; margin-bottom:10px;} ";
        html += ".btn-save{background:#2ed573; color:white; border:none; padding:12px; width:80%; border-radius:8px; cursor:pointer;} ";
        html += ".btn-del{background:#ff4757; color:white; border:none; padding:5px 10px; border-radius:5px; margin-top:15px; font-size:0.8em;}</style></head><body>";
        html += "<h1>MagicBand Manager</h1>";

        for(int i=0; i < bandCount; i++) {
            // FIX: Declare and format hexStr properly inside the loop
            char hexStr[8];
            sprintf(hexStr, "#%06X", (unsigned int)registeredBands[i].color);
            
            html += "<div class='band'><h3>" + String(registeredBands[i].name) + "</h3>";
            
            // Show preview if URL exists
            if(strlen(registeredBands[i].imageUrl) > 5) {
                html += "<img src='" + String(registeredBands[i].imageUrl) + "' style='width:100px; border-radius:8px;'><br>";
            }

            html += "<form action='/update' method='GET'>";
            html += "<input type='hidden' name='id' value='" + String(i) + "'>";
            html += "Name: <input type='text' name='name' id='n"+String(i)+"' value='" + String(registeredBands[i].name) + "'><br>";
            
            // Search button logic
            html += "<button type='button' class='btn-search' onclick=\"window.open('https://www.magicbandcollectors.com/?s=' + document.getElementById('n"+String(i)+"').value)\">Find Image Online</button><br>";
            
            html += "Img URL: <input type='text' name='img' value='" + String(registeredBands[i].imageUrl) + "' placeholder='Paste .jpg link here'><br>";
            html += "Color: <input type='color' name='color' value='" + String(hexStr) + "'><br>";
            html += "<input type='submit' class='btn-save' value='Save All Changes'></form>";
            
            html += "<form action='/delete' method='GET' onsubmit='return confirm(\"Delete this band?\")'>";
            html += "<input type='hidden' name='id' value='" + String(i) + "'>";
            html += "<input type='submit' class='btn-del' value='Delete Band'></form></div>";
        }
        html += "</body></html>";
        request->send(200, "text/html", html);
    });

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
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("id")){
            int id = request->getParam("id")->value().toInt();
            if(id < bandCount) {
                if(request->hasParam("name")) strncpy(registeredBands[id].name, request->getParam("name")->value().c_str(), 19);
                if(request->hasParam("color")) registeredBands[id].color = hexToUint(request->getParam("color")->value());
                // Save the image URL
                if(request->hasParam("img")) strncpy(registeredBands[id].imageUrl, request->getParam("img")->value().c_str(), 99);
                
                prefs.begin("mbands", false);
                prefs.putBytes(("b" + String(id)).c_str(), &registeredBands[id], sizeof(BandRecord));
                prefs.end();
            }
        }
        request->redirect("/");
    });

    server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("id")){
            int id = request->getParam("id")->value().toInt();
            if(id < bandCount) {
                prefs.begin("mbands", false);
                for(int i = id; i < bandCount - 1; i++) {
                    registeredBands[i] = registeredBands[i+1];
                    prefs.putBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
                }
                bandCount--;
                prefs.putInt("count", bandCount);
                prefs.end();
            }
        }
        request->redirect("/");
    });
}

void startWebServer() { server.begin(); }
void stopWebServer() { server.end(); }