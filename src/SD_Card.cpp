#include "SD_Card.h"

bool isSDReady = false;

uint16_t SDCard_Size = 0;
uint16_t Flash_Size = 0;

void SD_Init() {
  // Configure SD MMC in 1-bit mode using specified pins
  if(!SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, -1, -1, -1)){
    Serial.println("SD MMC: Pin configuration failed!");
    return;
  }

  // Mount the SD card at /sdcard
  if (SD_MMC.begin("/sdcard", true, true)) {                
    Serial.println("SD card initialization successful!");
    isSDReady = true;
  } else {
    Serial.println("SD card initialization failed!");
    isSDReady = false;
    return;
  }

  uint8_t cardType = SD_MMC.cardType();
  if(cardType == CARD_NONE){
    Serial.println("No SD card attached");
  } else {
    uint64_t cardBytes = SD_MMC.cardSize();
    SDCard_Size = (uint16_t)min((uint64_t)65535ULL, cardBytes / (1024ULL * 1024ULL));
    Serial.printf("SD Card Size: %llu MB\n", cardBytes / (1024ULL * 1024ULL));
  }
}

// Logic for searching files
bool File_Search(const char* directory, const char* fileName) {
  File path = SD_MMC.open(directory);
  if (!path) return false;
  File file = path.openNextFile();
  while (file) {
    if (strcmp(file.name(), fileName) == 0) {
      path.close();
      return true;
    }
    file = path.openNextFile();
  }
  path.close();
  return false;
}

uint16_t Folder_retrieval(const char* directory, const char* fileExtension, char File_Name[][100], uint16_t maxFiles) {
  File path = SD_MMC.open(directory);
  if (!path) return 0;
  
  uint16_t fileCount = 0;
  File file = path.openNextFile();
  while (file && fileCount < maxFiles) {
    if (!file.isDirectory() && strstr(file.name(), fileExtension)) {
      strncpy(File_Name[fileCount], file.name(), 99);
      fileCount++;
    }
    file = path.openNextFile();
  }
  path.close();
  return fileCount;
}

void Flash_test() {
  uint32_t flashSize = ESP.getFlashChipSize();
  Flash_Size = flashSize/1024/1024;
  Serial.printf("Internal Flash size: %d MB\n", Flash_Size);
}