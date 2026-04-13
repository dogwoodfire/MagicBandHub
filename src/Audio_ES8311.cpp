#include "Audio_ES8311.h"
#include <Wire.h>
#include <driver/i2s.h>
#include "esp_err.h"
#include <LittleFS.h>

extern Audio audio;

uint8_t g_volume = 14; // 0-21, persisted across tracks

static i2s_port_t active_i2s_port = I2S_NUM_0;
static bool resolveRootFileCaseInsensitive(const char* wantedPath, char* outPath, size_t outPathSize);

static void toneTestOnPort(i2s_port_t port, uint32_t ms = 2000) {
    Serial.printf("Starting quick tone test on I2S port %d for %u ms...\n", port, ms);
    int16_t sample = 10000;
    size_t bytes_written;
    uint32_t endTime = millis() + ms;
    while (millis() < endTime) {
        int16_t frame[2] = { sample, sample };
        esp_err_t err = i2s_write(port, frame, sizeof(frame), &bytes_written, 50 / portTICK_PERIOD_MS);
        if (err != ESP_OK) {
            Serial.printf("i2s_write returned %s (0x%X)\n", esp_err_to_name(err), err);
            break;
        }
        sample = -sample;
    }
    Serial.println("Tone test finished.");
}

static uint8_t readPCA9555(uint8_t reg) {
    Wire.beginTransmission((uint8_t)0x20);
    Wire.write(reg);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)0x20, (uint8_t)1);
    if (Wire.available()) return Wire.read();
    return 0xFF;
}

static uint8_t readES8311(uint8_t reg) {
    Wire.beginTransmission((uint8_t)0x18);
    Wire.write(reg);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)0x18, (uint8_t)1);
    if (Wire.available()) return Wire.read();
    return 0xFF;
}

static void writeES8311(uint8_t reg, uint8_t val) {
    Wire.beginTransmission((uint8_t)0x18);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

void Audio_SetMute(bool mute) {
    uint8_t cur = readES8311(0x31);
    if (mute) cur |= 0x20; else cur &= ~0x20;
    writeES8311(0x31, cur);
}

// Mute codec, start decode pipeline, wait for DMA to prime, then unmute.
// This eliminates the intermittent crackle caused by stale DMA buffer output
// during the I2S pipeline restart that connecttoFS triggers.
bool Audio_ConnectToFS(fs::FS &fs, const char* path) {
    Audio_SetMute(true);
    bool ok = audio.connecttoFS(fs, path);
    if (ok) {
        delay(80); // ~2 DMA buffer fills at 44100 Hz — enough for the decoder to prime
    }
    Audio_SetMute(false);
    return ok;
}

static void dumpES8311Regs() {
    Serial.println("ES8311 register dump:");
    for (uint8_t r = 0; r <= 0x40; ++r) {
        uint8_t v = readES8311(r);
        Serial.printf("0x%02X: 0x%02X\n", r, v);
    }
}

void Audio_Init() {
    Serial.printf("I2S pins: MCLK=%d BCLK=%d LRCK=%d DOUT=%d DIN=%d\n",
                  BSP_I2S_MCLK, BSP_I2S_SCLK, BSP_I2S_LCLK, BSP_I2S_DOUT, BSP_I2S_DIN);

    // Match factory demo expander setup before codec/I2S playback.
    Wire.beginTransmission((uint8_t)0x20); Wire.write((uint8_t)0x06); Wire.write((uint8_t)0x00); Wire.endTransmission();
    Wire.beginTransmission((uint8_t)0x20); Wire.write((uint8_t)0x07); Wire.write((uint8_t)0x00); Wire.endTransmission();
    Wire.beginTransmission((uint8_t)0x20); Wire.write((uint8_t)0x02); Wire.write((uint8_t)0xFF); Wire.endTransmission();
    Wire.beginTransmission((uint8_t)0x20); Wire.write((uint8_t)0x03); Wire.write((uint8_t)0x00); Wire.endTransmission(); // PA stays off — enabled after LEDs settle
    Serial.printf("PCA9555 init: port0=0x%02X port1=0x%02X\n", readPCA9555(0x02), readPCA9555(0x03));

    // Try to unmount SDMMC to free pins (if mounted)
    bool sd_was_mounted = false;
    if (SD_MMC.begin()) {
        sd_was_mounted = true;
        SD_MMC.end();
        Serial.println("Temporarily unmounted SD_MMC to avoid pin conflicts.");
    }

    // Conservative I2S config
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 44100,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S, // use non-deprecated name
        .intr_alloc_flags = 0,
        .dma_buf_count = 4,
        .dma_buf_len = 128,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .mck_io_num = BSP_I2S_MCLK,
        .bck_io_num = BSP_I2S_SCLK,
        .ws_io_num  = BSP_I2S_LCLK,
        .data_out_num = BSP_I2S_DOUT,
        .data_in_num  = BSP_I2S_DIN
    };

    // Use the library's already-installed I2S port
    uint8_t lib_i2s_port = audio.getI2sPort();
    Serial.printf("Library I2S port: %u\n", lib_i2s_port);

    if (!audio.setPinout(BSP_I2S_SCLK, BSP_I2S_LCLK, BSP_I2S_DOUT, BSP_I2S_DIN, BSP_I2S_MCLK)) {
        Serial.println("audio.setPinout failed");
    } else {
        Serial.println("audio.setPinout succeeded");
    }
    audio.setVolume(g_volume);

    active_i2s_port = (i2s_port_t)lib_i2s_port;

    Serial.printf("Using I2S port %d via library object.\n", active_i2s_port);

    // ES8311 init
    auto wr = [](uint8_t reg, uint8_t val) {
        Wire.beginTransmission((uint8_t)0x18);
        Wire.write(reg);
        Wire.write(val);
        Wire.endTransmission();
    };

    // Reset + power-up sequence aligned to supplier ES8311 driver.
    wr(0x00, 0x1F); delay(20);
    wr(0x00, 0x00);
    wr(0x00, 0x80);

    // Clock + I2S format (slave, 16-bit I2S).
    wr(0x01, 0x3F);
    wr(0x02, 0x00);
    wr(0x03, 0x10);
    wr(0x04, 0x10);
    wr(0x05, 0x00);
    wr(0x06, 0x03);
    wr(0x07, 0x00);
    wr(0x08, 0xFF);
    wr(0x09, 0x0C);
    wr(0x0A, 0x0C);

    // Analog/DAC path power-up and output routing.
    wr(0x0D, 0x01);
    wr(0x0E, 0x02);
    wr(0x12, 0x00);
    wr(0x13, 0x10);
    wr(0x14, 0x1A);
    wr(0x17, 0xC8);
    wr(0x1C, 0x6A);

    // Keep DAC muted during power-up to avoid startup pop; will unmute after silence flush.
    wr(0x31, 0x20); // bit5 = DAC_MUTE
    wr(0x32, 0xB8);
    wr(0x37, 0x08);

    delay(50);

    dumpES8311Regs();

    Serial.printf("PCA9555 port0 output state (reg 0x02): 0x%02X\n", readPCA9555(0x02));
    Serial.printf("PCA9555 port1 output state (reg 0x03): 0x%02X\n", readPCA9555(0x03));

    // Skip startup square-wave stress tone; it sounds like static by design.

    if (sd_was_mounted) {
        if (SD_MMC.begin("/sdcard", true, true)) {
            Serial.println("SD_MMC remounted.");
        } else {
            Serial.println("Failed to remount SD_MMC.");
        }
    }

    // Tell the Audio library about the pinout and volume
    audio.setPinout(BSP_I2S_SCLK, BSP_I2S_LCLK, BSP_I2S_DOUT, BSP_I2S_DIN, BSP_I2S_MCLK);
    audio.setVolume(g_volume);

    // Flush silence into the I2S DMA buffers before unmuting the DAC.
    // This ensures the amp sees a clean zero signal the moment audio is enabled.
    {
        int16_t silence[64] = {};
        size_t bw;
        for (int k = 0; k < 64; k++) {
            i2s_write(active_i2s_port, silence, sizeof(silence), &bw, pdMS_TO_TICKS(10));
        }
    }
    delay(20);

    // Now unmute the DAC — DMA is filled with silence so no pop.
    wr(0x31, 0x00);
    delay(10);

    Serial.println("Audio_Init complete.");
}

void Play_Raw_Hardware_Test() {
    Serial.println("!!! STARTING HARDWARE TONE TEST (manual) !!!");
    int16_t sample = 10000;
    size_t bytes_written;
    uint32_t endTime = millis() + 3000;
    while(millis() < endTime) {
        for(int j = 0; j < 50; j++) {
            int16_t frame[2] = {sample, sample};
            i2s_write(active_i2s_port, frame, 4, &bytes_written, portMAX_DELAY);
        }
        sample = -sample;
    }
    Serial.println("Tone Test Finished.");
}

void Play_Music_test() {
    const char* candidates[] = {"/test.wav", "/test.mp3", "/sdcard/test.wav", "/sdcard/test.mp3"};
    const char* path = nullptr;

    if (!SD_MMC.begin()) {
        Serial.println("SD_MMC begin failed (audio test)");
        return;
    }

    for (const char* candidate : candidates) {
        if (SD_MMC.exists(candidate)) {
            path = candidate;
            break;
        }
    }

    if (!path) {
        Serial.println("No audio test file found on SD card. Place test.wav or test.mp3 in root.");
        return;
    }

    Serial.printf("Audio test file: %s\n", path);
    Serial.println("Attempting to play...");
    audio.setVolume(g_volume);
    bool ok = Audio_ConnectToFS(SD_MMC, path);
    if (!ok) {
        Serial.println("Audio connecttoFS failed.");
    } else {
        Serial.println("Audio playback initiated.");
    }
}

bool Play_Music_file(const char* path) {
    if (!path || strlen(path) == 0) {
        return false;
    }
    if (!isSDReady) {
        Serial.println("Play_Music_file: SD not ready.");
        return false;
    }
    if (!SD_MMC.begin("/sdcard", true, true)) {
        Serial.println("SD_MMC begin failed (audio file)");
        return false;
    }
    const char* candidates[2] = { path, nullptr };
    char prefixed[96] = {0};
    if (strncmp(path, "/sdcard/", 8) != 0) {
        snprintf(prefixed, sizeof(prefixed), "/sdcard%s", path);
        candidates[1] = prefixed;
    }

    for (int i = 0; i < 2; i++) {
        const char* candidate = candidates[i];
        if (!candidate) {
            continue;
        }
        if (!SD_MMC.exists(candidate)) {
            Serial.printf("Theme audio file not found: %s\n", candidate);
            continue;
        }
        Serial.printf("Playing audio file: %s\n", candidate);
        audio.setVolume(g_volume);
        bool ok = Audio_ConnectToFS(SD_MMC, candidate);
        if (!ok) {
            Serial.println("Audio connecttoFS failed.");
            continue;
        }
        Serial.println("Audio playback initiated.");
        return true;
    }

    return false;
}

static bool playResolvedFile(const char* requested) {
    if (Play_Music_file(requested)) {
        return true;
    }

    char resolved[128] = {0};
    if (resolveRootFileCaseInsensitive(requested, resolved, sizeof(resolved))) {
        Serial.printf("Resolved audio file in SD root: %s\n", resolved);
        if (Play_Music_file(resolved)) {
            return true;
        }
    }

    return false;
}

bool Play_Default_Band_Chime() {
    if (!isSDReady) {
        Serial.println("SD not ready — using LittleFS fallback chime.");
        return Play_Fallback_Chime();
    }

    const char* candidates[] = {
        "/MB_chime.wav",
        "/Chime.wav",
        "/chime.wav",
        "/success.mp3",
        "/test.mp3"
    };

    for (const char* candidate : candidates) {
        Serial.printf("Default band chime request: %s\n", candidate);
        if (playResolvedFile(candidate)) {
            return true;
        }
    }

    Serial.println("Default band chime missing on SD — using LittleFS fallback.");
    return Play_Fallback_Chime();
}

bool Play_Fallback_Chime() {
    const char* candidates[] = {"/MB_chime.wav", "/success.mp3"};
    for (const char* c : candidates) {
        if (LittleFS.exists(c)) {
            Serial.printf("LittleFS fallback chime: %s\n", c);
            audio.setVolume(g_volume);
            return Audio_ConnectToFS(LittleFS, c);
        }
    }
    Serial.println("No fallback chime found in LittleFS.");
    return false;
}

static bool resolveRootFileCaseInsensitive(const char* wantedPath, char* outPath, size_t outPathSize) {
    if (!wantedPath || !outPath || outPathSize < 4) {
        return false;
    }

    const char* base = strrchr(wantedPath, '/');
    base = base ? (base + 1) : wantedPath;
    if (!base || strlen(base) == 0) {
        return false;
    }

    File root = SD_MMC.open("/");
    if (!root) {
        return false;
    }

    File f = root.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            const char* entryName = f.name();
            const char* entryBase = strrchr(entryName, '/');
            entryBase = entryBase ? (entryBase + 1) : entryName;
            if (entryBase && strcasecmp(entryBase, base) == 0) {
                snprintf(outPath, outPathSize, "%s", entryName);
                f.close();
                root.close();
                return true;
            }
        }
        f = root.openNextFile();
    }

    root.close();
    return false;
}

bool Play_Music_theme(uint16_t themeId) {
    // Theme ID 7 is "Boo To You" from web_portal.cpp.
    const bool isBooToYou = (themeId == 7);
    const char* requested = isBooToYou ? "/bootoyou.mp3" : "/test.mp3";

    Serial.printf("Theme playback request: themeId=%u -> %s\n", themeId, requested);
    if (isSDReady && playResolvedFile(requested)) {
        return true;
    }

    Serial.println("Theme file missing or no SD — using LittleFS fallback chime.");
    return Play_Fallback_Chime();
}

void Music_stop() {
    audio.stopSong();
}

void Music_set_volume(uint8_t vol) {
    if (vol > 21) vol = 21;
    g_volume = vol;
    audio.setVolume(g_volume);
    Serial.printf("Volume set to %u\n", g_volume);
}

void Audio_Loop() {
    audio.loop();
}

void Music_pause() { audio.pauseResume(); }
void Music_resume() { audio.pauseResume(); }
