#pragma once
#include "Arduino.h"
#include "Audio.h"
#include "SD_Card.h"

// Waveshare factory_01 mapping (ESP32-S3-AUDIO-Board)
#define BSP_I2S_MCLK   12
#define BSP_I2S_SCLK   13
#define BSP_I2S_LCLK   14
#define BSP_I2S_DOUT   16
#define BSP_I2S_DIN    15


void Audio_Init(); 
void Audio_Loop();
void Play_Music_test();
bool Play_Music_file(const char* path);
bool Play_Default_Band_Chime();
bool Play_Music_theme(uint16_t themeId);
void Music_pause(); 
void Music_resume();
void Play_Raw_Hardware_Test();