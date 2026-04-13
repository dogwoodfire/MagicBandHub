#include <Arduino.h>
#include <Wire.h>
#include "Audio_ES8311.h"
#include "SD_Card.h"

#define AUDIO_I2C_SDA 11
#define AUDIO_I2C_SCL 10
#define AUDIO_EXPANDER_ADDR 0x20
#define AUDIO_EXPANDER_DIR_PORT0 0x06
#define AUDIO_EXPANDER_DIR_PORT1 0x07
#define AUDIO_EXPANDER_OUT_PORT0 0x02
#define AUDIO_EXPANDER_OUT_PORT1 0x03
#define AUDIO_EXPANDER_PORT0_INIT 0xFF
#define AUDIO_EXPANDER_PORT1_INIT 0xFF // factory_01 enables EXIO8 high before playback

// Factory demo uses I2S1 on this board.
Audio audio(false, 3, I2S_NUM_1);

void audio_info(const char *info) { Serial.print("AUDIO_INFO: "); Serial.println(info); }
void audio_eof_mp3(const char *info) { Serial.print("AUDIO_EOF: "); Serial.println(info); }

static void initAudioHardware() {
  Wire.begin(AUDIO_I2C_SDA, AUDIO_I2C_SCL);
  Wire.setTimeOut(250);
  Serial.println("I2C initialized for audio devices.");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== ESP32-S3 Audio Board Test ===");

  initAudioHardware();
  SD_Init();

  // Give decoder/file I/O more headroom to reduce stutter.
  audio.setBufsize(4096, 65536);
  delay(50);

  // Initialize I2S + codec (this function installs driver and runs tone test)
  Audio_Init();

  // Small delay and heap check before starting playback
  delay(200);
  Serial.printf("Library I2S port (audio.getI2sPort): %u\n", audio.getI2sPort());

  Serial.printf("Free heap before playback: %u\n", ESP.getFreeHeap());

  // Only attempt playback if there is reasonable free heap
  if (ESP.getFreeHeap() > 120000) {
    Play_Music_test();
  } else {
    Serial.println("Not enough free heap to start playback safely. Increase PSRAM or reduce buffer sizes.");
  }
}

void loop() {
  Audio_Loop();
}
