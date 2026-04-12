#pragma once
#include "Arduino.h"
#include "Audio.h"
#include "SD_Card.h"

// Corrected Pins from your Screenshot
#define BSP_I2S_MCLK   12
#define BSP_I2S_SCLK   13
#define BSP_I2S_LCLK   14
#define BSP_I2S_DOUT   16
#define BSP_I2S_DIN    15

void Audio_Init(); 
void Audio_Loop();
void Play_Music_test();
void Music_pause(); 
void Music_resume();
void Play_Raw_Hardware_Test();