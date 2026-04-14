#ifndef SCREEN_ENABLED
#define SCREEN_ENABLED 0
#endif
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "web_portal.h"
#include "Audio_ES8311.h"
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

// Player state (defined in main.cpp) — struct and externs are in web_portal.h

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
        case CTL_LS_RAINBOW:     return "🌈 Rainbow";
        case CTL_LS_PULSE:       return "✨ Pulse";
        case CTL_LS_COLOUR_CYCLE:return "🎠 Colour Cycle";
        case CTL_LS_MAIN_STREET: return "🏠 Main Street";
        case CTL_LS_ADVENTURE:   return "🌿 Adventureland";
        case CTL_LS_FRONTIER:    return "🔥 Frontierland";
        case CTL_LS_LIBERTY:     return "🇬🇧 Liberty Square";
        case CTL_LS_FANTASY:     return "🦄 Fantasyland";
        case CTL_LS_TOMORROW:    return "🚀 Tomorrowland";
        case CTL_LS_HAUNTED:     return "👻 Haunted Mansion";
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
        html += "<style>";
        html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;text-align:center;background:linear-gradient(160deg,#1a237e 0%,#1565c0 60%,#0288d1 100%);min-height:100vh;padding:20px;margin:0;}";
        html += "h1{color:#fff;font-weight:900;margin:10px 0 4px 0;letter-spacing:-0.5px;text-shadow:0 2px 8px rgba(0,0,0,0.25);}";
        html += ".card{background:#fff;border-radius:18px;padding:18px;margin:12px auto;max-width:100%;box-shadow:0 4px 20px rgba(10,30,90,0.18);}";
        html += ".container{max-width:820px;margin:0 auto;}";
        html += "@media(min-width:700px){body{padding:30px 40px;} h1{font-size:2.4em;} .card{padding:24px;}}";
        html += ".summary{display:flex;align-items:center;justify-content:space-between;gap:10px;cursor:pointer;}";
        html += ".summary-left{display:flex;align-items:center;gap:12px;text-align:left;}";
        html += ".thumb{width:110px;height:180px;object-fit:contain;object-position:top center;border-radius:12px;background:#f4f6ff;box-sizing:border-box;padding:10px;display:block;border:1px solid #e0e4ff;}";
        html += ".settings-header{cursor:pointer;font-weight:700;padding:10px;color:#1a237e;font-size:0.95em;} .settings-body{display:none;text-align:left;}";
        html += ".meta{color:#888;font-size:0.82em;line-height:1.4;}";
        html += ".status{margin-top:10px;padding:10px 14px;border-radius:10px;font-weight:700;background:#fff8e1;color:#e65100;border:1px solid #ffe0b2;}";
        html += ".details{margin-top:14px;text-align:left;}";
        html += "input,select{margin:0;padding:9px 10px;border-radius:8px;border:1.5px solid #dde;box-sizing:border-box;max-width:100%;font-size:15px;transition:border-color 0.2s;}";
        html += "input:focus,select:focus{outline:none;border-color:#5765f2;}";
        html += ".btn-save{background:linear-gradient(135deg,#00b894,#00cec9);color:white;border:none;padding:12px;width:80%;border-radius:10px;cursor:pointer;font-size:16px;box-sizing:border-box;font-weight:700;}";
        html += ".btn-del{background:#ff4757;color:white;border:none;padding:5px 12px;border-radius:8px;font-size:0.82em;display:inline-block;text-decoration:none;cursor:pointer;}";
        html += ".btn-del:active{opacity:0.85;}";
        html += ".section{margin-top:20px;padding-top:14px;border-top:1px solid #f0f4ff;}";
        html += ".section-title{font-size:0.75em;font-weight:800;color:#5765f2;margin-bottom:10px;text-transform:uppercase;letter-spacing:0.08em;}";
        html += ".field{margin-bottom:14px;}";
        html += ".field label{display:block;font-size:0.8em;color:#555;margin-bottom:5px;font-weight:600;}";
        html += ".field input,.field select{width:100%;max-width:100%;display:block;}";
        html += "input[type=date]{width:170px;}";
        html += ".btn-secondary{background:#f0f2ff;color:#3b4ce2;border:1.5px solid #c5caf5;padding:10px 12px;width:100%;border-radius:10px;cursor:pointer;margin-top:6px;font-size:15px;box-sizing:border-box;display:block;font-weight:600;transition:background 0.15s;}";
        html += ".btn-secondary:hover{background:#e0e4ff;}";
        html += ".scan-btn{width:auto;padding:10px 18px;margin:0;font-size:15px;font-weight:600;}";
        html += ".scan-cancel{background:#ff4757;color:#fff;border:none;border-radius:10px;cursor:pointer;}";
        html += ".view-toggle{display:flex;gap:8px;justify-content:flex-end;margin-bottom:6px;}";
        html += ".view-toggle button{background:#fff;border:2px solid #e0e4ff;color:#5765f2;border-radius:10px;padding:7px 16px;cursor:pointer;font-size:13px;font-weight:700;transition:all 0.15s;}";
        html += ".view-toggle button.active,.view-toggle button:hover{background:#5765f2;color:#fff;border-color:#5765f2;}";
        html += "#tileView{display:none;}";
        html += ".tile-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:14px;padding:4px 0;}";
        html += ".tile{background:#f8f9ff;border-radius:14px;padding:12px;cursor:pointer;box-shadow:0 2px 8px rgba(30,50,150,0.08);transition:transform 0.12s,box-shadow 0.12s;text-align:center;border:2px solid #eef0ff;}";
        html += ".tile:hover{transform:translateY(-3px);box-shadow:0 6px 18px rgba(30,50,150,0.16);border-color:#5765f2;}";
        html += ".tile img{width:100%;max-height:150px;object-fit:contain;border-radius:10px;background:#fff;padding:6px;box-sizing:border-box;border:1px solid #eee;}";
        html += ".tile .tile-placeholder{width:100%;height:120px;background:#e8ebff;border-radius:10px;display:flex;align-items:center;justify-content:center;color:#a0a8e0;font-size:2.2em;}";
        html += ".tile-name{font-weight:800;font-size:0.84em;margin-top:8px;color:#1a237e;word-break:break-word;line-height:1.3;}";
        html += ".tile-meta{font-size:0.72em;color:#999;margin-top:3px;}";
        html += ".modal-overlay{display:none;position:fixed;inset:0;background:rgba(10,20,60,0.6);z-index:100;align-items:center;justify-content:center;}";
        html += ".modal-overlay.open{display:flex;}";
        html += ".modal{background:#fff;border-radius:20px;padding:24px;max-width:460px;width:92%;max-height:88vh;overflow-y:auto;position:relative;text-align:left;box-shadow:0 8px 40px rgba(10,30,100,0.25);}";
        html += ".modal-img{width:150px;display:block;margin:0 auto 16px;border-radius:12px;background:#f4f6ff;padding:8px;box-sizing:border-box;border:1px solid #e0e4ff;}";
        html += ".modal-close{position:absolute;top:14px;right:16px;background:#f0f2ff;border:none;width:32px;height:32px;border-radius:50%;font-size:1em;cursor:pointer;color:#3b4ce2;}";
        html += ".modal h2{color:#1a237e;margin:0 0 14px;}";
        html += ".meta-row{display:flex;gap:8px;margin-bottom:7px;font-size:0.88em;align-items:baseline;}";
        html += ".meta-row b{min-width:120px;color:#5765f2;font-size:0.82em;text-transform:uppercase;letter-spacing:0.04em;}";
        html += "</style>";
        
        // SEARCH FILTER SCRIPT
        html += "<script>function filterBands() { var val = document.getElementById('search').value.toLowerCase();";
        html += "var owner = document.getElementById('fOwner').value;";
        html += "var loc   = document.getElementById('fLoc').value;";
        html += "var cards = document.getElementsByClassName('band-card');";
        html += "for (var i=0; i<cards.length; i++) { var txt = cards[i].innerText.toLowerCase();";
        html += "var od = cards[i].getAttribute('data-owner')||'';";
        html += "var ld = cards[i].getAttribute('data-loc')||'';";
        html += "var okTxt   = !val   || txt.includes(val);";
        html += "var okOwner = !owner || od === owner;";
        html += "var okLoc   = !loc   || ld === loc;";
        html += "var show = okTxt && okOwner && okLoc;";
        html += "cards[i].style.display = show ? 'block' : 'none';";
        // also hide corresponding tile
        html += "var tile=document.getElementById('tile'+cards[i].getAttribute('data-idx'));";
        html += "if(tile) tile.style.display = show ? 'block' : 'none'; }}";
        html += "function setView(v){";
        html += "  document.getElementById('listView').style.display = v==='list' ? 'block' : 'none';";
        html += "  document.getElementById('tileView').style.display = v==='tile' ? 'block' : 'none';";
        html += "  document.getElementById('btnList').classList.toggle('active', v==='list');";
        html += "  document.getElementById('btnTile').classList.toggle('active', v==='tile');";
        html += "  try{localStorage.setItem('mbhub_view',v);}catch(e){}";
        html += "}";
        html += "function openMeta(idx){";
        html += "  var data=window.__bandData&&window.__bandData[idx];";
        html += "  if(!data) return;";
        html += "  var m=document.getElementById('metaModal');";
        html += "  document.getElementById('metaImg').src=data.img||'';";
        html += "  document.getElementById('metaImg').style.display=data.img?'block':'none';";
        html += "  document.getElementById('metaName').textContent=data.name||'(no name)';";
        html += "  document.getElementById('metaOwner').textContent=data.owner||'None';";
        html += "  document.getElementById('metaLoc').textContent=data.loc||'None';";
        html += "  document.getElementById('metaType').textContent=data.type||'';";
        html += "  document.getElementById('metaRelease').textContent=data.rtype||'';";
        html += "  document.getElementById('metaRdate').textContent=data.rdate||'';";
        html += "  document.getElementById('metaBcol').textContent=data.bcol||'';";
        html += "  document.getElementById('metaIcol').textContent=data.icol||'';";
        html += "  document.getElementById('metaPrice').textContent=data.price||'';";
        html += "  document.getElementById('metaSku').textContent=data.sku||'';";
        html += "  document.getElementById('metaEditBtn').onclick=function(){";
        html += "    document.getElementById('listView').style.display='block';";
        html += "    closeMeta();";
        html += "    toggleDetails(idx);";
        html += "    var el=document.getElementById('d'+idx);";
        html += "    if(el) setTimeout(function(){el.scrollIntoView({behavior:'smooth',block:'start'});},80);";
        html += "    setView('list');";
        html += "  };";
        html += "  m.classList.add('open');";
        html += "}";
        html += "function closeMeta(){document.getElementById('metaModal').classList.remove('open');}";
        html += "function toggleDetails(id){ var el=document.getElementById('d'+id); if(!el) return; el.style.display=(el.style.display==='none'||el.style.display==='')?'block':'none'; }";
        // Auto-open band card from ?open= param on load
        html += "window.addEventListener('load', function(){ try{ var sv=localStorage.getItem('mbhub_view'); if(sv==='tile') setView('tile'); } catch(e){} try{ var p=new URLSearchParams(window.location.search); var open=p.get('open'); if(open!==null){ var id=parseInt(open,10); if(!isNaN(id)){ setView('list'); toggleDetails(id); var el=document.getElementById('d'+id); if(el){ el.scrollIntoView({behavior:'smooth', block:'start'}); } } } }catch(e){} });";
        html += "function toggleSettings(){ var el=document.getElementById('settings'); if(!el) return; el.style.display=(el.style.display==='none'||el.style.display==='')?'block':'none'; }";

                // Screen-less scan/register helpers
        html += "let __scanPoll=null;";
        html += "function lookupMbc(id){ try{ var inp=document.getElementById('mbc'+id); var st=document.getElementById('mbcStatus'+id); if(!inp) return; var mbc=encodeURIComponent(inp.value||''); if(st){ st.style.display='block'; st.innerText='Fetching listing details\u2026'; } fetch('/lookup?ajax=1&id='+id+'&mbc='+mbc).then(r=>r.json()).then(d=>{ if(!d||!d.status){ if(st) st.innerText='Fetch failed.'; return; } if(d.status==='busy'){ if(st) st.innerText='Another fetch is already running. Please wait.'; return; } if(d.status==='fail'){ if(st) st.innerText='Fetch failed. Check Wi-Fi and the listing.'; return; } if(st) st.innerText='Fetching from MagicBandCollectors.com\u2026'; var __mbcPoll=setInterval(function(){ fetch('/lookup_poll').then(function(r){ return r.json(); }).then(function(p){ if(!p||!p.done) return; clearInterval(__mbcPoll); if(p.result===1){ if(st) st.innerText='Done! Loading\u2026'; window.location='/?open='+id; } else { if(st) st.innerText='Fetch failed. Check Wi-Fi and the listing.'; } }).catch(function(){}); },1000); }).catch(()=>{ if(st) st.innerText='Fetch failed.'; }); }catch(e){} }";
        html += "function armScan(){ fetch('/scan_arm?ajax=1').then(()=>{ var c=document.getElementById('btnScanCancel'); if(c) c.style.display='inline-block'; showScanFetching(); startScanPoll(); }).catch(()=>{}); }";
        html += "function cancelScan(){ fetch('/scan_cancel?ajax=1').then(()=>{ stopScanPoll(); var c=document.getElementById('btnScanCancel'); if(c) c.style.display='none'; var body=document.getElementById('scanBody'); var hint=document.getElementById('scanHint'); if(body){ body.style.display='none'; body.innerHTML=''; } if(hint) hint.innerHTML='Scan cancelled. Tap <b>Start Scan</b> to try again.'; }).catch(()=>{}); }";
        html += "function showScanFetching(){ var body=document.getElementById('scanBody'); var hint=document.getElementById('scanHint'); if(!body||!hint) return; body.style.display='block'; hint.innerText='Tap a band to the reader...'; body.innerHTML='<div style=\"padding:10px;border:1px dashed #ccc;border-radius:10px;\">Waiting for a band... <div style=\"margin-top:8px;font-weight:700;\">(Scanning active)</div></div>'; }";
        html += "function startScanPoll(){ if(__scanPoll) return; __scanPoll=setInterval(pollScan, 700); pollScan(); }";
        html += "function stopScanPoll(){ if(__scanPoll){ clearInterval(__scanPoll); __scanPoll=null; } }";
        html += "function showScanArmed(){ var body=document.getElementById('scanBody'); var hint=document.getElementById('scanHint'); if(!body||!hint) return; body.style.display='block'; hint.innerText='Tap a band to the reader...'; body.innerHTML='<div style=\"padding:10px;border:1px dashed #ccc;border-radius:10px;\">Waiting for a band...</div>'; }";
        html += "function showScanKnown(d){ var body=document.getElementById('scanBody'); var hint=document.getElementById('scanHint'); if(!body||!hint) return; body.style.display='block'; hint.innerText='Known band detected'; var img=''; if(d.img){ img='<img src=\"'+d.img+'\" style=\"width:140px;border-radius:10px;display:block;margin:10px auto;background:#fff;padding:8px;box-sizing:border-box;border:1px solid #e0e4ff;\">'; } var co=d.checkedOut?1:0; var checkBtn=co?'<button class=\"btn-secondary\" style=\"margin:4px 0;\" onclick=\"doCheckInOut('+d.knownIndex+',0);return false;\">&#10003; Check In (mark as home)</button>':'<button class=\"btn-secondary\" style=\"margin:4px 0;background:#fff8e1;color:#e65100;border-color:#ffe0b2;\" onclick=\"doCheckInOut('+d.knownIndex+',1);return false;\">&#x1f9f3; Check Out (pack for trip)</button>'; body.innerHTML= img + '<div style=\"font-weight:800;color:#1a237e;\">'+(d.name||'Known band')+'</div><div class=\"meta\">'+(d.type||'')+'</div>'+checkBtn+'<button class=\"btn-secondary\" style=\"margin-top:6px;\" onclick=\"openBand('+d.knownIndex+');return false;\">&#9998; Open / Edit</button>'; stopScanPoll(); }";
        html += "function doCheckInOut(idx,out){ fetch('/checkinout?id='+idx+'&out='+out,{headers:{'X-Requested-With':'XMLHttpRequest'}}).then(function(r){return r.json();}).then(function(d){ if(d&&d.ok) window.location='/'; else alert('Failed'); }).catch(function(){alert('Error');}); }";
        html += "function showScanNew(d){ var body=document.getElementById('scanBody'); var hint=document.getElementById('scanHint'); if(!body||!hint) return; var c=document.getElementById('btnScanCancel'); if(c) c.style.display='none'; body.style.display='block'; hint.innerText='New band detected'; body.innerHTML='<div style=\"font-weight:800;\">New band</div><div class=\"meta\">'+(d.uid||'')+' &nbsp;'+(d.type||'')+'</div><div style=\"display:flex;gap:10px;margin-top:10px;\"><button class=\"btn-save\" style=\"width:100%;padding:10px;\" onclick=\"confirmSave(1);return false;\">Yes, save</button><button class=\"btn-del\" style=\"width:100%;padding:10px;\" onclick=\"confirmSave(0);return false;\">No</button></div>'; }";
        html += "function confirmSave(yes){ fetch('/scan_confirm?yes='+yes+'&ajax=1').then(r=>r.json()).then(d=>{ var c=document.getElementById('btnScanCancel'); if(c) c.style.display='none'; if(d && d.saved && d.openId>=0){ window.location='/?msg=scan_saved&open='+d.openId; } else { window.location='/?msg=scan_cancel'; } }).catch(()=>{ window.location='/?msg=scan_cancel'; }); }";
        html += "function openBand(id){ window.location='/?open='+id; }";
        html += "function pollScan(){ fetch('/scan_status').then(r=>r.json()).then(d=>{ if(!d||!d.state) return; if(d.state==='known'){ showScanKnown(d); } else if(d.state==='new'){ showScanNew(d); } else if(d.state==='armed'){ showScanArmed(); } }).catch(()=>{}); }";
        html += "</script></head><body><div class='container'>";

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

            // Now-playing mini-bar (always visible)
            html += "<div id='miniPlayer' style='background:#ffffffcc;border-radius:14px;padding:10px 14px;margin-bottom:8px;box-shadow:0 2px 8px rgba(0,0,0,0.12);'>";
            html += "  <div style='display:flex;flex-direction:column;gap:6px;'>";
            // Row 1: icon + track name (wraps freely)
            html += "    <div style='display:flex;align-items:center;gap:8px;'>";
            html += "      <span style='font-size:1.2em;flex-shrink:0;'>&#127925;</span>";
            html += "      <span id='mpTrack' style='font-weight:700;color:#1a237e;font-size:0.9em;word-break:break-word;line-height:1.3;'>Nothing playing</span>";
            html += "    </div>";
            // Row 2: controls + link (hidden until playing)
            html += "    <div style='display:flex;align-items:center;gap:6px;flex-wrap:wrap;'>";
            html += "      <div id='mpControls' style='display:none;gap:6px;align-items:center;'>";
            html += "        <button id='mpPlayPause' onclick='mpToggle()' style='background:#5765f2;color:#fff;border:none;border-radius:8px;padding:6px 12px;cursor:pointer;font-size:0.85em;font-weight:700;'>&#9646;&#9646; Pause</button>";
            html += "        <button onclick='mpStop()' style='background:#ff4757;color:#fff;border:none;border-radius:8px;padding:6px 10px;cursor:pointer;font-size:0.85em;font-weight:700;'>&#9632;</button>";
            html += "      </div>";
            html += "      <a href='/player' style='background:#f0f2ff;color:#3b4ce2;border:1.5px solid #c5caf5;border-radius:8px;padding:6px 10px;text-decoration:none;font-size:0.82em;font-weight:700;white-space:nowrap;'>Music Player &#8594;</a>";
            html += "    </div>";
            html += "  </div>";
            html += "</div>";
            html += "<script>";
            html += "var __mpPoll=null;";
            // Cancel the poll, do the action, update from the response, then restart poll.
            // This prevents a stale poll response from overwriting the action response.
            html += "function mpAction(url){ clearInterval(__mpPoll); __mpPoll=null; fetch(url).then(r=>r.json()).then(function(d){ mpUpdateBar(d); __mpPoll=setInterval(mpPoll,3000); }).catch(function(){ __mpPoll=setInterval(mpPoll,3000); }); }";
            html += "function mpToggle(){ mpAction('/player_pause'); }";
            html += "function mpStop(){ mpAction('/player_stop'); }";
            html += "function mpUpdateBar(d){";
            html += "  if(!d) return;";
            html += "  var trk=document.getElementById('mpTrack');";
            html += "  var btn=document.getElementById('mpPlayPause');";
            html += "  var ctrl=document.getElementById('mpControls');";
            html += "  if(d.playing){";
            html += "    if(trk) trk.textContent=d.track||'Now playing';";
            html += "    if(btn) btn.innerHTML=d.paused?'&#9654; Resume':'&#9646;&#9646; Pause';";
            html += "    if(ctrl) ctrl.style.display='flex';";
            html += "  } else {";
            html += "    if(trk) trk.textContent='Nothing playing';";
            html += "    if(ctrl) ctrl.style.display='none';";
            html += "  }";
            html += "}";
            html += "function mpPoll(){ fetch('/player_status').then(r=>r.json()).then(mpUpdateBar).catch(function(){}); }";
            html += "mpPoll();";
            html += "__mpPoll=setInterval(mpPoll,3000);";
            html += "</script>";

            // SCREEN-LESS REGISTER / SCAN CARD
            html += "<div class='card' id='scanCard' style='text-align:left;'>";
            html += "<div style='display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;'>";
            html += "<div>";
            html += "<div style='font-weight:800;color:#1a237e;font-size:1em;'>Scan a Band</div>";
            html += "<div class='meta' style='margin-top:2px;'>See a band&#39;s info, check it in or out, or register a new band.</div>";
            html += "</div>";
            html += "<div style='display:flex;gap:8px;'>";
            html += "<button id='btnScanStart' class='btn-secondary scan-btn' onclick='armScan(); return false;'>Start Scan</button>";
            html += "<button id='btnScanCancel' class='scan-btn scan-cancel' style='display:none;' onclick='cancelScan(); return false;'>Cancel</button>";
            html += "</div>";
            html += "</div>";
            html += "<div id='scanBody' style='margin-top:10px;'></div>";
            html += "<div class='meta' id='scanHint' style='margin-top:8px;display:none;'>Hold your MagicBand to the reader. The LEDs will swirl while scanning is active.</div>";
            html += "</div>";
            
            // FILTER BAR
            html += "<div class='card' style='text-align:left;'>";
            html += "<div style='display:flex; flex-wrap:wrap; gap:10px; align-items:center;'>";
            html += "<input type='text' id='search' oninput='filterBands()' placeholder='Search by name, type\u2026' style='flex:1; min-width:140px;'>";
            html += "<select id='fOwner' onchange='filterBands()' style='flex:1; min-width:120px;'><option value=''>All owners</option>";
            for(int i=0; i<10; i++) {
                if(ownersList[i][0] != '\0') {
                    String opt = htmlEscape(String(ownersList[i]));
                    html += "<option value='" + opt + "'>" + opt + "</option>";
                }
            }
            html += "</select>";
            html += "<select id='fLoc' onchange='filterBands()' style='flex:1; min-width:120px;'><option value=''>All locations</option>";
            for(int i=0; i<10; i++) {
                if(locationsList[i][0] != '\0') {
                    String opt = htmlEscape(String(locationsList[i]));
                    html += "<option value='" + opt + "'>" + opt + "</option>";
                }
            }
            html += "</select>";
            html += "</div></div>";

            // VIEW TOGGLE
            html += "<div class='view-toggle'>";
            html += "<button id='btnList' class='active' onclick='setView(\"list\")'>&#9776; List</button>";
            html += "<button id='btnTile' onclick='setView(\"tile\")'>&#9632;&#9632; Tiles</button>";
            html += "</div>";

            // BAND JS DATA (for metadata modal)
            html += "<script>window.__bandData=[";
            for(int i=0; i < bandCount; i++) {
                auto &b = registeredBands[i];
                String jImg   = jsonEscape(String(b.imageUrl));
                String jName  = jsonEscape(String(b.name));
                String jOwner = jsonEscape(strlen(b.owner)>0 ? String(b.owner) : "None");
                String jLoc   = jsonEscape(strlen(b.location)>0 ? String(b.location) : "None");
                String jType  = jsonEscape(String(b.type));
                String jRtype = jsonEscape(String(b.releaseType));
                String jRdate = jsonEscape(String(b.releaseDate));
                String jBcol  = jsonEscape(String(b.bandColorName));
                String jIcol  = jsonEscape(String(b.iconColorName));
                String jPrice = jsonEscape(String(b.originalPrice));
                String jSku   = jsonEscape(String(b.sku));
                html += "{\"img\":\"" + jImg + "\",\"name\":\"" + jName + "\",\"owner\":\"" + jOwner + "\",\"loc\":\"" + jLoc + "\",\"type\":\"" + jType + "\",\"rtype\":\"" + jRtype + "\",\"rdate\":\"" + jRdate + "\",\"bcol\":\"" + jBcol + "\",\"icol\":\"" + jIcol + "\",\"price\":\"" + jPrice + "\",\"sku\":\"" + jSku + "\",\"checkedOut\":" + String((int)b.checkedOut) + "}";
                if(i < bandCount - 1) html += ",";
            }
            html += "];</script>";

            // TILE VIEW
            html += "<div id='tileView' style='display:none;'>";
            html += "<div class='card'><div class='tile-grid'>";
            for(int i=0; i < bandCount; i++) {
                auto &b = registeredBands[i];
                if(b.checkedOut) continue; // shown in Packed section
                String escImg  = htmlEscape(String(b.imageUrl));
                String escName = htmlEscape(String(b.name));
                String escOwner = htmlEscape(strlen(b.owner)>0 ? String(b.owner) : "None");
                String escLoc   = htmlEscape(strlen(b.location)>0 ? String(b.location) : "None");
                html += "<div id='tile" + String(i) + "' class='tile' onclick='openMeta(" + String(i) + ")' data-owner='" + escOwner + "' data-loc='" + escLoc + "'>";
                if(strlen(b.imageUrl) > 5) {
                    html += "<img src='" + escImg + "' alt=''>";
                } else {
                    html += "<div class='tile-placeholder'>&#127925;</div>";
                }
                html += "<div class='tile-name'>" + escName + "</div>";
                html += "<div class='tile-meta'>" + escOwner + "</div>";
                html += "</div>";
            }
            html += "</div></div></div>";

            // METADATA MODAL
            html += "<div class='modal-overlay' id='metaModal' onclick='if(event.target===this)closeMeta()'>";
            html += "<div class='modal'>";
            html += "<button class='modal-close' onclick='closeMeta()'>&#10005;</button>";
            html += "<img id='metaImg' class='modal-img' src='' alt=''>";
            html += "<h2 id='metaName' style='margin:0 0 12px;font-size:1.2em;'></h2>";
            html += "<div class='meta-row'><b>Owner</b><span id='metaOwner'></span></div>";
            html += "<div class='meta-row'><b>Location</b><span id='metaLoc'></span></div>";
            html += "<div class='meta-row'><b>Type</b><span id='metaType'></span></div>";
            html += "<div class='meta-row'><b>Release</b><span id='metaRelease'></span></div>";
            html += "<div class='meta-row'><b>Release date</b><span id='metaRdate'></span></div>";
            html += "<div class='meta-row'><b>Band colour</b><span id='metaBcol'></span></div>";
            html += "<div class='meta-row'><b>Icon colour</b><span id='metaIcol'></span></div>";
            html += "<div class='meta-row'><b>Original price</b><span id='metaPrice'></span></div>";
            html += "<div class='meta-row'><b>SKU</b><span id='metaSku'></span></div>";
            html += "<button id='metaEditBtn' class='btn-secondary' style='margin-top:16px;'>Edit this band</button>";
            html += "</div></div>";

            // LIST VIEW wrapper open
            html += "<div id='listView'>";

            // CHECKED-OUT / PACKED SECTION
            {
                int coCount = 0;
                for(int i=0; i<bandCount; i++) if(registeredBands[i].checkedOut) coCount++;
                if(coCount > 0) {
                    html += "<div class='card' style='border:2px solid #ffe082;background:#fffde7;'>";
                    html += "<div style='display:flex;align-items:center;gap:10px;margin-bottom:12px;'>";
                    html += "<span style='font-size:1.4em;'>&#x1f9f3;</span>";
                    html += "<div><div style='font-weight:800;color:#e65100;font-size:1em;'>Packed for Trip</div>";
                    html += "<div class='meta'>" + String(coCount) + " band" + (coCount==1?"":"s") + " checked out &mdash; scan to check back in</div></div>";
                    html += "</div>";
                    html += "<div style='display:flex;flex-wrap:wrap;gap:10px;'>";
                    for(int i=0; i<bandCount; i++) {
                        if(!registeredBands[i].checkedOut) continue;
                        String escImg2  = htmlEscape(String(registeredBands[i].imageUrl));
                        String escName2 = htmlEscape(String(registeredBands[i].name));
                        String escOwner2 = htmlEscape(strlen(registeredBands[i].owner)>0 ? String(registeredBands[i].owner) : "None");
                        html += "<div style='display:flex;flex-direction:column;align-items:center;gap:4px;'>";
                        if(strlen(registeredBands[i].imageUrl) > 5) {
                            html += "<img src='" + escImg2 + "' style='width:70px;height:100px;object-fit:contain;border-radius:8px;background:#fff;padding:4px;border:1px solid #ffe082;'>";
                        } else {
                            html += "<div style='width:70px;height:100px;border-radius:8px;background:#fff;border:1px solid #ffe082;display:flex;align-items:center;justify-content:center;font-size:1.6em;'>&#127925;</div>";
                        }
                        html += "<div style='font-size:0.78em;font-weight:700;color:#1a237e;text-align:center;max-width:80px;word-break:break-word;'>" + escName2 + "</div>";
                        html += "<div style='font-size:0.7em;color:#888;'>" + escOwner2 + "</div>";
                        html += "<a href='/checkinout?id=" + String(i) + "&out=0' class='btn-del' style='background:#43a047;font-size:0.72em;padding:4px 8px;' onclick='return confirm(\"Check in " + escName2 + "?\")'>Check In</a>";
                        html += "</div>";
                    }
                    html += "</div></div>";
                }
            }

            // BAND CARDS (summary + expandable details)

            // BAND CARDS (summary + expandable details)
            for(int i=0; i < bandCount; i++) {
                if(registeredBands[i].checkedOut) continue; // shown in Packed section
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

                html += "<div class='card band-card' data-owner='" + escOwner + "' data-loc='" + escLoc + "' data-idx='" + String(i) + "'>";

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
                // MagicBandCollectors Section (shown first)
                html += "<div class='section'>";
                html += "<div class='section-title'>MagicBandCollectors</div>";
                html += "<p class='meta' style='margin-bottom:10px;line-height:1.5;'>Search for your band on <b>magicbandcollectors.com</b>, open the listing page, then paste the full URL or just the ID number from the URL (e.g. <code>2535</code> from <code>.../?id=2535</code>). Hit Fetch to auto-fill the details below.</p>";
                html += "<div class='field'><label>URL or ID from MagicBandCollectors.com</label><input id='mbc" + String(i) + "' type='text' name='mbc' value='" + escMbc + "' placeholder='e.g. 2535 or full URL'></div>";
                html += "<button type='button' class='btn-secondary' onclick='lookupMbc(" + String(i) + "); return false;'>Fetch Details from MagicBandCollectors</button>";
                html += "<div id='mbcStatus" + String(i) + "' class='status' style='display:none;'></div>";
                html += "</div>";
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

            html += "</div>"; // #listView

            // SETTINGS (collapsible)
            html += "<div class='card'>";
            html += "<div class='settings-header' onclick='toggleSettings()'>Settings (Owners, Locations, Hub)</div>";
            html += "<div class='settings-body' id='settings'>";

            // THEME BUILDER LINK
            html += "<hr><h3>Theme Builder</h3>";
            html += "<div class='meta' style='margin-bottom:10px;'>Create custom LED light patterns with your own colours and audio files.</div>";
            html += "<a class='btn-secondary' style='text-align:center;text-decoration:none;margin-top:8px;' href='/themes'>Open Theme Builder</a>";

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

            // BACKUP / RESTORE
            html += "<hr><h3>Backup / Restore</h3>";
            html += "<div class='meta' style='margin-bottom:10px;'>Export your bands before flashing new firmware. Import restores bands + owners + locations + hub timeouts.</div>";
            html += "<a class='btn-secondary' style='text-align:center;text-decoration:none;margin-top:8px;' href='/export' download>Export Backup (JSON)</a>";
            html += "<hr><form action='/import' method='POST'>";
            html += "<div class='field'><label>Import Backup JSON</label>";
            html += "<textarea name='data' style='width:100%;min-height:140px;padding:10px;border-radius:8px;border:1px solid #ccc;font-size:14px;' placeholder='Paste exported JSON here...'></textarea>";
            html += "</div>";
            html += "<input type='submit' class='btn-save' value='Import Backup' onclick='return confirm(\"Import will overwrite all stored bands. Continue?\")'>";
            html += "</form>";

            html += "</div></div>";
        }
        html += "</div>"; // container
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
        json += "\"img\":\"" + jsonEscape(img) + "\",";
        json += "\"checkedOut\":" + String((kidx >= 0 && kidx < bandCount) ? (int)registeredBands[kidx].checkedOut : 0);
        json += "}";
        request->send(200, "application/json", json);
    });

    // ROUTE: Check In / Check Out a band
    server.on("/checkinout", HTTP_GET, [](AsyncWebServerRequest *request){
        if(!request->hasParam("id") || !request->hasParam("out")){
            request->send(400, "application/json", "{\"ok\":false}");
            return;
        }
        int id  = request->getParam("id")->value().toInt();
        int out = request->getParam("out")->value().toInt();
        if(id < 0 || id >= bandCount){
            request->send(400, "application/json", "{\"ok\":false}");
            return;
        }
        registeredBands[id].checkedOut = (out != 0) ? 1 : 0;
        prefs.begin("mbands", false);
        prefs.putBytes(("b" + String(id)).c_str(), &registeredBands[id], sizeof(BandRecord));
        prefs.end();
        // If called via fetch (XHR) return JSON; otherwise redirect back to main page
        if(request->hasHeader("X-Requested-With")) {
            request->send(200, "application/json", "{\"ok\":true}");
        } else {
            request->redirect("/");
        }
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

    // Poll endpoint: JS calls this every second while a lookup is running
    server.on("/lookup_poll", HTTP_GET, [](AsyncWebServerRequest *request){
        String json;
        if(g_lookupInProgress) {
            json = "{\"done\":false}";
        } else {
            json = "{\"done\":true,\"result\":" + String((int)g_lookupResult) +
                   ",\"bandId\":" + String((int)g_lookupBandId) + "}";
        }
        request->send(200, "application/json", json);
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
            json += "\"audioFile\":\"" + jsonEscape(String(customThemes[i].audioFile)) + "\",";
            json += "\"lsUseColors\":" + String((unsigned int)customThemes[i].lsUseThemeColors);
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
                        int ucv = 0; jsonFindValueInt(cobj, "lsUseColors", ucv); ct.lsUseThemeColors = (uint8_t)ucv;
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
                String path = "/" + safe;
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
        html += ".btn-del{background:#ff4757;color:white;border:none;padding:6px 14px;border-radius:5px;font-size:0.82em;cursor:pointer;}";
        html += ".btn-edit{background:#3b4ce2;color:white;border:none;padding:6px 14px;border-radius:5px;font-size:0.82em;cursor:pointer;text-decoration:none;display:inline-block;}";
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
                html += "<form action='/theme_delete' method='GET' onsubmit='return confirm(\"Delete theme?\")' style='display:inline;'>";
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
            const char* loopNames[CUSTOM_THEME_LOOP_COUNT] = {
                "None (stop after opening)", "Spinning Comet", "Gentle Pulse", "Rainbow Spin",
                "🌈 Rainbow", "✨ Pulse", "🎠 Colour Cycle",
                "🏠 Main Street", "🌿 Adventureland", "🔥 Frontierland",
                "Liberty Square", "🦄 Fantasyland", "🚀 Tomorrowland", "👻 Haunted Mansion"
            };
            // Group: classic modes
            html += "<optgroup label='Classic'>";
            for (uint8_t lp = 0; lp < 4; lp++)
                html += "<option value='" + String(lp) + "'" + String(defLoop == lp ? " selected" : "") + ">" + String(loopNames[lp]) + "</option>";
            html += "</optgroup>";
            // Group: player lightshow modes
            html += "<optgroup label='Lightshow Modes'>";
            for (uint8_t lp = 4; lp < CUSTOM_THEME_LOOP_COUNT; lp++)
                html += "<option value='" + String(lp) + "'" + String(defLoop == lp ? " selected" : "") + ">" + String(loopNames[lp]) + "</option>";
            html += "</optgroup>";
            html += "</select></div>";

            // ---- Colour override toggle (only meaningful for lightshow modes) ----
            bool defUseColors = isEdit ? (customThemes[editIdx].lsUseThemeColors != 0) : false;
            html += "<div class='field' id='lsColorRow'>";
            html += "<label style='display:flex;align-items:center;gap:10px;cursor:pointer;'>";
            html += "<input type='checkbox' name='lsUseThemeColors' value='1'" + String(defUseColors ? " checked" : "") + " id='ckLsColors'> ";
            html += "<span>Use theme colours in lightshow <span style='font-size:0.8em;color:#888;'>(replaces lightshow's default palette)</span></span>";
            html += "</label></div>";

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

        ct.lsUseThemeColors = (request->hasParam("lsUseThemeColors") &&
                                request->getParam("lsUseThemeColors")->value() == "1") ? 1 : 0;

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

    // =========================================================================
    // MUSIC PLAYER
    // =========================================================================

    // Helper: rebuild playlist from SD
    // (called by player_play on first load and by upload completion)
    // Declared as a lambda we can invoke inside multiple handlers.

    // -------------------------------------------------------------------------
    // ROUTE: /player — full music player page
    // -------------------------------------------------------------------------
    server.on("/player", HTTP_GET, [](AsyncWebServerRequest *request){
        String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
        html += "<title>MagicBand Hub &mdash; Player</title><style>";
        html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:linear-gradient(160deg,#1a237e 0%,#1565c0 60%,#0288d1 100%);min-height:100vh;padding:20px;margin:0;color:#fff;}";
        html += "h1{font-weight:900;margin:10px 0 4px 0;letter-spacing:-0.5px;text-shadow:0 2px 8px rgba(0,0,0,0.25);}";
        html += ".card{background:#fff;border-radius:18px;padding:18px;margin:12px auto;max-width:100%;box-shadow:0 4px 20px rgba(10,30,90,0.18);color:#1a237e;}";
        html += ".container{max-width:600px;margin:0 auto;}";
        html += "button,a.btn{display:inline-block;border:none;border-radius:10px;padding:10px 18px;font-size:0.95em;font-weight:700;cursor:pointer;text-decoration:none;}";
        html += ".btn-primary{background:#5765f2;color:#fff;} .btn-primary:hover{background:#3d4fd6;}";
        html += ".btn-secondary{background:#e8eaff;color:#1a237e;} .btn-secondary:hover{background:#c5caff;}";
        html += ".btn-sm{padding:7px 13px;font-size:0.82em;}";
        html += ".btn-danger{background:#ffebee;color:#c62828;} .btn-danger:hover{background:#ffcdd2;}";
        html += ".track-list{list-style:none;padding:0;margin:0;}";
        html += ".track-item{display:flex;align-items:center;gap:8px;padding:9px 10px;border-radius:10px;cursor:pointer;transition:background 0.15s;font-size:0.93em;}";
        html += ".track-item:hover{background:#f0f4ff;} .track-item.active{background:#e8eaff;font-weight:700;}";
        html += ".track-name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}";
        html += ".player-bar{display:flex;align-items:center;justify-content:center;gap:12px;margin:16px 0;}";
        html += ".vol-row{display:flex;align-items:center;gap:10px;margin-top:10px;}";
        html += "input[type=range]{flex:1;accent-color:#5765f2;}";
        html += ".now-playing{font-size:1em;font-weight:800;color:#1a237e;margin-bottom:4px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}";
        html += ".status-bar{font-size:0.8em;color:#888;margin-top:4px;}";
        html += ".ls-row{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px;}";
        html += ".ls-btn{background:#f0f4ff;border:2px solid transparent;border-radius:10px;padding:8px 12px;font-size:0.82em;font-weight:600;cursor:pointer;color:#1a237e;transition:all 0.15s;}";
        html += ".ls-btn.active{border-color:#5765f2;background:#e8eaff;}";
        html += ".upload-area{margin-top:12px;} .upload-area input[type=file]{width:100%;}";
        html += ".progress-wrap{height:6px;background:#e0e4ff;border-radius:3px;margin:10px 0;overflow:hidden;}";
        html += ".progress-bar{height:100%;background:#5765f2;border-radius:3px;width:0;transition:width 0.4s;}";
        html += "a.back{color:#ffffffcc;text-decoration:none;font-size:0.9em;display:inline-block;margin-bottom:10px;} a.back:hover{color:#fff;}";
        html += "</style></head><body><div class='container'>";
        html += "<a class='back' href='/'>&#8592; Back to Hub</a>";
        html += "<h1>&#127925; Music Player</h1>";

        // Now-playing card
        html += "<div class='card'>";
        html += "<div class='now-playing' id='nowPlaying'>Not playing</div>";
        html += "<div class='status-bar' id='statusBar'>Select a track or press Play</div>";
        html += "<div class='progress-wrap'><div class='progress-bar' id='progressBar'></div></div>";
        html += "<div class='player-bar'>";
        html += "<button class='btn btn-secondary btn-sm' onclick='doPrev()'>&#9664;&#9664;</button>";
        html += "<button class='btn btn-primary' id='btnPlayPause' onclick='doPlayPause()'>&#9654; Play</button>";
        html += "<button class='btn btn-secondary btn-sm' onclick='doNext()'>&#9654;&#9654;</button>";
        html += "<button class='btn btn-danger btn-sm' onclick='doStop()'>&#9632; Stop</button>";
        html += "</div>";
        html += "<div class='vol-row'>";
        html += "<span style='font-size:0.85em;color:#888;'>&#128266;</span>";
        html += "<input type='range' id='volSlider' min='0' max='21' value='" + String((int)g_volume) + "' oninput='doVolume(this.value)' onchange='doVolume(this.value)'>";
        html += "<span id='volLabel' style='font-size:0.85em;color:#888;width:28px;'>" + String((int)g_volume) + "</span>";
        html += "</div>";
        html += "<div style='display:flex;gap:8px;margin-top:10px;'>"; 
        html += "<button class='ls-btn" + String(g_playerRepeatOne?" active":"") + "' id='btnRepeatOne' onclick='doRepeatOne()' title='Repeat current track'>&#128258; Repeat 1</button>";
        html += "<button class='ls-btn" + String(g_playerRepeat?" active":"") + "' id='btnRepeat' onclick='doRepeat()' title='Loop through playlist'>&infin; Keep Playing</button>";
        html += "</div></div>";

        // Lightshow card
        html += "<div class='card'>";
        html += "<div style='font-weight:800;margin-bottom:10px;'>&#127811; LED Lightshow</div>";
        html += "<div class='ls-row'>";
        html += "<button class='ls-btn" + String(g_playerLightshow==0?" active":"") + "' id='ls0' onclick='setLightshow(0)'>Off</button>";
        html += "<button class='ls-btn" + String(g_playerLightshow==1?" active":"") + "' id='ls1' onclick='setLightshow(1)'>&#127752; Rainbow</button>";
        html += "<button class='ls-btn" + String(g_playerLightshow==2?" active":"") + "' id='ls2' onclick='setLightshow(2)'>&#10024; Pulse</button>";
        html += "<button class='ls-btn" + String(g_playerLightshow==3?" active":"") + "' id='ls3' onclick='setLightshow(3)'>&#127894; Colour Cycle</button>";
        html += "</div>";
        html += "<div style='font-size:0.8em;color:#888;margin:10px 0 6px;font-weight:700;letter-spacing:0.04em;text-transform:uppercase;'>Magic Kingdom Lands</div>";
        html += "<div class='ls-row'>";
        html += "<button class='ls-btn" + String(g_playerLightshow==4?" active":"") + "' id='ls4' onclick='setLightshow(4)'>&#127968; Main Street</button>";
        html += "<button class='ls-btn" + String(g_playerLightshow==5?" active":"") + "' id='ls5' onclick='setLightshow(5)'>&#127807; Adventureland</button>";
        html += "<button class='ls-btn" + String(g_playerLightshow==6?" active":"") + "' id='ls6' onclick='setLightshow(6)'>&#128293; Frontierland</button>";
        html += "<button class='ls-btn" + String(g_playerLightshow==7?" active":"") + "' id='ls7' onclick='setLightshow(7)'>&#127468;&#127463; Liberty Square</button>";
        html += "<button class='ls-btn" + String(g_playerLightshow==8?" active":"") + "' id='ls8' onclick='setLightshow(8)'>&#129984; Fantasyland</button>";
        html += "<button class='ls-btn" + String(g_playerLightshow==9?" active":"") + "' id='ls9' onclick='setLightshow(9)'>&#128640; Tomorrowland</button>";
        html += "</div>";
        html += "<div style='font-size:0.8em;color:#888;margin:10px 0 6px;font-weight:700;letter-spacing:0.04em;text-transform:uppercase;'>Special</div>";
        html += "<div class='ls-row'>";
        html += "<button class='ls-btn" + String(g_playerLightshow==10?" active":"") + "' id='ls10' onclick='setLightshow(10)'>&#128123; Haunted Mansion</button>";
        html += "</div>"; // close special ls-row
        html += "<div style='font-size:0.8em;color:#888;margin:10px 0 6px;font-weight:700;letter-spacing:0.04em;text-transform:uppercase;'>Speed</div>";
        html += "<div class='ls-row'>";
        html += "<button class='ls-btn" + String(g_playerLsSpeed==0?" active":"") + "' id='spd0' onclick='setSpeed(0)'>&#128034; Sedate</button>";
        html += "<button class='ls-btn" + String(g_playerLsSpeed==1?" active":"") + "' id='spd1' onclick='setSpeed(1)'>Medium</button>";
        html += "<button class='ls-btn" + String(g_playerLsSpeed==2?" active":"") + "' id='spd2' onclick='setSpeed(2)'>&#9889; Lively</button>";
        html += "</div></div>"; // close speed row + card

        // Track list card
        html += "<div class='card'>";
        html += "<div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:12px;'>";
        html += "<div style='font-weight:800;'>&#127916; Tracks on SD Card</div>";
        html += "<button class='btn btn-secondary btn-sm' onclick='loadTrackList()'>&#8635; Refresh</button>";
        html += "</div>";
        html += "<ul class='track-list' id='trackList'><li style='color:#888;font-size:0.9em;'>Loading&hellip;</li></ul>";
        html += "</div>";

        // Upload card
        html += "<div class='card'>";
        html += "<div style='font-weight:800;margin-bottom:10px;'>&#128190; Upload Audio</div>";
        html += "<div class='meta' style='color:#888;margin-bottom:10px;'>MP3, WAV, AAC or FLAC &mdash; saved to SD card</div>";
        html += "<input type='file' id='upFile' accept='.mp3,.wav,.aac,.flac' multiple>";
        html += "<div id='upStatus' style='margin-top:8px;font-size:0.85em;color:#888;'></div>";
        html += "<button class='btn btn-primary' style='margin-top:10px;' onclick='doUpload()'>Upload</button>";
        html += "</div>";

        // JS
        html += "<script>";
        html += "var currentTrack=-1, isPlaying=false, statusPoll=null;";
        html += "function loadTrackList(){";
        html += "  fetch('/player_tracks').then(r=>r.json()).then(function(tr){";
        html += "    var ul=document.getElementById('trackList'); ul.innerHTML='';";
        html += "    if(!tr||!tr.length){ul.innerHTML='<li style=\"color:#888;font-size:0.9em;\">No audio files found on SD card</li>';return;}";
        html += "    tr.forEach(function(t,i){";
        html += "      var li=document.createElement('li'); li.className='track-item'+(i===currentTrack?' active':'');";
        html += "      li.innerHTML='<span style=\"color:#5765f2;font-weight:700;min-width:22px;\">'+(i+1)+'</span><span class=\"track-name\">'+t+'</span><button class=\"btn btn-primary btn-sm\" onclick=\"playTrack('+i+');event.stopPropagation();\">&#9654;</button>';";
        html += "      li.onclick=function(){playTrack(i);};";
        html += "      ul.appendChild(li);";
        html += "    });";
        html += "  });";
        html += "}";
        html += "function playTrack(i){";
        html += "  fetch('/player_play?idx='+i).then(r=>r.json()).then(function(d){if(d.ok)startPoll();});";
        html += "}";
        html += "function doPlayPause(){";
        html += "  if(!isPlaying) playTrack(currentTrack>=0?currentTrack:0);";
        html += "  else fetch('/player_pause').then(r=>r.json()).then(function(d){updateStatus(d);});";
        html += "}";
        html += "function doStop(){ fetch('/player_stop').then(r=>r.json()).then(function(d){updateStatus(d);stopPoll();}); }";
        html += "function doRepeat(){ var cur=document.getElementById('btnRepeat'); var now=cur&&cur.classList.contains('active'); fetch('/player_repeat?r='+(now?0:1)).then(r=>r.json()).then(function(d){ if(cur) cur.classList.toggle('active',!!d.repeat); var r1=document.getElementById('btnRepeatOne'); if(r1&&d.repeat_one!=null) r1.classList.toggle('active',!!d.repeat_one); }); }";
        html += "function doRepeatOne(){ var cur=document.getElementById('btnRepeatOne'); var now=cur&&cur.classList.contains('active'); fetch('/player_repeat_one?r='+(now?0:1)).then(r=>r.json()).then(function(d){ if(cur) cur.classList.toggle('active',!!d.repeat_one); var rb=document.getElementById('btnRepeat'); if(rb&&d.repeat!=null) rb.classList.toggle('active',!!d.repeat); }); }";
        html += "function doPrev(){ fetch('/player_prev').then(r=>r.json()).then(function(d){if(d.ok)startPoll();}); }";
        html += "function doNext(){ fetch('/player_next').then(r=>r.json()).then(function(d){if(d.ok)startPoll();}); }";
        html += "function doVolume(v){ document.getElementById('volLabel').textContent=v; fetch('/player_volume?v='+v); }";
        html += "function setLightshow(m){";
        html += "  fetch('/player_lightshow?m='+m).then(r=>r.json()).then(function(){";
        html += "    [0,1,2,3,4,5,6,7,8,9,10].forEach(function(i){ var b=document.getElementById('ls'+i); if(b) b.classList.toggle('active',i===m); });";
        html += "  });";
        html += "}";
        html += "function setSpeed(s){ fetch('/player_speed?s='+s).then(r=>r.json()).then(function(){ [0,1,2].forEach(function(i){ var b=document.getElementById('spd'+i); if(b) b.classList.toggle('active',i===s); }); }); }";
        html += "function updateStatus(d){";
        html += "  if(!d) return;";
        html += "  currentTrack=d.idx!=null?d.idx:-1;";
        html += "  isPlaying=d.playing;";
        html += "  var np=document.getElementById('nowPlaying');";
        html += "  var sb=document.getElementById('statusBar');";
        html += "  var pp=document.getElementById('btnPlayPause');";
        html += "  if(np) np.textContent=d.track||'Not playing';";
        html += "  if(sb) sb.textContent=d.playing?(d.paused?'Paused':'Playing'):'Stopped';";
        html += "  if(pp) pp.innerHTML=d.paused?'&#9654; Resume':(d.playing?'&#9646;&#9646; Pause':'&#9654; Play');";
        html += "  var tl=document.getElementById('trackList');";
        html += "  if(tl){ Array.from(tl.children).forEach(function(li,i){ li.classList.toggle('active',i===currentTrack); }); }";
        html += "  if(d.speed!=null){ [0,1,2].forEach(function(i){ var b=document.getElementById('spd'+i); if(b) b.classList.toggle('active',i===d.speed); }); }";
        html += "  var rb=document.getElementById('btnRepeat'); if(rb&&d.repeat!=null) rb.classList.toggle('active',!!d.repeat);";        html += "  var r1=document.getElementById('btnRepeatOne'); if(r1&&d.repeat_one!=null) r1.classList.toggle('active',!!d.repeat_one);"
;        html += "}";
        html += "function startPoll(){ if(statusPoll) clearInterval(statusPoll); statusPoll=setInterval(pollStatus,2000); pollStatus(); }";
        html += "function stopPoll(){ if(statusPoll){ clearInterval(statusPoll); statusPoll=null; } }";
        html += "function pollStatus(){ fetch('/player_status').then(r=>r.json()).then(updateStatus).catch(function(){}); }";
        html += "function doUpload(){";
        html += "  var f=document.getElementById('upFile'); if(!f.files.length){alert('Select a file first');return;}";
        html += "  var st=document.getElementById('upStatus');";
        html += "  Array.from(f.files).forEach(function(file){";
        html += "    var fd=new FormData(); fd.append('file',file,file.name);";
        html += "    st.textContent='Uploading '+file.name+'...';";
        html += "    fetch('/upload_audio',{method:'POST',body:fd}).then(r=>r.json()).then(function(d){";
        html += "      st.textContent=d.ok?('Uploaded: '+d.name):'Upload failed';";
        html += "      if(d.ok) loadTrackList();";
        html += "    }).catch(function(){ st.textContent='Upload error'; });";
        html += "  });";
        html += "}";
        html += "loadTrackList(); startPoll();";
        html += "</script></div></body></html>";
        request->send(200, "text/html", html);
    });

    // -------------------------------------------------------------------------
    // ROUTE: /player_tracks — JSON array of audio filenames on SD
    // -------------------------------------------------------------------------
    server.on("/player_tracks", HTTP_GET, [](AsyncWebServerRequest *request){
        // Rebuild in-memory playlist and return as JSON
        g_playlistCount = 0;
        String json = "[";
        bool first = true;
        File root = SD_MMC.open("/");
        if (root) {
            File f = root.openNextFile();
            while (f && g_playlistCount < 64) {
                if (!f.isDirectory()) {
                    String fname = String(f.name());
                    int sl = fname.lastIndexOf('/');
                    String base = (sl >= 0) ? fname.substring(sl + 1) : fname;
                    if (base.startsWith("._")) { f = root.openNextFile(); continue; }
                    String lower = base; lower.toLowerCase();
                    if (lower.endsWith(".mp3") || lower.endsWith(".wav") ||
                        lower.endsWith(".aac") || lower.endsWith(".flac")) {
                        strncpy(g_playlist[g_playlistCount].name, base.c_str(), 63);
                        g_playlist[g_playlistCount].name[63] = '\0';
                        g_playlistCount++;
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
    // ROUTE: /player_play?idx=N
    // -------------------------------------------------------------------------
    server.on("/player_play", HTTP_GET, [](AsyncWebServerRequest *request){
        int idx = request->hasParam("idx") ? request->getParam("idx")->value().toInt() : 0;
        if (g_playlistCount == 0) {
            // Rebuild playlist first
            g_playlistCount = 0;
            File root = SD_MMC.open("/");
            if (root) {
                File f = root.openNextFile();
                while (f && g_playlistCount < 64) {
                    if (!f.isDirectory()) {
                        String fname = String(f.name());
                        int sl = fname.lastIndexOf('/');
                        String base = (sl >= 0) ? fname.substring(sl + 1) : fname;
                        if (base.startsWith("._")) { f = root.openNextFile(); continue; }
                        String lower = base; lower.toLowerCase();
                        if (lower.endsWith(".mp3") || lower.endsWith(".wav") ||
                            lower.endsWith(".aac") || lower.endsWith(".flac")) {
                            strncpy(g_playlist[g_playlistCount].name, base.c_str(), 63);
                            g_playlist[g_playlistCount].name[63] = '\0';
                            g_playlistCount++;
                        }
                    }
                    f = root.openNextFile();
                }
                root.close();
            }
        }
        if (idx < 0 || idx >= g_playlistCount) {
            request->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid index\"}");
            return;
        }
        g_playerIndex = idx;
        char path[80]; snprintf(path, sizeof(path), "/%s", g_playlist[g_playerIndex].name);
        bool ok = Play_Music_file(path);
        g_playerActive = ok;
        g_playerPaused = false;
        String resp = "{\"ok\":" + String(ok?"true":"false") + ",\"track\":\"" + jsonEscape(String(g_playlist[g_playerIndex].name)) + "\",\"idx\":" + String(g_playerIndex) + "}";
        request->send(200, "application/json", resp);
    });

    // -------------------------------------------------------------------------
    // ROUTE: /player_pause — toggle pause
    // -------------------------------------------------------------------------
    server.on("/player_pause", HTTP_GET, [](AsyncWebServerRequest *request){
        if (g_playerActive) {
            g_playerPaused = !g_playerPaused;
            Music_pause(); // library uses pauseResume() toggle
        }
        String name = (g_playerIndex >= 0 && g_playerIndex < g_playlistCount) ? String(g_playlist[g_playerIndex].name) : "";
        String resp = "{\"ok\":true,\"playing\":" + String(g_playerActive?"true":"false") + ",\"paused\":" + String(g_playerPaused?"true":"false") + ",\"track\":\"" + jsonEscape(name) + "\",\"idx\":" + String(g_playerIndex) + "}";
        request->send(200, "application/json", resp);
    });

    // -------------------------------------------------------------------------
    // ROUTE: /player_stop
    // -------------------------------------------------------------------------
    server.on("/player_stop", HTTP_GET, [](AsyncWebServerRequest *request){
        Music_stop();
        g_playerActive = false;
        g_playerPaused = false;
        g_playerIndex  = -1;
        request->send(200, "application/json", "{\"ok\":true,\"playing\":false,\"paused\":false,\"track\":\"\",\"idx\":-1}");
    });

    // -------------------------------------------------------------------------
    // ROUTE: /player_next
    // -------------------------------------------------------------------------
    server.on("/player_next", HTTP_GET, [](AsyncWebServerRequest *request){
        if (g_playlistCount == 0) { request->send(200, "application/json", "{\"ok\":false}"); return; }
        g_playerIndex = (g_playerIndex + 1) % g_playlistCount;
        char path[80]; snprintf(path, sizeof(path), "/%s", g_playlist[g_playerIndex].name);
        bool ok = Play_Music_file(path);
        g_playerActive = ok; g_playerPaused = false;
        request->send(200, "application/json", "{\"ok\":" + String(ok?"true":"false") + ",\"idx\":" + String(g_playerIndex) + "}");
    });

    // -------------------------------------------------------------------------
    // ROUTE: /player_prev
    // -------------------------------------------------------------------------
    server.on("/player_prev", HTTP_GET, [](AsyncWebServerRequest *request){
        if (g_playlistCount == 0) { request->send(200, "application/json", "{\"ok\":false}"); return; }
        g_playerIndex = (g_playerIndex - 1 + g_playlistCount) % g_playlistCount;
        char path[80]; snprintf(path, sizeof(path), "/%s", g_playlist[g_playerIndex].name);
        bool ok = Play_Music_file(path);
        g_playerActive = ok; g_playerPaused = false;
        request->send(200, "application/json", "{\"ok\":" + String(ok?"true":"false") + ",\"idx\":" + String(g_playerIndex) + "}");
    });

    // -------------------------------------------------------------------------
    // ROUTE: /player_volume?v=0-21
    // -------------------------------------------------------------------------
    server.on("/player_volume", HTTP_GET, [](AsyncWebServerRequest *request){
        if (request->hasParam("v")) {
            uint8_t v = (uint8_t)constrain(request->getParam("v")->value().toInt(), 0, 21);
            Music_set_volume(v);
        }
        request->send(200, "application/json", "{\"ok\":true,\"vol\":" + String((int)g_volume) + "}");
    });

    // -------------------------------------------------------------------------
    // ROUTE: /player_lightshow?m=0-3
    // -------------------------------------------------------------------------
    server.on("/player_lightshow", HTTP_GET, [](AsyncWebServerRequest *request){
        if (request->hasParam("m")) {
            uint8_t m = (uint8_t)constrain(request->getParam("m")->value().toInt(), 0, 10);
            g_playerLightshow = m;
        }
        request->send(200, "application/json", "{\"ok\":true,\"mode\":" + String((int)g_playerLightshow) + "}");
    });

    // -------------------------------------------------------------------------
    // ROUTE: /player_speed?s=0-2
    // -------------------------------------------------------------------------
    server.on("/player_speed", HTTP_GET, [](AsyncWebServerRequest *request){
        if (request->hasParam("s")) {
            g_playerLsSpeed = (uint8_t)constrain(request->getParam("s")->value().toInt(), 0, 2);
        }
        request->send(200, "application/json", "{\"ok\":true,\"speed\":" + String((int)g_playerLsSpeed) + "}");
    });

    // -------------------------------------------------------------------------
    // ROUTE: /player_repeat?r=0|1
    // -------------------------------------------------------------------------
    server.on("/player_repeat", HTTP_GET, [](AsyncWebServerRequest *request){
        if (request->hasParam("r")) {
            g_playerRepeat = request->getParam("r")->value().toInt() != 0;
            if (g_playerRepeat) g_playerRepeatOne = false; // mutually exclusive
        }
        request->send(200, "application/json", "{\"ok\":true,\"repeat\":" + String(g_playerRepeat?"true":"false") + ",\"repeat_one\":" + String(g_playerRepeatOne?"true":"false") + "}");
    });

    // -------------------------------------------------------------------------
    // ROUTE: /player_repeat_one?r=0|1
    // -------------------------------------------------------------------------
    server.on("/player_repeat_one", HTTP_GET, [](AsyncWebServerRequest *request){
        if (request->hasParam("r")) {
            g_playerRepeatOne = request->getParam("r")->value().toInt() != 0;
            if (g_playerRepeatOne) g_playerRepeat = false; // mutually exclusive
        }
        request->send(200, "application/json", "{\"ok\":true,\"repeat_one\":" + String(g_playerRepeatOne?"true":"false") + ",\"repeat\":" + String(g_playerRepeat?"true":"false") + "}");
    });

    // -------------------------------------------------------------------------
    // ROUTE: /player_status — current play state
    // -------------------------------------------------------------------------
    server.on("/player_status", HTTP_GET, [](AsyncWebServerRequest *request){
        String name = (g_playerIndex >= 0 && g_playerIndex < g_playlistCount) ? String(g_playlist[g_playerIndex].name) : "";
        String resp = "{\"playing\":" + String(g_playerActive?"true":"false");
        resp += ",\"paused\":"  + String(g_playerPaused?"true":"false");
        resp += ",\"idx\":"     + String(g_playerIndex);
        resp += ",\"track\":\"" + jsonEscape(name) + "\"";
        resp += ",\"vol\":"     + String((int)g_volume);
        resp += ",\"lightshow\":" + String((int)g_playerLightshow);
        resp += ",\"speed\":"     + String((int)g_playerLsSpeed);
        resp += ",\"repeat\":"      + String(g_playerRepeat?"true":"false");
        resp += ",\"repeat_one\":" + String(g_playerRepeatOne?"true":"false");
        resp += "}";
        request->send(200, "application/json", resp);
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
    