// SD_Diagnostic.ino — console-only microSD probe for Waveshare ESP32-S3-LCD-1.47
//
// No display. Prints everything to USB Serial so you can copy/paste the result.
// Tries to mount the card in several SDMMC modes/speeds and reports which works,
// plus card type and size. Use this to pin down "NO SD CARD".
//
// Board: ESP32S3 Dev Module, USB CDC On Boot: Enabled, PSRAM: OPI PSRAM,
//   Flash 16MB. Open Serial Monitor at 115200 baud.
#include <Arduino.h>
#include <SD_MMC.h>

// SDMMC pins (Waveshare wiki)
#define SD_CLK 14
#define SD_CMD 15
#define SD_D0  16
#define SD_D1  18
#define SD_D2  17
#define SD_D3  21

static const char *cardTypeStr() {
  switch (SD_MMC.cardType()) {
    case CARD_NONE: return "NONE";
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC/SDXC";
    default:        return "UNKNOWN";
  }
}

static bool tryMount(const char *label, bool oneBit, int khz) {
  SD_MMC.end();
  delay(40);
  bool pinsOk = oneBit ? SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0)
                       : SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
  if (!pinsOk) { Serial.printf("  %s : setPins FAILED\n", label); return false; }
  bool ok = SD_MMC.begin("/sdcard", oneBit, false, khz);
  Serial.printf("  %s : %s\n", label, ok ? "PASS" : "FAIL");
  return ok;
}

void setup() {
  Serial.begin(115200);
  // Native USB: wait a bit for the host to open the port so nothing is lost.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) delay(10);
  delay(600);

  Serial.println();
  Serial.println("===== SD DIAGNOSTIC =====");
  Serial.println("Board: Waveshare ESP32-S3-LCD-1.47");
  Serial.printf ("Core:  esp-arduino %d.%d.%d\n",
                 ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
  Serial.printf ("IDF:   %s\n", esp_get_idf_version());
  Serial.println("Pins:  CLK=14 CMD=15 D0=16 D1=18 D2=17 D3=21");
  Serial.println("-------------------------");
  Serial.println("Mount attempts:");

  struct { const char *l; bool one; int khz; } tests[] = {
    { "4-bit  MAX  ", false, BOARD_MAX_SDMMC_FREQ },
    { "4-bit  20MHz", false, SDMMC_FREQ_DEFAULT   },
    { "4-bit  probe", false, SDMMC_FREQ_PROBING   },
    { "1-bit  20MHz", true,  SDMMC_FREQ_DEFAULT   },
    { "1-bit  probe", true,  SDMMC_FREQ_PROBING   },
  };

  bool mounted = false;
  for (auto &t : tests) {
    if (tryMount(t.l, t.one, t.khz)) { mounted = true; break; }
  }

  Serial.println("-------------------------");
  if (mounted) {
    Serial.printf("RESULT: CARD OK\n");
    Serial.printf("  Type      : %s\n", cardTypeStr());
    Serial.printf("  Card size : %llu MB\n", (unsigned long long)(SD_MMC.cardSize()  / (1024ULL * 1024ULL)));
    Serial.printf("  Total     : %llu MB\n", (unsigned long long)(SD_MMC.totalBytes()/ (1024ULL * 1024ULL)));
    Serial.printf("  Used      : %llu MB\n", (unsigned long long)(SD_MMC.usedBytes() / (1024ULL * 1024ULL)));
  } else {
    Serial.println("RESULT: ALL MOUNTS FAILED");
    Serial.println("  -> card not answering in any mode.");
    Serial.println("  -> check: fully seated, FAT32, try a <=32GB card.");
  }
  Serial.println("=========================");
}

void loop() {
  delay(2000);
  Serial.println("(idle - reset the board to re-run the test)");
}
