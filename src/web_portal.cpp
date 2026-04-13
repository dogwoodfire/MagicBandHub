#ifndef SCREEN_ENABLED
#define SCREEN_ENABLED 0
#endif
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "web_portal.h"
#include <WiFi.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#if SCREEN_ENABLED
#include <lvgl.h>
#include "ui/ui.h"
#else
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <SD_MMC.h>
#include "SD_Card.h"
// Forward-declare LVGL event type so we can keep stub/extern signatures without pulling LVGL in.
typedef struct _lv_event_t lv_event_t;
#endif

extern BandRecord registeredBands[];
extern int bandCount;
extern char ownersList[][20];
extern char locationsList[][20];
extern AsyncWebServer server;
extern Preferences prefs;
// Access the timeout variables from main.cpp
extern uint32_t idleTimeout;
extern uint32_t sleepTimeout;

extern "C" void fn_refresh_roller(lv_event_t * e);
extern "C" void formatUidString(const uint8_t* uid, size_t len, char* out, size_t outSize);

// Screen-less web-driven scan/register (implemented in main.cpp)
extern "C" void web_arm_scan();
extern "C" void web_cancel_scan();
extern "C" bool web_has_scan_result();
extern "C" bool web_scan_result_is_known();
extern "C" int  web_scan_known_index();
extern "C" void web_scan_uid_string(char *out, size_t outSize);
extern "C" void web_scan_type_string(char *out, size_t outSize);
extern "C" bool web_pending_new_band();
extern "C" int  web_confirm_save_pending_new(bool yes);

uint32_t hexToUint(String hex) {
    if(hex.startsWith("#")) hex = hex.substring(1);
    return (uint32_t) strtol(hex.c_str(), NULL, 16);
}

// HTML escape helper for safe output in attributes and text
static String htmlEscape(const String &in) {
    String s = in;
    s.replace("&", "&amp;");
    s.replace("<", "&lt;");
    s.replace(">", "&gt;");
    s.replace("\"", "&quot;");
    s.replace("'", "&#39;");
    return s;
}

// JSON escape helper for small JSON responses
static String jsonEscape(const String &in) {
    String s = in;
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    s.replace("\n", " ");
    s.replace("\r", " ");
    return s;
}

// ---- Theme definitions (IDs stored in BandRecord.themeId) ----
// Keep this list stable; you can expand later.
static const char* kThemeNames[] = {
    "Default",
    "Classic Green",
    "Comet Blue",
    "Pulse Purple",
    "Rainbow",
    "Spooky",
    "MNSSHP",
    "Boo To You"
};
static const int kThemeCount = (int)(sizeof(kThemeNames) / sizeof(kThemeNames[0]));

static String buildThemeSelectOptions(uint16_t currentId) {
    String out;
    for (int i = 0; i < kThemeCount; i++) {
        String sel = ((uint16_t)i == currentId) ? "selected" : "";
        out += "<option value='" + String(i) + "' " + sel + ">"
            + htmlEscape(String(kThemeNames[i])) + "</option>";
    }
    return out;
}

// --- Minimal JSON helpers (sufficient for importing OUR exported backup) ---
static String jsonUnescape(String s) {
    s.replace("\\\"", "\"");
    s.replace("\\\\", "\\");
    s.replace("\\n", " ");
    s.replace("\\r", " ");
    return s;
}

static bool jsonFindValueString(const String &obj, const String &key, String &out) {
    // Looks for "key":"value"
    String needle = "\"" + key + "\":";
    int p = obj.indexOf(needle);
    if (p < 0) return false;
    p += needle.length();

    // Skip whitespace
    while (p < (int)obj.length() && (obj[p] == ' ' || obj[p] == '\t' || obj[p] == '\n' || obj[p] == '\r')) p++;

    if (p >= (int)obj.length() || obj[p] != '\"') return false;
    p++;

    String val = "";
    bool esc = false;
    for (; p < (int)obj.length(); p++) {
        char ch = obj[p];
        if (esc) { val += ch; esc = false; continue; }
        if (ch == '\\') { val += ch; esc = true; continue; }
        if (ch == '\"') break;
        val += ch;
    }
    out = jsonUnescape(val);
    out.trim();
    return true;
}

static bool jsonFindValueInt(const String &obj, const String &key, int &out) {
    String needle = "\"" + key + "\":";
    int p = obj.indexOf(needle);
    if (p < 0) return false;
    p += needle.length();
    while (p < (int)obj.length() && (obj[p] == ' ' || obj[p] == '\t' || obj[p] == '\n' || obj[p] == '\r')) p++;

    int sign = 1;
    if (p < (int)obj.length() && obj[p] == '-') { sign = -1; p++; }

    long v = 0;
    bool any = false;
    while (p < (int)obj.length()) {
        char ch = obj[p];
        if (ch < '0' || ch > '9') break;
        v = v * 10 + (ch - '0');
        any = true;
        p++;
    }
    if (!any) return false;
    out = (int)(v * sign);
    return true;
}

static bool jsonFindValueUInt32(const String &obj, const String &key, uint32_t &out) {
    String needle = "\"" + key + "\":";
    int p = obj.indexOf(needle);
    if (p < 0) return false;
    p += needle.length();
    while (p < (int)obj.length() && (obj[p] == ' ' || obj[p] == '\t' || obj[p] == '\n' || obj[p] == '\r')) p++;

    // allow either plain number or quoted hex like "#RRGGBB"
    if (p < (int)obj.length() && obj[p] == '\"') {
        String s;
        if (!jsonFindValueString(obj, key, s)) return false;
        if (s.startsWith("#")) s = s.substring(1);
        out = (uint32_t) strtoul(s.c_str(), NULL, 16);
        return true;
    }

    unsigned long v = 0;
    bool any = false;
    while (p < (int)obj.length()) {
        char ch = obj[p];
        if (ch < '0' || ch > '9') break;
        v = v * 10 + (ch - '0');
        any = true;
        p++;
    }
    if (!any) return false;
    out = (uint32_t)v;
    return true;
}

static bool parseUidString(const String &uidStr, uint8_t *outUid, size_t uidLenExpected = 7) {
    // Accept "AA:BB:CC:DD:EE:FF:GG" or "AABBCC..." (hex pairs)
    for (size_t i = 0; i < uidLenExpected; i++) outUid[i] = 0;

    String s = uidStr;
    s.trim();
    s.toUpperCase();

    // Remove separators
    s.replace(":", "");
    s.replace("-", "");
    s.replace(" ", "");

    if ((int)s.length() < (int)(uidLenExpected * 2)) return false;

    for (size_t i = 0; i < uidLenExpected; i++) {
        String byteStr = s.substring(i * 2, i * 2 + 2);
        char *endp = nullptr;
        long v = strtol(byteStr.c_str(), &endp, 16);
        if (endp == byteStr.c_str()) return false;
        outUid[i] = (uint8_t)(v & 0xFF);
    }
    return true;
}

static bool extractJsonArrayOfStrings(const String &json, const String &key, String outArr[], int maxItems) {
    // Looks for "key":["a","b",...]
    String needle = "\"" + key + "\":[";
    int p = json.indexOf(needle);
    if (p < 0) return false;
    p += needle.length();

    int count = 0;
    while (p < (int)json.length() && count < maxItems) {
        while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r' || json[p] == ',')) p++;
        if (p >= (int)json.length()) break;
        if (json[p] == ']') break;
        if (json[p] != '\"') break;
        p++;

        String val = "";
        bool esc = false;
        for (; p < (int)json.length(); p++) {
            char ch = json[p];
            if (esc) { val += ch; esc = false; continue; }
            if (ch == '\\') { val += ch; esc = true; continue; }
            if (ch == '\"') break;
            val += ch;
        }
        val = jsonUnescape(val);
        val.trim();
        outArr[count++] = val;

        // move past closing quote
        while (p < (int)json.length() && json[p] != ',' && json[p] != ']') p++;
        if (p < (int)json.length() && json[p] == ']') break;
    }

    // Clear remaining
    for (int i = count; i < maxItems; i++) outArr[i] = "";
    return true;
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

// =========================================================================
// Custom Theme storage
// =========================================================================
CustomTheme customThemes[CUSTOM_THEME_MAX];
int customThemeCount = 0;

// SD upload state (used by /upload_audio handler)
File   _sdUploadFile;
bool   _sdUploadError = false;
String _sdUploadName;

void loadCustomThemesFromPrefs() {
    prefs.begin("cthemes", true);
    int c = prefs.getInt("count", 0);
    if (c < 0 || c > CUSTOM_THEME_MAX) c = 0;
    customThemeCount = c;
    for (int i = 0; i < customThemeCount; i++) {
        prefs.getBytes(("t" + String(i)).c_str(), &customThemes[i], sizeof(CustomTheme));
    }
    prefs.end();
}

int findCustomTheme(uint16_t themeId) {
    for (int i = 0; i < customThemeCount; i++) {
        if (customThemes[i].id == themeId) return i;
    }
    return -1;
}

static void saveCustomThemesToPrefs() {
    prefs.begin("cthemes", false);
    prefs.putInt("count", customThemeCount);
    for (int i = 0; i < customThemeCount; i++) {
        prefs.putBytes(("t" + String(i)).c_str(), &customThemes[i], sizeof(CustomTheme));
    }
    prefs.end();
}

static const char* customThemeLoopName(uint8_t p) {
    switch (p) {
        case CTL_NONE:           return "None (stop)";
        case CTL_SPINNING_COMET: return "Spinning Comet";
        case CTL_GENTLE_PULSE:   return "Gentle Pulse";
        case CTL_RAINBOW_SPIN:   return "Rainbow Spin";
    }
    return "Unknown";
}

static String customThemeSummary(const CustomTheme& ct) {
    String s;
    if (ct.phases & CTP_PHASE_COMET) s += "Comet ";
    if (ct.phases & CTP_PHASE_FILL)  s += "Fill ";
    if (ct.phases & CTP_PHASE_PULSE) s += "Pulse ";
    s += String("→ ") + String(customThemeLoopName(ct.loopPattern));
    return s;
}

// Build the band-edit Theme dropdown including custom themes
static String buildFullThemeSelectOptions(uint16_t currentId) {
    String out;
    // Built-in themes
    for (int i = 0; i < kThemeCount; i++) {
        String sel = ((uint16_t)i == currentId) ? "selected" : "";
        out += "<option value='" + String(i) + "' " + sel + ">"
            + htmlEscape(String(kThemeNames[i])) + "</option>";
    }
    // Custom themes
    if (customThemeCount > 0) {
        out += "<optgroup label='Custom Themes'>";
        for (int i = 0; i < customThemeCount; i++) {
            String sel = (customThemes[i].id == currentId) ? "selected" : "";
            out += "<option value='" + String(customThemes[i].id) + "' " + sel + ">"
                + htmlEscape(String(customThemes[i].name)) + "</option>";
        }
        out += "</optgroup>";
    }
    return out;
}

void initWebServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
        html += "<style>body{font-family:sans-serif; text-align:center; background:#3677A3; padding:20px;} ";
        html += "h1{color:#fff; font-weight:800; margin:10px 0 18px 0;} ";
        html += ".card{background:white; border-radius:15px; padding:15px; margin:10px auto; max-width:350px; box-shadow: 2px 2px 10px rgba(11, 18, 108, 0.6);} ";
        html += ".summary{display:flex; align-items:center; justify-content:space-between; gap:10px; cursor:pointer;} ";
        html += ".summary-left{display:flex; align-items:center; gap:10px; text-align:left;} ";
        html += ".thumb{width:110px; height:180px; object-fit:contain; object-position:top center; border-radius:10px; background:#fff; box-sizing:border-box; padding:10px; display:block;} ";
        html += ".thumb{border:1px solid #e6e6e6;} ";
        html += ".settings-header{cursor:pointer; font-weight:bold; padding:10px;} .settings-body{display:none; text-align:left;} ";
        html += ".meta{color:#666; font-size:0.8em;} ";
        html += ".status{margin-top:10px; padding:10px 12px; border-radius:10px; font-weight:800; background:#fff3cd; color:#856404; border:1px solid #ffeeba;} ";
        html += ".details{margin-top:12px; text-align:left;} ";
        html += "input, select{margin:0; padding:8px; border-radius:6px; border:1px solid #ccc; box-sizing:border-box; max-width:100%; font-size:16px;} ";
        html += ".btn-search{background:#5765f2; color:white; border:none; padding:8px; border-radius:5px; cursor:pointer; width:80%; margin-bottom:10px;} ";
        html += ".btn-save{background:#2ed573; color:white; border:none; padding:12px; width:80%; border-radius:8px; cursor:pointer; font-size:16px;} ";
        html += ".btn-save{box-sizing:border-box;} ";
        html += ".btn-del{background:#ff4757; color:white; border:none; padding:5px 10px; border-radius:5px; font-size:0.8em; display:inline-block; text-decoration:none;} ";
        html += ".btn-del:active{opacity:0.9;} ";
        html += ".details{margin-top:12px; text-align:left;} ";
        html += ".section{margin-top:18px; padding-top:12px; border-top:1px solid #eee;} ";
        html += ".section-title{font-size:0.85em; font-weight:600; color:#666; margin-bottom:10px;} ";
        html += ".field{margin-bottom:12px;} ";
        html += ".field label{display:block; font-size:0.8em; color:#555; margin-bottom:4px;} ";
        html += ".field input, .field select{width:100%; max-width:100%; display:block;} ";
        html += "input[type=date]{width:170px;} ";
        html += ".btn-secondary{background:#eef1ff; color:#3b4ce2; border:none; padding:10px 12px; width:100%; border-radius:8px; cursor:pointer; margin-top:6px; font-size:16px; box-sizing:border-box; display:block;} ";
        html += ".scan-btn{width:auto; padding:10px 14px; margin:0; font-size:16px;} ";
        html += ".scan-cancel{background:#ff4757; color:#fff; border:none; border-radius:8px; cursor:pointer;} ";
        html += "</style>";
        
        // SEARCH FILTER SCRIPT
        html += "<script>function filterBands() { var val = document.getElementById('search').value.toLowerCase();";
        html += "var cards = document.getElementsByClassName('band-card');";
        html += "for (var i=0; i<cards.length; i++) { var txt = cards[i].innerText.toLowerCase();";
        html += "cards[i].style.display = txt.includes(val) ? '' : 'none'; }}";
        html += "function toggleDetails(id){ var el=document.getElementById('d'+id); if(!el) return; el.style.display=(el.style.display==='none'||el.style.display==='')?'block':'none'; }";
        // Auto-open band card from ?open= param on load
        html += "window.addEventListener('load', function(){ startScanPoll(); try{ var p=new URLSearchParams(window.location.search); var open=p.get('open'); if(open!==null){ var id=parseInt(open,10); if(!isNaN(id)){ toggleDetails(id); var el=document.getElementById('d'+id); if(el){ el.scrollIntoView({behavior:'smooth', block:'start'}); } } } }catch(e){} });";
        html += "function toggleSettings(){ var el=document.getElementById('settings'); if(!el) return; el.style.display=(el.style.display==='none'||el.style.display==='')?'block':'none'; }";
                // Screen-less scan/register helpers
        html += "let __scanPoll=null;";
        html += "function lookupMbc(id){ try{ var inp=document.getElementById('mbc'+id); var st=document.getElementById('mbcStatus'+id); if(!inp) return; var mbc=encodeURIComponent(inp.value||''); if(st){ st.style.display='block'; st.innerText='Fetching listing details...'; } fetch('/lookup?ajax=1&id='+id+'&mbc='+mbc).then(r=>r.json()).then(d=>{ if(!d||!d.status){ if(st) st.innerText='Fetch failed.'; return; } if(d.status==='busy'){ if(st) st.innerText='Another fetch is already running. Please wait.'; return; } if(d.status==='fail'){ if(st) st.innerText='Fetch failed. Check Wi-Fi and the listing.'; return; } if(st) st.innerText='Fetching from MagicBandCollectors.com...'; setTimeout(function(){ window.location='/?open='+id; }, 5000); }).catch(()=>{ if(st) st.innerText='Fetch failed.'; }); }catch(e){} }";
        html += "function armScan(){ fetch('/scan_arm?ajax=1').then(()=>{ var c=document.getElementById('btnScanCancel'); if(c) c.style.display='inline-block'; showScanFetching(); startScanPoll(); }).catch(()=>{}); }";
        html += "function cancelScan(){ fetch('/scan_cancel?ajax=1').then(()=>{ stopScanPoll(); var c=document.getElementById('btnScanCancel'); if(c) c.style.display='none'; var body=document.getElementById('scanBody'); var hint=document.getElementById('scanHint'); if(body) body.style.display='none'; if(hint) hint.innerText='Scan cancelled.'; }).catch(()=>{}); }";
        html += "function showScanFetching(){ var body=document.getElementById('scanBody'); var hint=document.getElementById('scanHint'); if(!body||!hint) return; body.style.display='block'; hint.innerText='Tap a band to the reader...'; body.innerHTML='<div style=\"padding:10px;border:1px dashed #ccc;border-radius:10px;\">Waiting for a band... <div style=\"margin-top:8px;font-weight:700;\">(Scanning active)</div></div>'; }";
        html += "function startScanPoll(){ if(__scanPoll) return; __scanPoll=setInterval(pollScan, 700); pollScan(); }";
        html += "function stopScanPoll(){ if(__scanPoll){ clearInterval(__scanPoll); __scanPoll=null; } }";
        html += "function showScanArmed(){ var body=document.getElementById('scanBody'); var hint=document.getElementById('scanHint'); if(!body||!hint) return; body.style.display='block'; hint.innerText='Tap a band to the reader...'; body.innerHTML='<div style=\"padding:10px;border:1px dashed #ccc;border-radius:10px;\">Waiting for a band...</div>'; }";
        html += "function showScanKnown(d){ var body=document.getElementById('scanBody'); var hint=document.getElementById('scanHint'); if(!body||!hint) return; body.style.display='block'; hint.innerText='Known band detected'; var img=''; if(d.img){ img='<img src=\"'+d.img+'\" style=\"width:160px;border-radius:10px;display:block;margin:10px auto;background:#fff;padding:10px;box-sizing:border-box;border:1px solid #e6e6e6;\">'; } body.innerHTML= img + '<div style=\"font-weight:800;\">'+(d.name||'Known band')+'</div><div class=\"meta\">'+(d.type||'')+'</div><button class=\"btn-secondary\" style=\"margin-top:10px;\" onclick=\"openBand('+d.knownIndex+');return false;\">Open / Edit</button>'; stopScanPoll(); }";
        html += "function showScanNew(d){ var body=document.getElementById('scanBody'); var hint=document.getElementById('scanHint'); if(!body||!hint) return; var c=document.getElementById('btnScanCancel'); if(c) c.style.display='none'; body.style.display='block'; hint.innerText='New band detected'; body.innerHTML='<div style=\"font-weight:800;\">New band</div><div class=\"meta\">'+(d.uid||'')+' &nbsp;'+(d.type||'')+'</div><div style=\"display:flex;gap:10px;margin-top:10px;\"><button class=\"btn-save\" style=\"width:100%;padding:10px;\" onclick=\"confirmSave(1);return false;\">Yes, save</button><button class=\"btn-del\" style=\"width:100%;padding:10px;\" onclick=\"confirmSave(0);return false;\">No</button></div>'; }";
        html += "function confirmSave(yes){ fetch('/scan_confirm?yes='+yes+'&ajax=1').then(r=>r.json()).then(d=>{ var c=document.getElementById('btnScanCancel'); if(c) c.style.display='none'; if(d && d.saved && d.openId>=0){ window.location='/?msg=scan_saved&open='+d.openId; } else { window.location='/?msg=scan_cancel'; } }).catch(()=>{ window.location='/?msg=scan_cancel'; }); }";
        html += "function openBand(id){ window.location='/?open='+id; }";
        html += "function pollScan(){ fetch('/scan_status').then(r=>r.json()).then(d=>{ if(!d||!d.state) return; if(d.state==='known'){ showScanKnown(d); } else if(d.state==='new'){ showScanNew(d); } else if(d.state==='armed'){ showScanArmed(); } }).catch(()=>{}); }";
        html += "</script></head><body>";

        if(request->hasParam("msg")) {
            String msg = request->getParam("msg")->value();
            if(msg == "category_updated") {
                html += "<div class='card' style='background:#d4edda;color:#155724;'>Category updated successfully.</div>";
            } else if(msg == "lookup_ok") {
                html += "<div class='card' style='background:#d4edda;color:#155724;'>Fetching listing details... refreshing shortly.</div>";
                // one-time refresh back to the open card (no msg param)
                String openId = request->hasParam("open") ? request->getParam("open")->value() : "0";
                html += "<meta http-equiv='refresh' content='5;url=/?open=" + openId + "'>";
            } else if(msg == "saved") {
                html += "<div class='card' style='background:#d4edda;color:#155724;'>Saved &#10003;</div>";
            } else if(msg == "lookup_busy") {
                html += "<div class='card' style='background:#fff3cd;color:#856404;'>A lookup is already running. Please wait and refresh.</div>";
            } else if(msg == "lookup_fail") {
                html += "<div class='card' style='background:#f8d7da;color:#721c24;'>Could not fetch listing details. Check the ID and Wi-Fi.</div>";
            } else if(msg == "scan_armed") {
                html += "<div class='card' style='background:#d1ecf1;color:#0c5460;'>Ready to scan. Tap a band to the reader&#8230;</div>";
            } else if(msg == "scan_saved") {
                html += "<div class='card' style='background:#d4edda;color:#155724;'>Band saved &#10003;</div>";
            } else if(msg == "scan_cancel") {
                html += "<div class='card' style='background:#fff3cd;color:#856404;'>Scan cancelled.</div>";
            } else if(msg == "export_ready") {
                html += "<div class='card' style='background:#d1ecf1;color:#0c5460;'>Backup ready. Your download should start automatically.</div>";
            } else if(msg == "import_ok") {
                html += "<div class='card' style='background:#d4edda;color:#155724;'>Import complete &#10003;</div>";
            } else if(msg == "import_fail") {
                html += "<div class='card' style='background:#f8d7da;color:#721c24;'>Import failed. Paste a valid backup JSON from this hub.</div>";
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

            // SCREEN-LESS REGISTER / SCAN CARD
            html += "<div class='card' id='scanCard' style='text-align:left;'>";
            html += "<div style='display:flex; align-items:center; justify-content:space-between; gap:10px;'>";
            html += "<div style='font-weight:800; color:#1f2d3d;'>Register Band</div>";
            html += "<div style='display:flex; gap:8px;'>";
            html += "<button id='btnScanStart' class='btn-secondary scan-btn' onclick='armScan(); return false;'>Start</button>";
            html += "<button id='btnScanCancel' class='scan-btn scan-cancel' style='display:none;' onclick='cancelScan(); return false;'>Cancel</button>";
            html += "</div>";
            html += "</div>";
            html += "<div class='meta' id='scanHint' style='margin-top:6px;'>Start a scan from the browser (screen-less mode). LEDs will swirl until a band is detected.</div>";
            html += "<div id='scanBody' style='margin-top:10px; display:none;'></div>";
            html += "</div>";
            
            // SEARCH BOX
            html += "<input type='text' id='search' onkeyup='filterBands()' placeholder='Filter by Owner, Location, or Name...' style='width:90%; padding:15px; margin-bottom:20px;'>";

            // BAND CARDS (summary + expandable details)

            // BAND CARDS (summary + expandable details)
            for(int i=0; i < bandCount; i++) {
                char hStr[8]; sprintf(hStr, "#%06X", (unsigned int)registeredBands[i].color);

                String ownerStr = (strlen(registeredBands[i].owner) > 0) ? String(registeredBands[i].owner) : String("None");
                String locStr   = (strlen(registeredBands[i].location) > 0) ? String(registeredBands[i].location) : String("None");
                String escName  = htmlEscape(String(registeredBands[i].name));
                String escOwner = htmlEscape(ownerStr);
                String escLoc   = htmlEscape(locStr);
                String escType  = htmlEscape(String(registeredBands[i].type));
                String escImg   = htmlEscape(String(registeredBands[i].imageUrl));
                String escMbc   = htmlEscape(String(registeredBands[i].mbcListing));
                String escRtype = htmlEscape(String(registeredBands[i].releaseType));
                String escRdate = htmlEscape(String(registeredBands[i].releaseDate));
                String escRat   = htmlEscape(String(registeredBands[i].releasedAt));
                String escBcol  = htmlEscape(String(registeredBands[i].bandColorName));
                String escIcol  = htmlEscape(String(registeredBands[i].iconColorName));
                String escOp    = htmlEscape(String(registeredBands[i].originalPrice));
                String escSku   = htmlEscape(String(registeredBands[i].sku));

                html += "<div class='card band-card'>";

                // Summary header (click to expand)
                html += "<div class='summary' onclick='toggleDetails(" + String(i) + ")'>";
                html += "<div class='summary-left'>";
                if(strlen(registeredBands[i].imageUrl) > 5) {
                    html += "<img class='thumb' src='" + escImg + "'>";
                } else {
                    html += "<div class='thumb'></div>";
                }
                html += "<div>";
                html += "<div><b>" + escName + "</b></div>";
                html += "<div class='meta'>Owner: " + escOwner + " &nbsp;|&nbsp; Location: " + escLoc + "</div>";
                html += "<div class='meta'>Type: " + escType + "</div>";
                html += "</div></div>";
                html += "<div class='meta'>Tap to edit</div>";
                html += "</div>";

                // Expandable details
                html += "<div class='details' id='d" + String(i) + "' style='display:none;'>";

                // Larger image preview (optional)
                if(strlen(registeredBands[i].imageUrl) > 5) {
                    html += "<img src='" + escImg + "' style='width:160px; border-radius:10px; display:block; margin:10px auto; background:#fff; padding:10px; box-sizing:border-box; border:1px solid #e6e6e6;'><br>";
                }

                html += "<form action='/update' method='GET'><input type='hidden' name='id' value='" + String(i) + "'>";
                // General Section
                html += "<div class='section'>";
                html += "<div class='section-title'>General</div>";
                html += "<div class='field'><label>Name</label><input type='text' name='name' value='" + escName + "' maxlength='39'></div>";
                html += "<div class='field'><label>Bought</label><input type='date' name='date' value='" + String(registeredBands[i].dateBought) + "'></div>";
                // OWNER DROPDOWN
                html += "<div class='field'><label>Owner</label><select name='owner'><option value=''>None</option>";
                for(int j=0; j<10; j++) {
                    if(ownersList[j][0] != '\0') {
                        String opt = htmlEscape(String(ownersList[j]));
                        String sel = (String(registeredBands[i].owner) == String(ownersList[j])) ? "selected" : "";
                        html += "<option value='" + opt + "' " + sel + ">" + opt + "</option>";
                    }
                }
                html += "</select></div>";
                // LOCATION DROPDOWN
                html += "<div class='field'><label>Location</label><select name='loc'><option value=''>None</option>";
                for(int k=0; k<10; k++) {
                    if(locationsList[k][0] != '\0') {
                        String opt = htmlEscape(String(locationsList[k]));
                        String sel = (String(registeredBands[i].location) == String(locationsList[k])) ? "selected" : "";
                        html += "<option value='" + opt + "' " + sel + ">" + opt + "</option>";
                    }
                }
                html += "</select></div>";
                html += "</div>";
                // MagicBandCollectors Section
                html += "<div class='section'>";
                html += "<div class='section-title'>MagicBandCollectors</div>";
                html += "<div class='field'><label>MagicBandCollectors.com Listing</label><input id='mbc" + String(i) + "' type='text' name='mbc' value='" + escMbc + "' placeholder='2535 or full URL'></div>";
                html += "<button type='button' class='btn-secondary' onclick='lookupMbc(" + String(i) + "); return false;'>Fetch from MagicBandCollectors</button>";
                html += "<div id='mbcStatus" + String(i) + "' class='status' style='display:none;'></div>";
                html += "</div>";
                // Imported Metadata Section
                html += "<div class='section'>";
                html += "<div class='section-title'>Imported Metadata</div>";
                html += "<div class='field'><label>Image URL</label><input type='text' name='img' value='" + escImg + "' placeholder='.jpg link'></div>";
                html += "<div class='field'><label>Release</label><input type='text' name='rtype' value='" + escRtype + "'></div>";
                html += "<div class='field'><label>Release Date</label><input type='text' name='rdate' value='" + escRdate + "'></div>";
                html += "<div class='field'><label>Sold locations</label><input type='text' name='rat' value='" + escRat + "'></div>";
                html += "<div class='field'><label>Band Color</label><input type='text' name='bcol' value='" + escBcol + "'></div>";
                html += "<div class='field'><label>Icon Color</label><input type='text' name='icol' value='" + escIcol + "'></div>";
                html += "<div class='field'><label>Original Price</label><input type='text' name='op' value='" + escOp + "'></div>";
                html += "<div class='field'><label>SKU / Barcode</label><input type='text' name='sku' value='" + escSku + "'></div>";
                html += "</div>";
                // Appearance Section
                html += "<div class='section'>";
                html += "<div class='section-title'>Appearance</div>";

                // Theme dropdown
                html += "<div class='field'><label>Theme</label><select name='theme'>";
                html += buildFullThemeSelectOptions(registeredBands[i].themeId);
                html += "</select></div>";

                // Color picker
                html += "<div class='field'><label>Scanning Lights Colour</label><input type='color' name='color' value='" + String(hStr) + "' style='width:60px;'></div>";

                html += "<input type='submit' value='Save Changes' class='btn-save'>";
                html += "</div>";
                html += "</form>";
                // Danger Zone Section
                html += "<div class='section'>";
                html += "<div class='section-title'>Danger Zone</div>";
                html += "<form action='/delete' method='GET' onsubmit='return confirm(\"Delete?\")'>";
                html += "<input type='hidden' name='id' value='" + String(i) + "'>";
                html += "<input type='submit' value='Delete Band' class='btn-del'></form>";
                html += "</div>";

                html += "</div>"; // details
                html += "</div>"; // card
            }

            // SETTINGS (collapsible)
            html += "<div class='card'>";
            html += "<div class='settings-header' onclick='toggleSettings()'>Settings (Owners, Locations, Hub)</div>";
            html += "<div class='settings-body' id='settings'>";

            // BACKUP / RESTORE
            html += "<hr><h3>Backup / Restore</h3>";
            html += "<div class='meta' style='margin-bottom:10px;'>Export your bands before flashing new firmware. Import restores bands + owners + locations + hub timeouts.</div>";
            html += "<a class='btn-secondary' style='text-align:center;text-decoration:none;margin-top:8px;' href='/export' download>Export Backup (JSON)</a>";

            // THEME BUILDER LINK
            html += "<hr><h3>Theme Builder</h3>";
            html += "<div class='meta' style='margin-bottom:10px;'>Create custom LED light patterns with your own colours and audio files.</div>";
            html += "<a class='btn-secondary' style='text-align:center;text-decoration:none;margin-top:8px;' href='/themes'>Open Theme Builder</a>";

            html += "<hr><form action='/import' method='POST'>";
            html += "<div class='field'><label>Import Backup JSON</label>";
            html += "<textarea name='data' style='width:100%;min-height:140px;padding:10px;border-radius:8px;border:1px solid #ccc;font-size:14px;' placeholder='Paste exported JSON here...'></textarea>";
            html += "</div>";
            html += "<input type='submit' class='btn-save' value='Import Backup' onclick='return confirm(\"Import will overwrite all stored bands. Continue?\")'>";
            html += "</form>";

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
            html += "<hr><form action='/addcat' method='GET' style='margin-top:10px;'>";
            html += "<input type='hidden' name='type' value='o'>";
            html += "<div style='display:flex; gap:10px; align-items:center;'>";
            html += "<input type='text' name='val' placeholder='New owner name' required style='flex:1;'>";
            html += "<input type='submit' class='btn-save' value='Add Owner' style='width:auto; padding:10px 14px;'>";
            html += "</div></form>";

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
            html += "<hr><form action='/addcat' method='GET' style='margin-top:10px;'>";
            html += "<input type='hidden' name='type' value='l'>";
            html += "<div style='display:flex; gap:10px; align-items:center;'>";
            html += "<input type='text' name='val' placeholder='New location name' required style='flex:1;'>";
            html += "<input type='submit' class='btn-save' value='Add Location' style='width:auto; padding:10px 14px;'>";
            html += "</div></form>";

            // (Hub Settings removed)

            html += "</div></div>";
        }
        html += "</body></html>";
        request->send(200, "text/html; charset=utf-8", html);
    });

    // (Removed /settings handler)

    // ROUTE: Arm a web-driven scan (screen-less register)
    server.on("/scan_arm", HTTP_GET, [](AsyncWebServerRequest *request){
        web_arm_scan();
        if(request->hasParam("ajax")) {
            request->send(200, "text/plain", "OK");
        } else {
            request->redirect("/?msg=scan_armed");
        }
    });

    // ROUTE: Cancel a web-driven scan (screen-less register)
    server.on("/scan_cancel", HTTP_GET, [](AsyncWebServerRequest *request){
        web_cancel_scan();
        if(request->hasParam("ajax")) {
            request->send(200, "text/plain", "OK");
        } else {
            request->redirect("/?msg=scan_cancel");
        }
    });

    // ROUTE: Poll scan status (JSON)
    server.on("/scan_status", HTTP_GET, [](AsyncWebServerRequest *request){
        char uidBuf[32]; uidBuf[0] = '\0';
        char typeBuf[24]; typeBuf[0] = '\0';
        web_scan_uid_string(uidBuf, sizeof(uidBuf));
        web_scan_type_string(typeBuf, sizeof(typeBuf));

        String state = "idle";
        if(web_pending_new_band()) {
            state = "new";
        } else if(web_has_scan_result() && web_scan_result_is_known()) {
            state = "known";
        } else if(web_has_scan_result() && !web_scan_result_is_known()) {
            state = "new";
        } else {
            // No result yet: if user armed scan, show armed unless nothing is set at all
            if(String(uidBuf).length() == 0 && String(typeBuf).length() == 0) state = "armed";
        }

        int kidx = web_scan_known_index();

        String name = "";
        String img  = "";
        String type = String(typeBuf);
        if(state == "known" && kidx >= 0 && kidx < bandCount) {
            name = String(registeredBands[kidx].name);
            img  = String(registeredBands[kidx].imageUrl);
            type = String(registeredBands[kidx].type);
        }

        String json = "{";
        json += "\"state\":\"" + jsonEscape(state) + "\",";
        json += "\"uid\":\"" + jsonEscape(String(uidBuf)) + "\",";
        json += "\"type\":\"" + jsonEscape(type) + "\",";
        json += "\"knownIndex\":" + String(kidx) + ",";
        json += "\"name\":\"" + jsonEscape(name) + "\",";
        json += "\"img\":\"" + jsonEscape(img) + "\"";
        json += "}";
        request->send(200, "application/json", json);
    });

    // ROUTE: Confirm saving a newly scanned band (yes/no)
    server.on("/scan_confirm", HTTP_GET, [](AsyncWebServerRequest *request){
        bool yes = false;
        if(request->hasParam("yes")) {
            String yesStr = request->getParam("yes")->value();
            yes = (yesStr == "1" || yesStr.equalsIgnoreCase("true") || yesStr.equalsIgnoreCase("yes"));
        }

        int newIdx = web_confirm_save_pending_new(yes);

        if(request->hasParam("ajax")) {
            String json = "{";
            json += "\"saved\":" + String((newIdx >= 0) ? "true" : "false") + ",";
            json += "\"openId\":" + String(newIdx);
            json += "}";
            request->send(200, "application/json", json);
            return;
        }

        if(newIdx >= 0) {
            request->redirect("/?msg=scan_saved&open=" + String(newIdx));
        } else {
            request->redirect("/?msg=scan_cancel");
        }
    });

    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
        if(request->hasParam("id")){
            int id = request->getParam("id")->value().toInt();
            if(id < bandCount) {
                auto getParamTrimmed = [&](const char* name) -> String {
                    if(!request->hasParam(name)) return "";
                    String v = request->getParam(name)->value();
                    v.trim();
                    return v;
                };

                auto copyIfNonEmpty = [&](const char* name, char* dst, size_t dstSize) {
                    String v = getParamTrimmed(name);
                    if(v.length() == 0) return; // non-destructive
                    strncpy(dst, v.c_str(), dstSize - 1);
                    dst[dstSize - 1] = '\0';
                };

                // Non-destructive field updates: only overwrite when value is non-empty
                copyIfNonEmpty("name",  registeredBands[id].name, sizeof(registeredBands[id].name));
                copyIfNonEmpty("owner", registeredBands[id].owner, sizeof(registeredBands[id].owner));
                copyIfNonEmpty("loc",   registeredBands[id].location, sizeof(registeredBands[id].location));
                copyIfNonEmpty("img",   registeredBands[id].imageUrl, sizeof(registeredBands[id].imageUrl));
                copyIfNonEmpty("rtype", registeredBands[id].releaseType, sizeof(registeredBands[id].releaseType));
                copyIfNonEmpty("rdate", registeredBands[id].releaseDate, sizeof(registeredBands[id].releaseDate));
                copyIfNonEmpty("rat",   registeredBands[id].releasedAt, sizeof(registeredBands[id].releasedAt));
                copyIfNonEmpty("bcol",  registeredBands[id].bandColorName, sizeof(registeredBands[id].bandColorName));
                copyIfNonEmpty("icol",  registeredBands[id].iconColorName, sizeof(registeredBands[id].iconColorName));
                copyIfNonEmpty("op",    registeredBands[id].originalPrice, sizeof(registeredBands[id].originalPrice));
                copyIfNonEmpty("sku",   registeredBands[id].sku, sizeof(registeredBands[id].sku));
                copyIfNonEmpty("mbc",   registeredBands[id].mbcListing, sizeof(registeredBands[id].mbcListing));

                // Bought date: apply only if it looks like YYYY-MM-DD (10 chars). Otherwise keep existing.
                {
                    String d = getParamTrimmed("date");
                    if (d.length() == 10) {
                        strncpy(registeredBands[id].dateBought, d.c_str(), 11);
                        registeredBands[id].dateBought[10] = '\0';
                    }
                }

                // Color: apply only if present and non-empty
                {
                    String c = getParamTrimmed("color");
                    if (c.length() > 0) {
                        registeredBands[id].color = hexToUint(c);
                    }
                }

                // Theme: apply if present; allow 0 (Default), built-in IDs, or custom IDs (>=100)
                {
                    if (request->hasParam("theme")) {
                        String t = request->getParam("theme")->value();
                        t.trim();
                        int tid = t.toInt();
                        if (tid < 0) tid = 0;
                        // Allow built-in range OR valid custom id
                        bool isBuiltIn  = (tid < kThemeCount);
                        bool isCustom   = (findCustomTheme((uint16_t)tid) >= 0);
                        if (!isBuiltIn && !isCustom) tid = 0;
                        registeredBands[id].themeId = (uint16_t)tid;
                    }
                }

                prefs.begin("mbands", false);
                prefs.putBytes(("b" + String(id)).c_str(), &registeredBands[id], sizeof(BandRecord));
                prefs.end();
                fn_refresh_roller(NULL); 
            }
        }
        String openId = request->hasParam("id") ? request->getParam("id")->value() : "0";
        request->redirect("/?msg=saved&open=" + openId);
    });

    // ROUTE: Lookup MagicBandCollectors listing and apply to a band (runs in background task)
    server.on("/lookup", HTTP_GET, [](AsyncWebServerRequest *request){
        if(!request->hasParam("id") || !request->hasParam("mbc")) {
            if(request->hasParam("ajax")) {
                request->send(200, "application/json", "{\"status\":\"fail\"}");
            } else {
                request->redirect("/?msg=lookup_fail");
            }
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
            if(request->hasParam("ajax")) {
                request->send(200, "application/json", "{\"status\":\"fail\"}");
            } else {
                request->redirect("/?msg=lookup_fail");
            }
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
            if(request->hasParam("ajax")) {
                request->send(200, "application/json", "{\"status\":\"busy\"}");
            } else {
                request->redirect("/?msg=lookup_busy&open=" + String(id));
            }
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

        if(request->hasParam("ajax")) {
            request->send(200, "application/json", "{\"status\":\"started\"}");
        } else {
            request->redirect("/?msg=lookup_ok&open=" + String(id));
        }
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

    // ROUTE: Export backup JSON bands + owners + locations + hub settings
    server.on("/export", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{";
        json += "\"version\":1,";
        json += "\"bandCount\":" + String(bandCount) + ",";
        json += "\"bands\":[";

        for(int i = 0; i < bandCount; i++) {
            // UID string
            char uidBuf[32] = {0};
            formatUidString(registeredBands[i].uid, 7, uidBuf, sizeof(uidBuf));

            if(i > 0) json += ",";

            json += "{";
            json += "\"uid\":\"" + jsonEscape(String(uidBuf)) + "\",";
            json += "\"name\":\"" + jsonEscape(String(registeredBands[i].name)) + "\",";
            json += "\"type\":\"" + jsonEscape(String(registeredBands[i].type)) + "\",";
            json += "\"owner\":\"" + jsonEscape(String(registeredBands[i].owner)) + "\",";
            json += "\"location\":\"" + jsonEscape(String(registeredBands[i].location)) + "\",";
            json += "\"dateBought\":\"" + jsonEscape(String(registeredBands[i].dateBought)) + "\",";
            json += "\"color\":" + String((unsigned int)registeredBands[i].color) + ",";
            json += "\"themeId\":" + String((unsigned int)registeredBands[i].themeId) + ",";

            json += "\"imageUrl\":\"" + jsonEscape(String(registeredBands[i].imageUrl)) + "\",";
            json += "\"mbcListing\":\"" + jsonEscape(String(registeredBands[i].mbcListing)) + "\",";
            json += "\"releaseType\":\"" + jsonEscape(String(registeredBands[i].releaseType)) + "\",";
            json += "\"releaseDate\":\"" + jsonEscape(String(registeredBands[i].releaseDate)) + "\",";
            json += "\"releasedAt\":\"" + jsonEscape(String(registeredBands[i].releasedAt)) + "\",";
            json += "\"bandColorName\":\"" + jsonEscape(String(registeredBands[i].bandColorName)) + "\",";
            json += "\"iconColorName\":\"" + jsonEscape(String(registeredBands[i].iconColorName)) + "\",";
            json += "\"originalPrice\":\"" + jsonEscape(String(registeredBands[i].originalPrice)) + "\",";
            json += "\"sku\":\"" + jsonEscape(String(registeredBands[i].sku)) + "\"";
            json += "}";
        }

        json += "],";

        // Owners/Locations arrays
        json += "\"owners\":[";
        bool first = true;
        for(int i = 0; i < 10; i++) {
            if(ownersList[i][0] == '\0') continue;
            if(!first) json += ",";
            first = false;
            json += "\"" + jsonEscape(String(ownersList[i])) + "\"";
        }
        json += "],";

        json += "\"locations\":[";
        first = true;
        for(int i = 0; i < 10; i++) {
            if(locationsList[i][0] == '\0') continue;
            if(!first) json += ",";
            first = false;
            json += "\"" + jsonEscape(String(locationsList[i])) + "\"";
        }
        json += "],";

        json += "\"settings\":{";
        json += "\"idleTimeout\":" + String((unsigned int)idleTimeout) + ",";
        json += "\"sleepTimeout\":" + String((unsigned int)sleepTimeout);
        json += "},";

        // Custom themes
        json += "\"customThemes\":[";
        for (int i = 0; i < customThemeCount; i++) {
            if (i > 0) json += ",";
            json += "{";
            json += "\"id\":" + String((unsigned int)customThemes[i].id) + ",";
            json += "\"name\":\"" + jsonEscape(String(customThemes[i].name)) + "\",";
            json += "\"phases\":" + String((unsigned int)customThemes[i].phases) + ",";
            json += "\"loop\":" + String((unsigned int)customThemes[i].loopPattern) + ",";
            json += "\"colors\":[";
            for (uint8_t ci = 0; ci < customThemes[i].colorCount && ci < CUSTOM_THEME_MAX_COLORS; ci++) {
                if (ci > 0) json += ",";
                json += String((unsigned int)customThemes[i].colors[ci]);
            }
            json += "],";
            json += "\"audioFile\":\"" + jsonEscape(String(customThemes[i].audioFile)) + "\"";
            json += "}";
        }
        json += "]";  // closes customThemes"

        json += "}";

        AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", json);
        resp->addHeader("Content-Disposition", "attachment; filename=\"magicbandhub-backup.json\"");
        request->send(resp);
    });

    // ROUTE: Import backup JSON overwrites all stored bands/categories/settings
    server.on("/import", HTTP_POST, [](AsyncWebServerRequest *request){
        if(!request->hasParam("data", true)) {
            request->redirect("/?msg=import_fail");
            return;
        }

        String data = request->getParam("data", true)->value();
        data.trim();
        if(data.length() < 20 || data.indexOf("\"bands\"") < 0) {
            request->redirect("/?msg=import_fail");
            return;
        }

        // Parse owners/locations (best-effort)
        String ownersTmp[10];
        String locationsTmp[10];
        extractJsonArrayOfStrings(data, "owners", ownersTmp, 10);
        extractJsonArrayOfStrings(data, "locations", locationsTmp, 10);

        // Parse settings (best-effort)
        int idleMs = (int)idleTimeout;
        int sleepMs = (int)sleepTimeout;
        // settings object may be nested; search in full JSON
        jsonFindValueInt(data, "idleTimeout", idleMs);
        jsonFindValueInt(data, "sleepTimeout", sleepMs);

        // Extract bands array
        int bandsPos = data.indexOf("\"bands\":[");
        if(bandsPos < 0) { request->redirect("/?msg=import_fail"); return; }
        int arrStart = data.indexOf("[", bandsPos);
        int arrEnd = data.indexOf("]", arrStart);
        if(arrStart < 0 || arrEnd < 0 || arrEnd <= arrStart) { request->redirect("/?msg=import_fail"); return; }

        String arr = data.substring(arrStart + 1, arrEnd);

        // Iterate objects: naive scan for {...}
        int idx = 0;
        int imported = 0;
        while(idx < (int)arr.length() && imported < 50) {
            int objStart = arr.indexOf("{", idx);
            if(objStart < 0) break;

            int depth = 0;
            int objEnd = -1;
            for(int p = objStart; p < (int)arr.length(); p++) {
                if(arr[p] == '{') depth++;
                else if(arr[p] == '}') {
                    depth--;
                    if(depth == 0) { objEnd = p; break; }
                }
            }
            if(objEnd < 0) break;

            String obj = arr.substring(objStart, objEnd + 1);

            BandRecord br;
            memset(&br, 0, sizeof(BandRecord));

            String uidStr;
            if(!jsonFindValueString(obj, "uid", uidStr)) { idx = objEnd + 1; continue; }
            if(!parseUidString(uidStr, br.uid, 7)) { idx = objEnd + 1; continue; }

            String v;
            if(jsonFindValueString(obj, "name", v)) {
                strncpy(br.name, v.c_str(), sizeof(br.name) - 1);
                br.name[sizeof(br.name) - 1] = '\0';
            }
            if(jsonFindValueString(obj, "type", v)) {
                strncpy(br.type, v.c_str(), sizeof(br.type) - 1);
                br.type[sizeof(br.type) - 1] = '\0';
            }
            if(jsonFindValueString(obj, "owner", v)) {
                strncpy(br.owner, v.c_str(), sizeof(br.owner) - 1);
                br.owner[sizeof(br.owner) - 1] = '\0';
            }
            if(jsonFindValueString(obj, "location", v)) {
                strncpy(br.location, v.c_str(), sizeof(br.location) - 1);
                br.location[sizeof(br.location) - 1] = '\0';
            }
            if(jsonFindValueString(obj, "dateBought", v) && v.length() == 10) {
                strncpy(br.dateBought, v.c_str(), sizeof(br.dateBought) - 1);
                br.dateBought[sizeof(br.dateBought) - 1] = '\0';
            }

            uint32_t col = 0;
            if(jsonFindValueUInt32(obj, "color", col)) br.color = col;

            int theme = 0;
            if (jsonFindValueInt(obj, "themeId", theme)) {
                if (theme < 0) theme = 0;
                // Built-in IDs are 0..kThemeCount-1; custom IDs are CUSTOM_THEME_ID_BASE+.
                // Accept any non-negative value; invalid ones fall back at runtime.
                br.themeId = (uint16_t)theme;
            } else {
                br.themeId = 0;
            }

            if(jsonFindValueString(obj, "imageUrl", v)) {
                strncpy(br.imageUrl, v.c_str(), sizeof(br.imageUrl) - 1);
                br.imageUrl[sizeof(br.imageUrl) - 1] = '\0';
            }
            if(jsonFindValueString(obj, "mbcListing", v)) {
                strncpy(br.mbcListing, v.c_str(), sizeof(br.mbcListing) - 1);
                br.mbcListing[sizeof(br.mbcListing) - 1] = '\0';
            }
            if(jsonFindValueString(obj, "releaseType", v)) {
                strncpy(br.releaseType, v.c_str(), sizeof(br.releaseType) - 1);
                br.releaseType[sizeof(br.releaseType) - 1] = '\0';
            }
            if(jsonFindValueString(obj, "releaseDate", v)) {
                strncpy(br.releaseDate, v.c_str(), sizeof(br.releaseDate) - 1);
                br.releaseDate[sizeof(br.releaseDate) - 1] = '\0';
            }
            if(jsonFindValueString(obj, "releasedAt", v)) {
                strncpy(br.releasedAt, v.c_str(), sizeof(br.releasedAt) - 1);
                br.releasedAt[sizeof(br.releasedAt) - 1] = '\0';
            }
            if(jsonFindValueString(obj, "bandColorName", v)) {
                strncpy(br.bandColorName, v.c_str(), sizeof(br.bandColorName) - 1);
                br.bandColorName[sizeof(br.bandColorName) - 1] = '\0';
            }
            if(jsonFindValueString(obj, "iconColorName", v)) {
                strncpy(br.iconColorName, v.c_str(), sizeof(br.iconColorName) - 1);
                br.iconColorName[sizeof(br.iconColorName) - 1] = '\0';
            }
            if(jsonFindValueString(obj, "originalPrice", v)) {
                strncpy(br.originalPrice, v.c_str(), sizeof(br.originalPrice) - 1);
                br.originalPrice[sizeof(br.originalPrice) - 1] = '\0';
            }
            if(jsonFindValueString(obj, "sku", v)) {
                strncpy(br.sku, v.c_str(), sizeof(br.sku) - 1);
                br.sku[sizeof(br.sku) - 1] = '\0';
            }

            // Store in RAM
            registeredBands[imported] = br;
            imported++;

            idx = objEnd + 1;
        }

        if(imported <= 0) {
            request->redirect("/?msg=import_fail");
            return;
        }

        bandCount = imported;

        // Persist bands + categories + settings
        prefs.begin("mbands", false);
        prefs.clear();
        prefs.putInt("count", bandCount);
        for(int i = 0; i < bandCount; i++) {
            prefs.putBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
        }

        // Write owners/locations into o0.. / l0.. keys (compact)
        for(int i = 0; i < 10; i++) {
            String ok = "o" + String(i);
            String lk = "l" + String(i);
            if(ownersTmp[i].length() > 0) prefs.putString(ok.c_str(), ownersTmp[i]);
            if(locationsTmp[i].length() > 0) prefs.putString(lk.c_str(), locationsTmp[i]);
        }
        prefs.end();

        idleTimeout = (uint32_t)idleMs;
        sleepTimeout = (uint32_t)sleepMs;
        prefs.begin("settings", false);
        prefs.putUInt("idle", idleTimeout);
        prefs.putUInt("sleep", sleepTimeout);
        prefs.end();

        // Parse custom themes (best-effort)
        {
            String ctNeedle = "\"customThemes\":[";
            int ctPos = data.indexOf(ctNeedle);
            if (ctPos >= 0) {
                int ctStart = data.indexOf("[", ctPos);
                int ctEnd   = data.indexOf("]", ctStart);
                if (ctStart >= 0 && ctEnd > ctStart) {
                    String ctArr = data.substring(ctStart + 1, ctEnd);
                    int ci = 0; int ctImported = 0;
                    while (ci < (int)ctArr.length() && ctImported < CUSTOM_THEME_MAX) {
                        int os = ctArr.indexOf("{", ci);
                        if (os < 0) break;
                        int depth = 0; int oe = -1;
                        for (int p = os; p < (int)ctArr.length(); p++) {
                            if (ctArr[p] == '{') depth++;
                            else if (ctArr[p] == '}') { depth--; if (depth == 0) { oe = p; break; } }
                        }
                        if (oe < 0) break;
                        String cobj = ctArr.substring(os, oe + 1);
                        CustomTheme ct; memset(&ct, 0, sizeof(ct));
                        int ctid = CUSTOM_THEME_ID_BASE + ctImported;
                        jsonFindValueInt(cobj, "id", ctid);
                        ct.id = (uint16_t)ctid;
                        String sv;
                        if (jsonFindValueString(cobj, "name", sv)) strncpy(ct.name, sv.c_str(), sizeof(ct.name) - 1);
                        int pv = 0; jsonFindValueInt(cobj, "phases", pv); ct.phases = (uint8_t)pv;
                        int lv = 0; jsonFindValueInt(cobj, "loop",   lv); ct.loopPattern = (uint8_t)lv;
                        // Parse colors array
                        {
                            int cArr = cobj.indexOf("\"colors\":");
                            if (cArr >= 0) {
                                int cb = cobj.indexOf("[", cArr);
                                int ce = cobj.indexOf("]", cb);
                                if (cb >= 0 && ce > cb) {
                                    String ca = cobj.substring(cb + 1, ce);
                                    uint8_t nc = 0;
                                    int ci2 = 0;
                                    while (ci2 < (int)ca.length() && nc < CUSTOM_THEME_MAX_COLORS) {
                                        while (ci2 < (int)ca.length() && (ca[ci2] == ' ' || ca[ci2] == ',')) ci2++;
                                        if (ci2 >= (int)ca.length()) break;
                                        int end2 = ci2;
                                        while (end2 < (int)ca.length() && ca[end2] != ',' && ca[end2] != ']') end2++;
                                        String tok = ca.substring(ci2, end2);
                                        tok.trim();
                                        if (tok.length() > 0) ct.colors[nc++] = (uint32_t)tok.toInt();
                                        ci2 = end2;
                                    }
                                    ct.colorCount = nc > 0 ? nc : 1;
                                }
                            }
                        }
                        if (jsonFindValueString(cobj, "audioFile", sv)) strncpy(ct.audioFile, sv.c_str(), sizeof(ct.audioFile) - 1);
                        customThemes[ctImported++] = ct;
                        ci = oe + 1;
                    }
                    customThemeCount = ctImported;
                    saveCustomThemesToPrefs();
                }
            }
        }

        loadCategoriesFromPrefs();
        fn_refresh_roller(NULL);

        request->redirect("/?msg=import_ok");
    });

    // -------------------------------------------------------------------------
    // ROUTE: /sd_status — reports whether SD card is mounted
    // -------------------------------------------------------------------------
    server.on("/sd_status", HTTP_GET, [](AsyncWebServerRequest *request){
        bool ready = isSDReady && (SD_MMC.cardType() != CARD_NONE);
        request->send(200, "application/json", ready ? "{\"ready\":true}" : "{\"ready\":false}");
    });

    // -------------------------------------------------------------------------
    // ROUTE: /upload_audio  POST multipart — saves audio file to SD root
    // -------------------------------------------------------------------------
    server.on("/upload_audio", HTTP_POST,
        [](AsyncWebServerRequest *request) {
            extern File   _sdUploadFile;
            extern bool   _sdUploadError;
            extern String _sdUploadName;
            if (_sdUploadFile) _sdUploadFile.close();
            if (_sdUploadError) {
                request->send(400, "application/json", "{\"ok\":false,\"error\":\"Upload failed or bad file type\"}");
            } else {
                request->send(200, "application/json", "{\"ok\":true,\"name\":\"" + jsonEscape(_sdUploadName) + "\"}");
            }
        },
        [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
            extern File   _sdUploadFile;
            extern bool   _sdUploadError;
            extern String _sdUploadName;
            if (index == 0) {
                _sdUploadError = false;
                if (_sdUploadFile) _sdUploadFile.close();
                // Sanitise: strip path, validate extension
                String safe = filename;
                int sl = safe.lastIndexOf('/');
                if (sl >= 0) safe = safe.substring(sl + 1);
                safe.replace("..", "");
                String lower = safe; lower.toLowerCase();
                bool goodExt = lower.endsWith(".mp3") || lower.endsWith(".wav") ||
                               lower.endsWith(".aac") || lower.endsWith(".flac");
                if (!goodExt || safe.length() == 0 || !isSDReady) {
                    _sdUploadError = true;
                    Serial.println("Audio upload rejected: bad extension or no SD.");
                    return;
                }
                _sdUploadName = safe;
                String path = "/sdcard/" + safe;
                _sdUploadFile = SD_MMC.open(path.c_str(), FILE_WRITE);
                if (!_sdUploadFile) {
                    _sdUploadError = true;
                    Serial.printf("Audio upload: cannot open %s\n", path.c_str());
                    return;
                }
                Serial.printf("Audio upload started: %s\n", path.c_str());
            }
            if (!_sdUploadError && _sdUploadFile && len > 0) {
                _sdUploadFile.write(data, len);
            }
            if (final && _sdUploadFile) {
                _sdUploadFile.close();
                Serial.println("Audio upload complete.");
            }
        }
    );

    // -------------------------------------------------------------------------
    // ROUTE: /sd_files — returns JSON array of audio files at SD root
    // -------------------------------------------------------------------------
    server.on("/sd_files", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "[";
        bool first = true;

        // SD_MMC is already mounted by main.cpp; just open root.
        File root = SD_MMC.open("/");
        if (root) {
            File f = root.openNextFile();
            while (f) {
                if (!f.isDirectory()) {
                    String fname = String(f.name());
                    // Keep only audio files
                    fname.toLowerCase();
                    if (fname.endsWith(".mp3") || fname.endsWith(".wav") || fname.endsWith(".aac") || fname.endsWith(".flac")) {
                        // Get just the basename
                        int sl = fname.lastIndexOf('/');
                        String base = (sl >= 0) ? String(f.name()).substring(sl + 1) : String(f.name());
                        // Skip macOS metadata files
                        if (base.startsWith("._")) { f = root.openNextFile(); continue; }
                        if (!first) json += ",";
                        first = false;
                        json += "\"" + jsonEscape(base) + "\"";
                    }
                }
                f = root.openNextFile();
            }
            root.close();
        }

        json += "]";
        request->send(200, "application/json", json);
    });

    // -------------------------------------------------------------------------
    // ROUTE: /themes — Theme Builder page (create + edit)
    // -------------------------------------------------------------------------
    server.on("/themes", HTTP_GET, [](AsyncWebServerRequest *request){
        bool isEdit = false;
        int editIdx = -1;
        if (request->hasParam("edit")) {
            uint16_t eid = (uint16_t)request->getParam("edit")->value().toInt();
            editIdx = findCustomTheme(eid);
            isEdit = (editIdx >= 0);
        }

        String html;
        html.reserve(9000);

        html += "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
        html += "<style>";
        html += "body{font-family:sans-serif;text-align:center;background:#3677A3;padding:20px;}";
        html += "h1{color:#fff;font-weight:800;margin:10px 0 18px 0;}";
        html += ".card{background:white;border-radius:15px;padding:16px;margin:10px auto;max-width:440px;box-shadow:2px 2px 10px rgba(11,18,108,0.6);}";
        html += ".field{margin-bottom:12px;text-align:left;}";
        html += ".field label{display:block;font-size:0.85em;color:#555;margin-bottom:4px;font-weight:600;}";
        html += ".field input[type=text],.field select{width:100%;padding:8px;border-radius:6px;border:1px solid #ccc;box-sizing:border-box;font-size:15px;}";
        html += ".btn-save{background:#2ed573;color:white;border:none;padding:12px;width:100%;border-radius:8px;cursor:pointer;font-size:16px;box-sizing:border-box;margin-top:6px;}";
        html += ".btn-del{background:#ff4757;color:white;border:none;padding:5px 10px;border-radius:5px;font-size:0.82em;cursor:pointer;}";
        html += ".btn-edit{background:#3b4ce2;color:white;border:none;padding:5px 10px;border-radius:5px;font-size:0.82em;cursor:pointer;text-decoration:none;display:inline-block;margin-right:6px;}";
        html += ".btn-back{background:#eef1ff;color:#3b4ce2;border:none;padding:10px 16px;border-radius:8px;cursor:pointer;font-size:15px;text-decoration:none;display:inline-block;margin-bottom:14px;}";
        html += ".btn-addcol{background:#eef1ff;color:#3b4ce2;border:1px solid #b0b9f5;padding:7px 14px;border-radius:6px;cursor:pointer;font-size:0.88em;margin-top:4px;}";
        html += ".theme-row{display:flex;align-items:center;justify-content:space-between;padding:10px 0;border-bottom:1px solid #eee;}";
        html += ".prev{width:20px;height:20px;border-radius:50%;border:2px solid #ccc;display:inline-block;vertical-align:middle;margin-right:3px;}";
        html += ".sec{font-size:0.8em;font-weight:700;color:#888;text-transform:uppercase;letter-spacing:0.05em;margin:12px 0 6px 0;text-align:left;}";
        html += ".cr{display:flex;gap:8px;align-items:center;margin-bottom:6px;}";
        html += ".cr input[type=color]{width:42px;height:34px;padding:2px;border-radius:6px;border:1px solid #ccc;cursor:pointer;flex-shrink:0;}";
        html += ".cr input[type=text]{flex:1;}";
        html += ".cbr{display:flex;gap:14px;flex-wrap:wrap;margin-top:4px;}";
        html += ".cbr label{display:flex;align-items:center;gap:5px;font-size:0.93em;font-weight:500;color:#333;cursor:pointer;}";
        html += ".cbr input[type=checkbox]{width:16px;height:16px;}";
        html += "</style>";

        // ---- JS ----
        html += "<script>";
        html += "var sdFiles=[];";
        html += "window.addEventListener('load',function(){";
        html += "  fetch('/sd_files').then(function(r){return r.json();}).then(function(f){";
        html += "    sdFiles=f; var sel=document.getElementById('audioFile');";
        html += "    if(sel){var cur=sel.getAttribute('data-current')||'';fillAudio(sel,cur);}";
        html += "  }).catch(function(){});";
        html += "  initSync();";
        html += "});";
        html += "function fillAudio(sel,cur){while(sel.options.length)sel.remove(0);";
        html += "  var o=document.createElement('option');o.value='';o.text='-- None --';sel.appendChild(o);";
        html += "  sdFiles.forEach(function(f){var o=document.createElement('option');o.value=f;o.text=f;if(f===cur)o.selected=true;sel.appendChild(o);});";
        html += "}";
        html += "function initSync(){";
        html += "  for(var i=0;i<5;i++){(function(n){";
        html += "    var cp=document.getElementById('cp'+n),ch=document.getElementById('ch'+n);";
        html += "    if(cp&&ch){";
        html += "      cp.addEventListener('input',function(){ch.value=cp.value;});";
        html += "      ch.addEventListener('change',function(){var v=ch.value.trim();if(v&&v[0]!='#')v='#'+v;cp.value=v;});";
        html += "    }";
        html += "  })(i);}";
        html += "}";
        html += "function addCol(){";
        html += "  for(var i=1;i<5;i++){var r=document.getElementById('cr'+i);if(r&&r.style.display==='none'){r.style.display='flex';return;}}";
        html += "}";
        html += "function remCol(i){";
        html += "  var r=document.getElementById('cr'+i);if(r)r.style.display='none';";
        html += "  var cp=document.getElementById('cp'+i),ch=document.getElementById('ch'+i);";
        html += "  if(cp)cp.value='#000000';if(ch)ch.value='';";
        html += "}";
        html += "function onSub(){";
        html += "  var ph=0;";
        html += "  if(document.getElementById('ph_c')&&document.getElementById('ph_c').checked)ph|=1;";
        html += "  if(document.getElementById('ph_f')&&document.getElementById('ph_f').checked)ph|=2;";
        html += "  if(document.getElementById('ph_p')&&document.getElementById('ph_p').checked)ph|=4;";
        html += "  document.getElementById('phases_val').value=ph;";
        html += "  var vals=[];";
        html += "  for(var i=0;i<5;i++){var r=document.getElementById('cr'+i),ch=document.getElementById('ch'+i);";
        html += "    if(r&&r.style.display!=='none'&&ch&&ch.value.trim())vals.push(ch.value.trim());}";
        html += "  if(!vals.length)vals.push('#00FF00');";
        html += "  for(var j=0;j<5;j++){var h=document.getElementById('hc'+j);if(h)h.value=j<vals.length?vals[j]:'';}";
        html += "  document.getElementById('cc_val').value=vals.length;";
        html += "  return true;";
        html += "}";
        // Upload functions
        html += "function uploadReady(){";
        html += "  var f=document.getElementById('audioUpload').files[0];";
        html += "  document.getElementById('uploadBtn').disabled=!f;";
        html += "}";
        html += "function doUpload(){";
        html += "  var f=document.getElementById('audioUpload').files[0];";
        html += "  if(!f) return;";
        html += "  var btn=document.getElementById('uploadBtn');";
        html += "  var st=document.getElementById('uploadStatus');";
        html += "  btn.disabled=true; btn.value='Uploading\u2026'; st.textContent='';";
        html += "  var fd=new FormData(); fd.append('file',f,f.name);";
        html += "  fetch('/upload_audio',{method:'POST',body:fd})";
        html += "    .then(function(r){return r.json();})";
        html += "    .then(function(j){";
        html += "      if(j.ok){";
        html += "        st.textContent='\u2713 Uploaded: '+j.name;";
        html += "        fetch('/sd_files').then(function(r){return r.json();}).then(function(files){";
        html += "          sdFiles=files;";
        html += "          var sel=document.getElementById('audioFile');";
        html += "          if(sel) fillAudio(sel,j.name);";
        html += "        });";
        html += "      } else { st.textContent='Error: '+(j.error||'Upload failed'); }";
        html += "      btn.disabled=false; btn.value='Upload to SD';";
        html += "    }).catch(function(){ st.textContent='Upload error.'; btn.disabled=false; btn.value='Upload to SD'; });";
        html += "}";
        // Check SD on load and show upload card accordingly
        html += "window.addEventListener('load',function(){";
        html += "  fetch('/sd_status').then(function(r){return r.json();}).then(function(s){";
        html += "    document.getElementById(s.ready?'upload_card':'nosd_card').style.display='';";
        html += "  }).catch(function(){ document.getElementById('nosd_card').style.display=''; });";
        html += "});";
        html += "</script></head><body>";

        html += "<a class='btn-back' href='/'>&#8592; Back to Hub</a>";
        html += "<h1>Theme Builder</h1>";

        // ---- Saved themes list ----
        if (customThemeCount > 0) {
            html += "<div class='card'><div class='sec'>Saved Custom Themes</div>";
            for (int i = 0; i < customThemeCount; i++) {
                html += "<div class='theme-row'><div style='text-align:left;'>";
                // colour swatches
                for (uint8_t ci = 0; ci < customThemes[i].colorCount && ci < CUSTOM_THEME_MAX_COLORS; ci++) {
                    char sw[8]; snprintf(sw, sizeof(sw), "#%06X", (unsigned int)customThemes[i].colors[ci]);
                    html += "<span class='prev' style='background:" + String(sw) + ";'></span>";
                }
                html += "<b>" + htmlEscape(String(customThemes[i].name)) + "</b><br>";
                html += "<span style='font-size:0.8em;color:#666;'>" + customThemeSummary(customThemes[i]);
                if (strlen(customThemes[i].audioFile))
                    html += " &nbsp;&bull;&nbsp; " + htmlEscape(String(customThemes[i].audioFile));
                html += "</span></div>";
                html += "<div style='display:flex;gap:6px;align-items:center;'>";
                html += "<a class='btn-edit' href='/themes?edit=" + String((unsigned int)customThemes[i].id) + "'>Edit</a>";
                html += "<form action='/theme_delete' method='GET' onsubmit='return confirm(\"Delete theme?\")'>";
                html += "<input type='hidden' name='id' value='" + String((unsigned int)customThemes[i].id) + "'>";
                html += "<button type='submit' class='btn-del'>Delete</button></form>";
                html += "</div></div>";
            }
            html += "</div>";
        }

        // ---- Create / Edit form ----
        bool showForm = isEdit || (customThemeCount < CUSTOM_THEME_MAX);
        if (showForm) {
            html += "<div class='card'>";
            html += "<div class='sec'>" + String(isEdit ? "Edit Theme" : "Create New Theme") + "</div>";
            html += "<form action='/theme_save' method='GET' onsubmit='return onSub()'>";

            if (isEdit)
                html += "<input type='hidden' name='id' value='" + String((unsigned int)customThemes[editIdx].id) + "'>";

            // Hidden fields filled by JS on submit
            html += "<input type='hidden' id='phases_val' name='phases' value='0'>";
            for (int j = 0; j < 5; j++)
                html += "<input type='hidden' id='hc" + String(j) + "' name='hc" + String(j) + "' value=''>";
            html += "<input type='hidden' id='cc_val' name='colorCount' value='1'>";

            // Name
            String nameVal = isEdit ? htmlEscape(String(customThemes[editIdx].name)) : String("");
            html += "<div class='field'><label>Theme Name</label>";
            html += "<input type='text' name='name' maxlength='31' required placeholder='e.g. Haunted Mansion' value='" + nameVal + "'></div>";

            // ---- Phases (checkboxes) ----
            uint8_t defPhases = isEdit ? customThemes[editIdx].phases : (CTP_PHASE_COMET | CTP_PHASE_FILL | CTP_PHASE_PULSE);
            html += "<div class='field'><label>Opening Phases</label><div class='cbr'>";
            html += "<label><input type='checkbox' id='ph_c'" + String((defPhases & CTP_PHASE_COMET) ? " checked" : "") + "> Comet Trail</label>";
            html += "<label><input type='checkbox' id='ph_f'" + String((defPhases & CTP_PHASE_FILL)  ? " checked" : "") + "> Fill</label>";
            html += "<label><input type='checkbox' id='ph_p'" + String((defPhases & CTP_PHASE_PULSE) ? " checked" : "") + "> Pulse</label>";
            html += "</div></div>";

            // ---- Loop pattern dropdown ----
            uint8_t defLoop = isEdit ? customThemes[editIdx].loopPattern : CTL_SPINNING_COMET;
            html += "<div class='field'><label>Ongoing LED Pattern</label><select name='loop'>";
            const char* loopNames[CUSTOM_THEME_LOOP_COUNT] = {"None (stop after opening)", "Spinning Comet", "Gentle Pulse", "Rainbow Spin"};
            for (uint8_t lp = 0; lp < CUSTOM_THEME_LOOP_COUNT; lp++) {
                html += "<option value='" + String(lp) + "'" + String(defLoop == lp ? " selected" : "") + ">" + String(loopNames[lp]) + "</option>";
            }
            html += "</select></div>";

            // ---- Colours ----
            html += "<div class='field'><label>Colours (first is required)</label>";
            uint8_t numColors = isEdit ? customThemes[editIdx].colorCount : 1;
            if (numColors < 1) numColors = 1;
            for (int ci = 0; ci < 5; ci++) {
                char hexBuf[8];
                if (isEdit && ci < customThemes[editIdx].colorCount)
                    snprintf(hexBuf, sizeof(hexBuf), "#%06X", (unsigned int)customThemes[editIdx].colors[ci]);
                else
                    snprintf(hexBuf, sizeof(hexBuf), "#%s", ci == 0 ? "00FF00" : "0088FF");
                bool visible = (ci == 0) || (isEdit && ci < (int)numColors);
                html += "<div class='cr' id='cr" + String(ci) + "' style='display:" + String(visible ? "flex" : "none") + ";'>";
                html += "<input type='color' id='cp" + String(ci) + "' value='" + String(hexBuf) + "'>";
                html += "<input type='text'  id='ch" + String(ci) + "' value='" + String(visible ? hexBuf : "") + "' placeholder='#RRGGBB'>";
                if (ci > 0)
                    html += "<button type='button' class='btn-del' style='padding:4px 8px;' onclick='remCol(" + String(ci) + ")'>&#10005;</button>";
                html += "</div>";
            }
            html += "<button type='button' class='btn-addcol' onclick='addCol()'>+ Add Colour</button></div>";

            // ---- Audio file ----
            String currentAudio = isEdit ? String(customThemes[editIdx].audioFile) : String("");
            html += "<div class='field'><label>Audio File (SD Card Root)</label>";
            html += "<select id='audioFile' name='audioFile' data-current='" + htmlEscape(currentAudio) + "'>";
            html += "<option value=''>-- Loading... --</option></select></div>";

            html += "<input type='submit' class='btn-save' value='" + String(isEdit ? "Update Theme" : "Save Theme") + "'>";
            html += "</form></div>";
        } else {
            html += "<div class='card' style='color:#856404;background:#fff3cd;'>Maximum of ";
            html += String(CUSTOM_THEME_MAX) + " custom themes reached. Delete one to add more.</div>";
        }

        // ---- SD audio upload card (visibility set by JS /sd_status check) ----
        html += "<div id='upload_card' class='card' style='display:none;'>";
        html += "<div class='sec'>Upload Audio to SD Card</div>";
        html += "<div class='field'><label>File (.mp3, .wav, .aac, .flac)</label>";
        html += "<input type='file' id='audioUpload' accept='.mp3,.wav,.aac,.flac' onchange='uploadReady()'></div>";
        html += "<input type='button' id='uploadBtn' class='btn-save' value='Upload to SD' onclick='doUpload()' disabled>";
        html += "<div id='uploadStatus' style='margin-top:8px;font-size:0.85em;color:#333;text-align:left;'></div>";
        html += "</div>";
        html += "<div id='nosd_card' class='card' style='display:none;color:#856404;background:#fff3cd;'>";
        html += "&#9888;&#65039; Insert an SD card to upload audio files.";
        html += "</div>";

        html += "</body></html>";
        request->send(200, "text/html; charset=utf-8", html);
    });

    // -------------------------------------------------------------------------
    // ROUTE: /theme_save — create or update a custom theme
    // -------------------------------------------------------------------------
    server.on("/theme_save", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("name")) {
            request->redirect("/themes");
            return;
        }

        bool isEdit = request->hasParam("id");
        int editIdx = -1;
        if (isEdit) {
            uint16_t eid = (uint16_t)request->getParam("id")->value().toInt();
            editIdx = findCustomTheme(eid);
            if (editIdx < 0) isEdit = false;
        }

        if (!isEdit && customThemeCount >= CUSTOM_THEME_MAX) {
            request->redirect("/themes");
            return;
        }

        CustomTheme ct;
        memset(&ct, 0, sizeof(ct));

        ct.id = isEdit ? customThemes[editIdx].id
                       : (uint16_t)(CUSTOM_THEME_ID_BASE + customThemeCount);

        String nameVal = request->getParam("name")->value();
        nameVal.trim();
        strncpy(ct.name, nameVal.c_str(), sizeof(ct.name) - 1);

        ct.phases = request->hasParam("phases")
            ? (uint8_t)constrain(request->getParam("phases")->value().toInt(), 0, 7)
            : (CTP_PHASE_COMET | CTP_PHASE_FILL | CTP_PHASE_PULSE);

        ct.loopPattern = request->hasParam("loop")
            ? (uint8_t)constrain(request->getParam("loop")->value().toInt(), 0, CUSTOM_THEME_LOOP_COUNT - 1)
            : CTL_SPINNING_COMET;

        // Parse up to 5 colours submitted as hc0..hc4
        ct.colorCount = 0;
        int reqColorCount = request->hasParam("colorCount")
            ? constrain(request->getParam("colorCount")->value().toInt(), 1, CUSTOM_THEME_MAX_COLORS)
            : 1;
        for (int ci = 0; ci < 5 && ct.colorCount < CUSTOM_THEME_MAX_COLORS; ci++) {
            String key = "hc" + String(ci);
            if (request->hasParam(key)) {
                String hexVal = request->getParam(key)->value();
                hexVal.trim();
                if (hexVal.length() > 0 && ct.colorCount < (uint8_t)reqColorCount) {
                    ct.colors[ct.colorCount++] = hexToUint(hexVal);
                }
            }
        }
        if (ct.colorCount == 0) { ct.colors[0] = 0x00FF00; ct.colorCount = 1; }

        if (request->hasParam("audioFile")) {
            String af = request->getParam("audioFile")->value();
            af.trim();
            strncpy(ct.audioFile, af.c_str(), sizeof(ct.audioFile) - 1);
        }

        if (isEdit) {
            customThemes[editIdx] = ct;
        } else {
            customThemes[customThemeCount++] = ct;
        }
        saveCustomThemesToPrefs();

        request->redirect("/themes");
    });

    // -------------------------------------------------------------------------
    // ROUTE: /theme_delete — remove a custom theme by id
    // -------------------------------------------------------------------------
    server.on("/theme_delete", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!request->hasParam("id")) { request->redirect("/themes"); return; }

        uint16_t delId = (uint16_t)request->getParam("id")->value().toInt();
        int idx = findCustomTheme(delId);
        if (idx < 0) { request->redirect("/themes"); return; }

        // Shift array down
        for (int i = idx; i < customThemeCount - 1; i++) {
            customThemes[i] = customThemes[i + 1];
        }
        customThemeCount--;

        // Reassign IDs sequentially so they stay contiguous
        for (int i = 0; i < customThemeCount; i++) {
            customThemes[i].id = (uint16_t)(CUSTOM_THEME_ID_BASE + i);
        }

        // Fix any bands that referenced the deleted or shifted IDs
        for (int b = 0; b < bandCount; b++) {
            if (registeredBands[b].themeId >= CUSTOM_THEME_ID_BASE) {
                if (findCustomTheme(registeredBands[b].themeId) < 0) {
                    registeredBands[b].themeId = 0; // reset to Default
                }
            }
        }

        saveCustomThemesToPrefs();

        // Re-save bands (IDs may have shifted)
        prefs.begin("mbands", false);
        prefs.putInt("count", bandCount);
        for (int i = 0; i < bandCount; i++) {
            prefs.putBytes(("b" + String(i)).c_str(), &registeredBands[i], sizeof(BandRecord));
        }
        prefs.end();

        request->redirect("/themes");
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
    while (WiFi.status() != WL_CONNECTED && attempt < 30) {
        delay(500);
        attempt++;
#if SCREEN_ENABLED
        lv_timer_handler();
#endif
    }
    return (WiFi.status() == WL_CONNECTED);
}

extern "C" {
    void startWebServer() {
        server.begin();

        // mDNS only works once WiFi is actually connected.
        if (!MDNS.begin("magicband")) {
            Serial.println("[MDNS] begin failed");
            return;
        }

        // Advertise HTTP so browsers/resolvers discover it as a web server.
        MDNS.addService("http", "tcp", 80);
        Serial.println("[MDNS] http://magicband.local/");
    }

    void stopWebServer() {
        server.end();
        MDNS.end();
    }
}
    