// SD_WriteBench.ino — isolate the SD card from USB entirely.
//
// WHY: transfers die at ~10 MB / ~2 min at full speed. Either the card stalls,
// or our USB path breaks. This test removes USB from the equation: it writes a
// file using exactly the same pattern as the YMODEM receiver (1 KB blocks into
// a buffer, flushed in chunks), but the data comes from RAM. If the stall
// reproduces here, the card is the culprit and no amount of protocol tuning
// will help — we'd need to absorb stalls (big ring buffer + writer on core 1).
//
// Console only. Board: ESP32S3 Dev Module, USB CDC On Boot: Enabled,
// PSRAM: OPI PSRAM, Flash 16MB. Serial Monitor @ 115200.
#include <Arduino.h>
#include <SD_MMC.h>

#define SD_CLK 14
#define SD_CMD 15
#define SD_D0  16
#define SD_D1  18
#define SD_D2  17
#define SD_D3  21

#define TEST_MB      24          // write this many MB (we die around 10, so 24 is plenty)
#define BLOCK        1024        // same as a YMODEM 1K block
#define FLUSH_AT     (8 * 1024)  // same chunking as the firmware

static uint8_t  block[BLOCK];
static uint8_t *wbuf = nullptr;
static size_t   wlen = 0;

// latency histogram (ms buckets)
static uint32_t hist[8] = {0};    // <2, <5, <10, <25, <50, <100, <250, >=250
static const char *bucket[8] = {"  <2ms", "  <5ms", " <10ms", " <25ms",
                                " <50ms", "<100ms", "<250ms", ">250ms"};
static void record(uint32_t ms) {
  if      (ms <   2) hist[0]++;
  else if (ms <   5) hist[1]++;
  else if (ms <  10) hist[2]++;
  else if (ms <  25) hist[3]++;
  else if (ms <  50) hist[4]++;
  else if (ms < 100) hist[5]++;
  else if (ms < 250) hist[6]++;
  else               hist[7]++;
}

static bool mountCard() {
  if (!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3)) return false;
  if (SD_MMC.begin("/sdcard", false, false, BOARD_MAX_SDMMC_FREQ)) return true;
  SD_MMC.end(); delay(50);
  return SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT);
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 4000) delay(10);
  delay(600);

  Serial.println("\n===== SD WRITE BENCHMARK =====");
  Serial.println("Mimics the YMODEM write pattern, but with NO USB involved.");
  Serial.printf ("Plan: %d MB, %d-byte blocks, flush every %d KB\n\n",
                 TEST_MB, BLOCK, FLUSH_AT / 1024);

  if (!mountCard()) { Serial.println("MOUNT FAILED"); return; }
  Serial.printf("Card: %llu MB, used %llu MB\n\n",
                (unsigned long long)(SD_MMC.cardSize()  / (1024ULL * 1024ULL)),
                (unsigned long long)(SD_MMC.usedBytes() / (1024ULL * 1024ULL)));

  wbuf = (uint8_t *)ps_malloc(FLUSH_AT);
  if (!wbuf) { Serial.println("PSRAM alloc failed"); return; }
  Serial.printf("Write buffer in PSRAM: %d KB\n\n", FLUSH_AT / 1024);

  for (size_t i = 0; i < BLOCK; i++) block[i] = (uint8_t)i;

  SD_MMC.remove("/bench.tmp");
  File f = SD_MMC.open("/bench.tmp", FILE_WRITE);
  if (!f) { Serial.println("OPEN FAILED"); return; }

  const uint32_t totalBlocks = (uint32_t)TEST_MB * 1024;
  uint32_t worst = 0, worstAtKB = 0;
  uint32_t t0 = millis(), lastReport = t0, reportBytes = 0;
  uint64_t written = 0;

  Serial.println("  MB    inst KB/s   worst-flush-ms   (watching for a cliff)");
  Serial.println("  ----------------------------------------------------------");

  for (uint32_t b = 0; b < totalBlocks; b++) {
    memcpy(wbuf + wlen, block, BLOCK);
    wlen += BLOCK;

    if (wlen >= FLUSH_AT) {
      uint32_t ta = millis();
      size_t n = f.write(wbuf, wlen);
      uint32_t dt = millis() - ta;
      record(dt);
      if (dt > worst) { worst = dt; worstAtKB = (uint32_t)(written / 1024); }
      if (n != wlen) { Serial.printf("\n!! SHORT WRITE at %llu KB (%u of %u)\n",
                                     (unsigned long long)(written/1024), (unsigned)n, (unsigned)wlen); break; }
      written += wlen;
      reportBytes += wlen;
      wlen = 0;
    }

    // progress line every second
    uint32_t now = millis();
    if (now - lastReport >= 1000) {
      float kbps = reportBytes * 1000.0f / (now - lastReport) / 1024.0f;
      Serial.printf("  %4.1f   %8.1f   %10u\n",
                    written / 1048576.0, kbps, (unsigned)worst);
      lastReport = now; reportBytes = 0; worst = 0;   // worst per-second window
    }
  }

  uint32_t tclose = millis();
  if (wlen) f.write(wbuf, wlen);
  f.flush();
  f.close();
  uint32_t closeMs = millis() - tclose;
  uint32_t total   = millis() - t0;

  Serial.println("\n===== RESULTS =====");
  Serial.printf("Wrote        : %.1f MB\n", written / 1048576.0);
  Serial.printf("Total time   : %.1f s\n", total / 1000.0);
  Serial.printf("Average      : %.1f KB/s\n", written / 1024.0 / (total / 1000.0));
  Serial.printf("close+flush  : %u ms\n", (unsigned)closeMs);
  Serial.printf("Worst flush  : %u ms (at ~%u KB in)\n", (unsigned)worst, (unsigned)worstAtKB);

  Serial.println("\nFlush latency histogram:");
  uint32_t tot = 0; for (int i = 0; i < 8; i++) tot += hist[i];
  for (int i = 0; i < 8; i++) {
    if (!hist[i]) continue;
    Serial.printf("  %s : %6u  (%4.1f%%)", bucket[i], (unsigned)hist[i],
                  100.0 * hist[i] / (tot ? tot : 1));
    int bars = (int)(40.0 * hist[i] / (tot ? tot : 1));
    Serial.print("  ");
    for (int j = 0; j < bars; j++) Serial.print('#');
    Serial.println();
  }

  Serial.println("\nINTERPRETATION:");
  Serial.println("  If you see flushes >100ms, the card DOES stall. At ~100 KB/s");
  Serial.println("  incoming, a 250ms stall needs ~25 KB of USB buffer to survive;");
  Serial.println("  a 1s stall needs ~100 KB. That is why the fix is a big PSRAM");
  Serial.println("  ring buffer with the SD write on the OTHER CORE, so the USB");
  Serial.println("  read never waits for the card at all.");
  Serial.println("  If ALL flushes are fast and speed is flat: the card is fine,");
  Serial.println("  and the bug is in our USB/protocol path instead.");

  SD_MMC.remove("/bench.tmp");
  Serial.println("\n(bench.tmp removed. Reset to re-run.)");
}

void loop() { delay(5000); }
