#include "Audio_ES8311.h"
#include <Wire.h>
#include <LittleFS.h>
#include <driver/i2s.h>

extern Audio audio;

void Audio_Init() {
  // Map Pins (MCLK=12, SCLK=13, LRCK=14, DOUT=16)
  audio.setPinout(BSP_I2S_SCLK, BSP_I2S_LCLK, BSP_I2S_DOUT, BSP_I2S_MCLK);
  audio.setVolume(21); 

  auto writeReg = [](uint8_t reg, uint8_t val) {
    Wire.beginTransmission(0x18);
    Wire.write(reg); Wire.write(val);
    uint8_t err = Wire.endTransmission();
    if (err == 0) Serial.printf("Codec: Reg 0x%02X -> 0x%02X [OK]\n", reg, val);
  };

  Serial.println("--- CODEC POWER-ON SEQUENCE ---");
  
  writeReg(0x45, 0x00); delay(10); // System Reset
  writeReg(0x01, 0x30);            // Enable Clock Manager
  
  // CRITICAL FIX: 0x00 is POWER UP. (0x10 was Power Down).
  writeReg(0x02, 0x00);            // Digital Power ON
  writeReg(0x03, 0x00);            // Analog Power ON
  
  writeReg(0x0D, 0x02);            // Standard I2S Format
  writeReg(0x0E, 0x02);            // 16-bit Data
  writeReg(0x0F, 0x44);            // Slave Mode (Locks to ESP32 clock)
  writeReg(0x14, 0x1A);            // DAC Setup
  writeReg(0x16, 0x00);            // Internal Unmute
  writeReg(0x17, 0x00);            // System Unmute
  writeReg(0x31, 0x00);            // Hardware Path Unmute
  writeReg(0x32, 0x00);            // DAC Volume to 0dB (Loudest)
  
  Serial.println("--- CODEC READY ---");
}

// Bypasses MP3 logic to verify if the I2S pins are even pulsing
void Play_Raw_Hardware_Test() {
    audio.stopSong(); 
    Serial.println("!!! HARDWARE TEST: Sending Raw 440Hz Square Wave...");

    int16_t sample = 20000; // High amplitude
    size_t bytes_written;
    uint32_t endTime = millis() + 3000;
    
    while(millis() < endTime) {
        for(int j = 0; j < 50; j++) {
            int16_t frame[2] = {sample, sample}; 
            i2s_write(I2S_NUM_0, frame, 4, &bytes_written, portMAX_DELAY);
        }
        sample = -sample; 
    }
    Serial.println("Hardware Test Finished.");
}

void Play_Music_test() {
  if (LittleFS.exists("/success.mp3")) {
    audio.connecttoFS(LittleFS, "/success.mp3");
  }
}

void Audio_Loop() { audio.loop(); }