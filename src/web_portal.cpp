#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "web_portal.h"
#include <WiFi.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <lvgl.h>
#include "ui/ui.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

static String extractMetaContent(const String &html, const String &needle) {
    // Finds content="..." on the same tag as `needle`
    int p = html.indexOf(needle);
    if (p < 0) return "";
    int c = html.indexOf("content=\"", p);
    if (c < 0) return "";
    c += 9;
    int e = html.indexOf("\"", c);
    if (e < 0) return "";
    return html.substring(c, e);
}

static String extractFirstH1Text(const String &html) {
    // Prefer the specific label-for-band_name pattern if present
    int p = html.indexOf("for=\"band_name\"");
    if (p < 0) p = html.indexOf("for='band_name'");
    if (p < 0) p = html.indexOf("<h1");
    if (p < 0) return "";

    // Find the start of the <h1 ...> tag from p onward
    int h1 = html.indexOf("<h1", p);
    if (h1 < 0) return "";

    // Find end of opening tag
    int gt = html.indexOf(">", h1);
    if (gt < 0) return "";

    // Find closing tag
    int end = html.indexOf("</h1>", gt + 1);
    if (end < 0) return "";

    String inner = html.substring(gt + 1, end);

    // Strip any nested tags inside the H1 (simple)
    while (true) {
        int lt = inner.indexOf("<");
        if (lt < 0) break;
        int rt = inner.indexOf(">", lt);
        if (rt < 0) break;
        inner.remove(lt, rt - lt + 1);
    }

    inner.trim();
    return inner;
}

static String extractFixedImageSrc(const String &html) {
    // Look for <img class="fixedImage" src="...">
    int p = html.indexOf("class=\"fixedImage\"");
    if (p < 0) p = html.indexOf("class='fixedImage'");
    if (p < 0) return "";

    int s = html.indexOf("src=\"", p);
    bool isDouble = true;
    if (s < 0) { s = html.indexOf("src='", p); isDouble = false; }
    if (s < 0) return "";

    s += 5; // skip src="
    int e = html.indexOf(isDouble ? "\"" : "'", s);
    if (e < 0) return "";

    return html.substring(s, e);
}

static String htmlEntityDecode(String s) {
    // Minimal decode for common entities seen in titles
    s.replace("&amp;", "&");
    s.replace("&quot;", "\"");
    s.replace("&#039;", "'");
    s.replace("&lt;", "<");
    s.replace("&gt;", ">");
    return s;
}

static String extractLabelInnerTextByFor(const String &html, const String &forAttr) {
    int p = html.indexOf("for=\"" + forAttr + "\"");
    if (p < 0) p = html.indexOf("for='" + forAttr + "'");
    if (p < 0) return "";

    int labelStart = html.lastIndexOf("<label", p);
    if (labelStart < 0) return "";

    int gt = html.indexOf(">", labelStart);
    if (gt < 0) return "";

    int end = html.indexOf("</label>", gt + 1);
    if (end < 0) return "";

    String inner = html.substring(gt + 1, end);

    // Strip tags
    while (true) {
        int lt = inner.indexOf("<");
        if (lt < 0) break;
        int rt = inner.indexOf(">", lt);
        if (rt < 0) break;
        inner.remove(lt, rt - lt + 1);
    }

    inner.replace("&nbsp;", " ");
    inner = htmlEntityDecode(inner);
    inner.trim();
    return inner;
}

static String extractAfterBoldLabel(const String &html, const String &boldLabel) {
    String needle = "<b>" + boldLabel + "</b>";
    int p = html.indexOf(needle);
    if (p < 0) return "";

    int start = html.indexOf("</b>", p);
    if (start < 0) return "";
    start += 4;

    int end = html.indexOf("</label>", start);
    if (end < 0) end = html.indexOf("<br", start);
    if (end < 0) end = html.indexOf("</p>", start);
    if (end < 0) end = html.indexOf("\n", start);
    if (end < 0) return "";

    String inner = html.substring(start, end);

    // Strip tags
    while (true) {
        int lt = inner.indexOf("<");
        if (lt < 0) break;
        int rt = inner.indexOf(">", lt);
        if (rt < 0) break;
        inner.remove(lt, rt - lt + 1);
    }

    inner.replace("&nbsp;", " ");
    inner = htmlEntityDecode(inner);
    inner.trim();
    return inner;
}

static bool fetchMagicBandCollectors(int listingId, String &outTitle, String &outImageUrl);

// ---- Background MagicBandCollectors lookup worker ----
struct MbcLookupJob {
    int bandId;
    int listingId;
};

static volatile bool g_lookupInProgress = false;
static volatile int g_lookupResult = 0; // 0 none, 1 ok, -1 fail, 2 started, 3 busy
static volatile int g_lookupBandId = -1;

static String g_lastReleaseType, g_lastReleaseDate, g_lastReleasedAt,
              g_lastBandColor, g_lastIconColor, g_lastOriginalPrice, g_lastSku;

// Runs on a worker task so AsyncWebServer isn't blocked (prevents WDT reset)
static void mbcLookupTask(void *param) {
    MbcLookupJob *job = (MbcLookupJob*)param;

    String title, img;
    bool ok = fetchMagicBandCollectors(job->listingId, title, img);

    if(ok && job->bandId >= 0 && job->bandId < bandCount) {
        // Apply fetched fields (only overwrite if present)
        if(title.length() > 0) {
            strncpy(registeredBands[job->bandId].name, title.c_str(), sizeof(registeredBands[job->bandId].name) - 1);
            registeredBands[job->bandId].name[sizeof(registeredBands[job->bandId].name) - 1] = '\0';
        }
        if(img.length() > 0) {
            strncpy(registeredBands[job->bandId].imageUrl, img.c_str(), sizeof(registeredBands[job->bandId].imageUrl)-1);
            registeredBands[job->bandId].imageUrl[sizeof(registeredBands[job->bandId].imageUrl)-1] = '\0';
        }
        if(g_lastReleaseType.length() > 0) {
            strncpy(registeredBands[job->bandId].releaseType, g_lastReleaseType.c_str(), sizeof(registeredBands[job->bandId].releaseType)-1);
            registeredBands[job->bandId].releaseType[sizeof(registeredBands[job->bandId].releaseType)-1] = '\0';
        }
        if(g_lastReleaseDate.length() > 0) {
            strncpy(registeredBands[job->bandId].releaseDate, g_lastReleaseDate.c_str(), sizeof(registeredBands[job->bandId].releaseDate)-1);
            registeredBands[job->bandId].releaseDate[sizeof(registeredBands[job->bandId].releaseDate)-1] = '\0';
        }
        if(g_lastReleasedAt.length() > 0) {
            strncpy(registeredBands[job->bandId].releasedAt, g_lastReleasedAt.c_str(), sizeof(registeredBands[job->bandId].releasedAt)-1);
            registeredBands[job->bandId].releasedAt[sizeof(registeredBands[job->bandId].releasedAt)-1] = '\0';
        }
        if(g_lastBandColor.length() > 0) {
            strncpy(registeredBands[job->bandId].bandColorName, g_lastBandColor.c_str(), sizeof(registeredBands[job->bandId].bandColorName)-1);
            registeredBands[job->bandId].bandColorName[sizeof(registeredBands[job->bandId].bandColorName)-1] = '\0';
        }
        if(g_lastIconColor.length() > 0) {
            strncpy(registeredBands[job->bandId].iconColorName, g_lastIconColor.c_str(), sizeof(registeredBands[job->bandId].iconColorName)-1);
            registeredBands[job->bandId].iconColorName[sizeof(registeredBands[job->bandId].iconColorName)-1] = '\0';
        }
        if(g_lastOriginalPrice.length() > 0) {
            strncpy(registeredBands[job->bandId].originalPrice, g_lastOriginalPrice.c_str(), sizeof(registeredBands[job->bandId].originalPrice)-1);
            registeredBands[job->bandId].originalPrice[sizeof(registeredBands[job->bandId].originalPrice)-1] = '\0';
        }
        if(g_lastSku.length() > 0) {
            strncpy(registeredBands[job->bandId].sku, g_lastSku.c_str(), sizeof(registeredBands[job->bandId].sku)-1);
            registeredBands[job->bandId].sku[sizeof(registeredBands[job->bandId].sku)-1] = '\0';
        }

        prefs.begin("mbands", false);
        prefs.putBytes(("b" + String(job->bandId)).c_str(), &registeredBands[job->bandId], sizeof(BandRecord));
        prefs.end();

        g_lookupResult = 1;
    } else {
        g_lookupResult = -1;
    }

    g_lookupBandId = job->bandId;
    g_lookupInProgress = false;

    delete job;
    vTaskDelete(NULL);
}

static bool fetchMagicBandCollectors(int listingId, String &outTitle, String &outImageUrl) {
    outTitle = "";
    outImageUrl = "";

    if (WiFi.status() != WL_CONNECTED) return false;
    if (listingId <= 0) return false;

    WiFiClientSecure client;
    client.setInsecure(); // NOTE: skips TLS cert validation; simplest for ESP32

    HTTPClient http;
    http.setTimeout(8000);

    String url = "https://www.magicbandcollectors.com/magicband-details/?id=" + String(listingId);
    if (!http.begin(client, url)) return false;

    http.addHeader("User-Agent", "MagicBandHub/1.0");
    int code = http.GET();
    if (code != 200) {
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    // Basic safety: avoid extremely large pages on small RAM devices
    if (body.length() > 120000) return false;

    // Prefer OpenGraph tags (common on listing pages)
    String title = extractMetaContent(body, "property=\"og:title\"");
    if (title.length() == 0) title = extractMetaContent(body, "name=\"twitter:title\"");
    String img = extractMetaContent(body, "property=\"og:image\"");
    if (img.length() == 0) img = extractMetaContent(body, "name=\"twitter:image\"");
    // Fallback: listing often includes canonical image as <img class="fixedImage" src="...">
    if (img.length() == 0) img = extractFixedImageSrc(body);
    img = htmlEntityDecode(img);

    // Fallback: site sometimes uses a generic OG title like "MagicBand Details"
    String h1Title = extractFirstH1Text(body);
    if (h1Title.length() > 0) {
        // Use H1 when OG/Twitter title is missing or generic
        String t = title;
        t.trim();
        if (t.length() == 0 || t.equalsIgnoreCase("MagicBand Details") || t.indexOf("MagicBand Details") >= 0) {
            title = h1Title;
        }
    }

    title = htmlEntityDecode(title);

    // Some sites include the site name in the OG title; keep it if present, but trim whitespace
    title.trim();
    img.trim();

    // Extra fields
    String releaseType = extractLabelInnerTextByFor(body, "categoryOptions"); // Limited Release / Open Edition
    String releaseDate = extractLabelInnerTextByFor(body, "year");           // July 28, 2025

    String releasedAt = extractAfterBoldLabel(body, "Released at:");
    String bandColor  = extractAfterBoldLabel(body, "Band color:");
    String iconColor  = extractAfterBoldLabel(body, "Icon color:");

    // Original price: value appears in a subsequent <label for="original_price">...</label>
    String price = "";
    {
        int pPrice = body.indexOf("for=\"original_price\"");
        if (pPrice >= 0) {
            int nextLabel = body.indexOf("<label", pPrice + 1);
            if (nextLabel >= 0) {
                int gt = body.indexOf(">", nextLabel);
                int end = body.indexOf("</label>", gt + 1);
                if (gt >= 0 && end > gt) {
                    price = body.substring(gt + 1, end);
                    while (true) {
                        int lt = price.indexOf("<");
                        if (lt < 0) break;
                        int rt = price.indexOf(">", lt);
                        if (rt < 0) break;
                        price.remove(lt, rt - lt + 1);
                    }
                    price.replace("&nbsp;", " ");
                    price = htmlEntityDecode(price);
                    price.trim();
                }
            }
        }
    }

    // SKU: page uses two <label for="sku">...</label> elements (first is label text, second is value)
    String sku = "";
    {
        int p1 = body.indexOf("for=\"sku\"");
        if (p1 < 0) p1 = body.indexOf("for='sku'");
        if (p1 >= 0) {
            int p2 = body.indexOf("for=\"sku\"", p1 + 1);
            if (p2 < 0) p2 = body.indexOf("for='sku'", p1 + 1);

            int p = (p2 >= 0) ? p2 : p1;

            int labelStart = body.lastIndexOf("<label", p);
            if (labelStart >= 0) {
                int gt = body.indexOf(">", labelStart);
                int end = body.indexOf("</label>", gt + 1);
                if (gt >= 0 && end > gt) {
                    sku = body.substring(gt + 1, end);

                    // Strip tags
                    while (true) {
                        int lt = sku.indexOf("<");
                        if (lt < 0) break;
                        int rt = sku.indexOf(">", lt);
                        if (rt < 0) break;
                        sku.remove(lt, rt - lt + 1);
                    }

                    sku.replace("&nbsp;", " ");
                    sku = htmlEntityDecode(sku);
                    sku.trim();
                }
            }
        }
        if (sku.length() == 0) sku = extractLabelInnerTextByFor(body, "sku");
    }

    // Stash for worker task (only one lookup runs at a time)
    g_lastReleaseType = releaseType;
    g_lastReleaseDate = releaseDate;
    g_lastReleasedAt  = releasedAt;
    g_lastBandColor   = bandColor;
    g_lastIconColor   = iconColor;
    g_lastOriginalPrice = price;
    g_lastSku = sku;

    if (title.length() == 0 && img.length() == 0) return false;

    outTitle = title;
    outImageUrl = img;
    return true;
}

void initWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:sans-serif; text-align:center; background:#f4f4f9; padding:20px;} ";
        html += ".card{background:white; border-radius:15px; padding:15px; margin:10px auto; max-width:350px; box-shadow: 2px 2px 10px #ccc;} ";
        html += ".summary{display:flex; align-items:center; justify-content:space-between; gap:10px; cursor:pointer;} ";
        html += ".summary-left{display:flex; align-items:center; gap:10px; text-align:left;} ";
        html += ".thumb{width:110px; height:90px; object-fit:contain; border-radius:10px; background:#fff; box-sizing:border-box; padding-top:5px;} ";
        html += ".thumb{border:1px solid #e6e6e6;} ";
        html += ".settings-header{cursor:pointer; font-weight:bold; padding:10px;} .settings-body{display:none; text-align:left;} ";
        html += ".meta{color:#666; font-size:0.8em;} ";
        html += ".details{margin-top:10px; text-align:left;} ";
        html += "input, select{margin:5px; padding:8px; border-radius:5px; border:1px solid #ccc; width:80%;} ";
        html += ".btn-search{background:#5765f2; color:white; border:none; padding:8px; border-radius:5px; cursor:pointer; width:80%; margin-bottom:10px;} ";
        html += ".btn-save{background:#2ed573; color:white; border:none; padding:12px; width:80%; border-radius:8px; cursor:pointer;} ";
        html += ".btn-del{background:#ff4757; color:white; border:none; padding:5px 10px; border-radius:5px; font-size:0.8em; display:inline-block; text-decoration:none;} ";
        html += ".btn-del:active{opacity:0.9;} ";
        html += "</style>";
        
        // SEARCH FILTER SCRIPT
        html += "<script>function filterBands() { var val = document.getElementById('search').value.toLowerCase();";
        html += "var cards = document.getElementsByClassName('band-card');";
        html += "for (var i=0; i<cards.length; i++) { var txt = cards[i].innerText.toLowerCase();";
        html += "cards[i].style.display = txt.includes(val) ? '' : 'none'; }}";
        html += "function toggleDetails(id){ var el=document.getElementById('d'+id); if(!el) return; el.style.display=(el.style.display==='none'||el.style.display==='')?'block':'none'; }";
        html += "function toggleSettings(){ var el=document.getElementById('settings'); if(!el) return; el.style.display=(el.style.display==='none'||el.style.display==='')?'block':'none'; }";
        html += "</script></head><body>";

        if(request->hasParam("msg")) {
            String msg = request->getParam("msg")->value();
            if(msg == "category_updated") {
                html += "<div class='card' style='background:#d4edda;color:#155724;'>Category updated successfully.</div>";
            } else if(msg == "lookup_ok") {
                html += "<div class='card' style='background:#d4edda;color:#155724;'>Fetching listing details... refreshing shortly.</div>";
                // one-time refresh back to the open card (no msg param)
                String openId = request->hasParam("open") ? request->getParam("open")->value() : "0";
                html += "<meta http-equiv='refresh' content='2;url=/?open=" + openId + "'>";
            } else if(msg == "lookup_fail") {
                html += "<div class='card' style='background:#f8d7da;color:#721c24;'>Could not fetch listing details. Check the ID and Wi-Fi.</div>";
            }
        }

        // Background lookup status (prevents blocking the async_tcp task)
        if(g_lookupResult == 2) {
            html += "<div class='card' style='background:#fff3cd;color:#856404;'>Fetching listing details... refresh in a moment.</div>";
        } else if(g_lookupResult == 3) {
            html += "<div class='card' style='background:#fff3cd;color:#856404;'>A lookup is already running. Please wait and refresh.</div>";
        } else if(g_lookupResult == 1) {
            html += "<div class='card' style='background:#d4edda;color:#155724;'>Listing details applied to band.</div>";
            g_lookupResult = 0;
        } else if(g_lookupResult == -1) {
            html += "<div class='card' style='background:#f8d7da;color:#721c24;'>Could not fetch listing details. Check the ID and Wi-Fi.</div>";
            g_lookupResult = 0;
        }

        if(WiFi.status() != WL_CONNECTED) {
            html += "<h1>Hub Setup</h1><div class='card'><h3>Connect WiFi</h3><form action='/setwifi' method='GET'>";
            html += "SSID: <input type='text' name='ssid' required><br>Pass: <input type='password' name='pass'><br>";
            html += "<input type='submit' class='btn-save' value='Save & Restart'></form></div>";
        } else {
            html += "<h1>MagicBand Hub</h1>";
            
            // SEARCH BOX
            html += "<input type='text' id='search' onkeyup='filterBands()' placeholder='Filter by Owner, Location, or Name...' style='width:90%; padding:15px; margin-bottom:20px;'>";

            // BAND CARDS (summary + expandable details)

            // BAND CARDS (summary + expandable details)
            for(int i=0; i < bandCount; i++) {
                char hStr[8]; sprintf(hStr, "#%06X", (unsigned int)registeredBands[i].color);

                String ownerStr = (strlen(registeredBands[i].owner) > 0) ? String(registeredBands[i].owner) : String("None");
                String locStr   = (strlen(registeredBands[i].location) > 0) ? String(registeredBands[i].location) : String("None");

                html += "<div class='card band-card'>";

                // Summary header (click to expand)
                html += "<div class='summary' onclick='toggleDetails(" + String(i) + ")'>";
                html += "<div class='summary-left'>";
                if(strlen(registeredBands[i].imageUrl) > 5) {
                    html += "<img class='thumb' src='" + String(registeredBands[i].imageUrl) + "'>";
                } else {
                    html += "<div class='thumb'></div>";
                }
                html += "<div>";
                html += "<div><b>" + String(registeredBands[i].name) + "</b></div>";
                html += "<div class='meta'>Owner: " + ownerStr + " &nbsp;|&nbsp; Location: " + locStr + "</div>";
                html += "<div class='meta'>Type: " + String(registeredBands[i].type) + "</div>";
                html += "</div></div>";
                html += "<div class='meta'>Tap to edit</div>";
                html += "</div>";

                // Expandable details
                html += "<div class='details' id='d" + String(i) + "' style='display:none;'>";

                // Larger image preview (optional)
                if(strlen(registeredBands[i].imageUrl) > 5) {
                    html += "<img src='" + String(registeredBands[i].imageUrl) + "' style='width:120px; border-radius:10px; display:block; margin:10px auto;'><br>";
                }

                html += "<form action='/update' method='GET'><input type='hidden' name='id' value='" + String(i) + "'>";
                html += "Name: <input type='text' name='name' value='" + String(registeredBands[i].name) + "' maxlength='39'><br>";
                html += "Bought: <input type='date' name='date' value='" + String(registeredBands[i].dateBought) + "'><br>";

                // OWNER DROPDOWN
                html += "Owner: <select name='owner'><option value=''>None</option>";
                for(int j=0; j<10; j++) {
                    if(ownersList[j][0] != '\0') {
                        String sel = (String(registeredBands[i].owner) == String(ownersList[j])) ? "selected" : "";
                        html += "<option value='" + String(ownersList[j]) + "' " + sel + ">" + String(ownersList[j]) + "</option>";
                    }
                }
                html += "</select><br>";

                // LOCATION DROPDOWN
                html += "Location: <select name='loc'><option value=''>None</option>";
                for(int k=0; k<10; k++) {
                    if(locationsList[k][0] != '\0') {
                        String sel = (String(registeredBands[i].location) == String(locationsList[k])) ? "selected" : "";
                        html += "<option value='" + String(locationsList[k]) + "' " + sel + ">" + String(locationsList[k]) + "</option>";
                    }
                }
                html += "</select><br>";

                html += "Img URL: <input type='text' name='img' value='" + String(registeredBands[i].imageUrl) + "' placeholder='jpg link'><br>";

                // MagicBandCollectors fetch (runs async)
                html += "MBC Listing: <input type='text' name='mbc' value='" + String(registeredBands[i].mbcListing) + "' placeholder='2535 or full URL' style='width:80%;'><br>";
                html += "<button type='submit' formaction='/lookup' formmethod='GET' class='btn-search'>Fetch from MagicBandCollectors</button><br>";

                // Imported metadata (editable)
                html += "Release: <input type='text' name='rtype' value='" + String(registeredBands[i].releaseType) + "' placeholder='Limited Release'><br>";
                html += "Release Date: <input type='text' name='rdate' value='" + String(registeredBands[i].releaseDate) + "' placeholder='July 28, 2025'><br>";
                html += "Released At: <input type='text' name='rat' value='" + String(registeredBands[i].releasedAt) + "' placeholder='Disneyland, WDW, ShopDisney'><br>";
                html += "Band Color: <input type='text' name='bcol' value='" + String(registeredBands[i].bandColorName) + "' placeholder='Black'><br>";
                html += "Icon Color: <input type='text' name='icol' value='" + String(registeredBands[i].iconColorName) + "' placeholder='Custom Graphics'><br>";
                html += "Original Price: <input type='text' name='op' value='" + String(registeredBands[i].originalPrice) + "' placeholder='$54.99'><br>";
                html += "SKU / Barcode: <input type='text' name='sku' value='" + String(registeredBands[i].sku) + "' placeholder='400..., 419...'><br>";

                html += "Color: <input type='color' name='color' value='" + String(hStr) + "' style='width:40px;'><br>";
                html += "<input type='submit' value='Save Changes' class='btn-save'></form>";

                html += "<form action='/delete' method='GET' onsubmit='return confirm(\"Delete?\")'>";
                html += "<input type='hidden' name='id' value='" + String(i) + "'>";
                html += "<input type='submit' value='Delete Band' class='btn-del'></form>";

                html += "</div>"; // details
                html += "</div>"; // card
            }

            // SETTINGS (collapsible)
            html += "<div class='card'>";
            html += "<div class='settings-header' onclick='toggleSettings()'>Settings (Owners, Locations, Hub)</div>";
            html += "<div class='settings-body' id='settings'>";

            // OWNERS
            html += "<h3>Owners</h3>";
            for(int i=0; i<10; i++) {
                if(ownersList[i][0] != '\0') {
                    int usedCount = 0;
                    for(int b=0; b<bandCount; b++) {
                        if(strcmp(registeredBands[b].owner, ownersList[i]) == 0) usedCount++;
                    }
                    String warn = usedCount > 0 ?
                        "This owner is used by " + String(usedCount) + " band(s). They will be set to None. Continue?" :
                        "Delete owner?";
                    html += "<div style='display:flex; align-items:center; justify-content:space-between; gap:10px; margin:6px 0;'>";
                    html += "<div style='font-weight:600;'>" + String(ownersList[i]) + "</div>";
                    html += "<a href='/delcat?type=o&id=" + String(i) + "' class='btn-del' onclick='return confirm(\\\"" + warn + "\\\")'>Delete</a>";
                    html += "</div>";
                }
            }
            html += "<hr><form action='/addcat' method='GET'>";
            html += "<input type='hidden' name='type' value='o'>";
            html += "<input type='text' name='val' placeholder='New owner name' required><br>";
            html += "<input type='submit' class='btn-save' value='Add Owner'></form>";

            // LOCATIONS
            html += "<hr><h3>Locations</h3>";
            for(int i=0; i<10; i++) {
                if(locationsList[i][0] != '\0') {
                    int usedCount = 0;
                    for(int b=0; b<bandCount; b++) {
                        if(strcmp(registeredBands[b].location, locationsList[i]) == 0) usedCount++;
                    }
                    String warn = usedCount > 0 ?
                        "This location is used by " + String(usedCount) + " band(s). They will be set to None. Continue?" :
                        "Delete location?";
                    html += "<div style='display:flex; align-items:center; justify-content:space-between; gap:10px; margin:6px 0;'>";
                    html += "<div style='font-weight:600;'>" + String(locationsList[i]) + "</div>";
                    html += "<a href='/delcat?type=l&id=" + String(i) + "' class='btn-del' onclick='return confirm(\\\"" + warn + "\\\")'>Delete</a>";
                    html += "</div>";
                }
            }
            html += "<hr><form action='/addcat' method='GET'>";
            html += "<input type='hidden' name='type' value='l'>";
            html += "<input type='text' name='val' placeholder='New location name' required><br>";
            html += "<input type='submit' class='btn-save' value='Add Location'></form>";

            // HUB SETTINGS
            html += "<hr><h3>Hub Settings</h3><form action='/settings' method='GET'>";
            html += "Standby (Min): <input type='number' name='idle' value='" + String(idleTimeout / 60000) + "' min='1'><br>";
            html += "Sleep (Min): <input type='number' name='sleep' value='" + String(sleepTimeout / 60000) + "' min='1'><br>";
            html += "<input type='submit' class='btn-save' value='Save Settings'></form>";

            html += "</div></div>";
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
                if(request->hasParam("name")) {
                    strncpy(registeredBands[id].name, request->getParam("name")->value().c_str(), sizeof(registeredBands[id].name) - 1);
                    registeredBands[id].name[sizeof(registeredBands[id].name) - 1] = '\0';
                }
                if(request->hasParam("date")) strncpy(registeredBands[id].dateBought, request->getParam("date")->value().c_str(), 11);
                if(request->hasParam("owner")) {
                    strncpy(registeredBands[id].owner, request->getParam("owner")->value().c_str(), sizeof(registeredBands[id].owner) - 1);
                    registeredBands[id].owner[sizeof(registeredBands[id].owner) - 1] = '\0';
                }
                if(request->hasParam("loc")) {
                    strncpy(registeredBands[id].location, request->getParam("loc")->value().c_str(), sizeof(registeredBands[id].location) - 1);
                    registeredBands[id].location[sizeof(registeredBands[id].location) - 1] = '\0';
                }
                if(request->hasParam("color")) registeredBands[id].color = hexToUint(request->getParam("color")->value());
                if(request->hasParam("img")) {
                    strncpy(registeredBands[id].imageUrl, request->getParam("img")->value().c_str(), sizeof(registeredBands[id].imageUrl) - 1);
                    registeredBands[id].imageUrl[sizeof(registeredBands[id].imageUrl) - 1] = '\0';
                }
                if(request->hasParam("rtype")) {
                    strncpy(registeredBands[id].releaseType, request->getParam("rtype")->value().c_str(), sizeof(registeredBands[id].releaseType) - 1);
                    registeredBands[id].releaseType[sizeof(registeredBands[id].releaseType) - 1] = '\0';
                }
                if(request->hasParam("rdate")) {
                    strncpy(registeredBands[id].releaseDate, request->getParam("rdate")->value().c_str(), sizeof(registeredBands[id].releaseDate) - 1);
                    registeredBands[id].releaseDate[sizeof(registeredBands[id].releaseDate) - 1] = '\0';
                }
                if(request->hasParam("rat")) {
                    strncpy(registeredBands[id].releasedAt, request->getParam("rat")->value().c_str(), sizeof(registeredBands[id].releasedAt) - 1);
                    registeredBands[id].releasedAt[sizeof(registeredBands[id].releasedAt) - 1] = '\0';
                }
                if(request->hasParam("bcol")) {
                    strncpy(registeredBands[id].bandColorName, request->getParam("bcol")->value().c_str(), sizeof(registeredBands[id].bandColorName) - 1);
                    registeredBands[id].bandColorName[sizeof(registeredBands[id].bandColorName) - 1] = '\0';
                }
                if(request->hasParam("icol")) {
                    strncpy(registeredBands[id].iconColorName, request->getParam("icol")->value().c_str(), sizeof(registeredBands[id].iconColorName) - 1);
                    registeredBands[id].iconColorName[sizeof(registeredBands[id].iconColorName) - 1] = '\0';
                }
                if(request->hasParam("op")) {
                    strncpy(registeredBands[id].originalPrice, request->getParam("op")->value().c_str(), sizeof(registeredBands[id].originalPrice) - 1);
                    registeredBands[id].originalPrice[sizeof(registeredBands[id].originalPrice) - 1] = '\0';
                }
                if(request->hasParam("sku")) {
                    strncpy(registeredBands[id].sku, request->getParam("sku")->value().c_str(), sizeof(registeredBands[id].sku) - 1);
                    registeredBands[id].sku[sizeof(registeredBands[id].sku) - 1] = '\0';
                }
                if(request->hasParam("mbc")) {
                    strncpy(registeredBands[id].mbcListing,
                            request->getParam("mbc")->value().c_str(),
                            sizeof(registeredBands[id].mbcListing) - 1);
                    registeredBands[id].mbcListing[sizeof(registeredBands[id].mbcListing) - 1] = '\0';
                }
                prefs.begin("mbands", false);
                prefs.putBytes(("b" + String(id)).c_str(), &registeredBands[id], sizeof(BandRecord));
                prefs.end();
                fn_refresh_roller(NULL); 
            }
        }
        request->redirect("/");
    });

    // ROUTE: Lookup MagicBandCollectors listing and apply to a band (runs in background task)
    server.on("/lookup", HTTP_GET, [](AsyncWebServerRequest *request){
        if(!request->hasParam("id") || !request->hasParam("mbc")) {
            request->redirect("/?msg=lookup_fail");
            return;
        }

        int id = request->getParam("id")->value().toInt();
        String mbcRaw = request->getParam("mbc")->value();
        int mbc = 0;

        int idPos = mbcRaw.indexOf("id=");
        if (idPos >= 0) {
            String idStr = mbcRaw.substring(idPos + 3);
            int amp = idStr.indexOf("&");
            if (amp >= 0) idStr = idStr.substring(0, amp);
            mbc = idStr.toInt();
        } else {
            mbc = mbcRaw.toInt(); // handles plain "2526"
        }

        if(id < 0 || id >= bandCount || mbc <= 0) {
            request->redirect("/?msg=lookup_fail");
            return;
        }

        // Persist what the user entered so it stays in the form (even after refresh)
        strncpy(registeredBands[id].mbcListing, mbcRaw.c_str(),
                sizeof(registeredBands[id].mbcListing) - 1);
        registeredBands[id].mbcListing[sizeof(registeredBands[id].mbcListing) - 1] = '\0';

        prefs.begin("mbands", false);
        prefs.putBytes(("b" + String(id)).c_str(), &registeredBands[id], sizeof(BandRecord));
        prefs.end();

        if(g_lookupInProgress) {
            g_lookupResult = 3; // busy
            request->redirect("/?msg=lookup_fail");
            return;
        }

        g_lookupInProgress = true;
        g_lookupResult = 2; // started
        g_lookupBandId = id;

        MbcLookupJob *job = new MbcLookupJob();
        job->bandId = id;
        job->listingId = mbc;

        // Run on core 1 so async_tcp (often core 0) stays responsive
        xTaskCreatePinnedToCore(mbcLookupTask, "mbcLookupTask", 8192, job, 1, NULL, 1);

        request->redirect("/?msg=lookup_ok");
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