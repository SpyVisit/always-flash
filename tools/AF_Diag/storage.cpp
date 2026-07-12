// storage.cpp — async SD writer for Waveshare ESP32-S3-LCD-1.47 (SD_MMC, 4-bit)
#include "storage.h"
#include <SD_MMC.h>

// ---- microSD (SDMMC) pins, per Waveshare wiki ----
#define SD_CLK 14
#define SD_CMD 15
#define SD_D0  16
#define SD_D1  18
#define SD_D2  17
#define SD_D3  21

// ---- ring buffer (PSRAM) ----
// Sized so that even a pathological multi-second card stall cannot make the
// receiver wait. At ~100 KB/s inbound, 2 MB = ~20 s of slack.
#define RING_SIZE   (2 * 1024 * 1024)
#define WRITE_CHUNK (16 * 1024)     // how much the writer hands the card at once

static uint8_t         *g_ring = nullptr;
static volatile size_t  g_head = 0;      // producer (receive task) writes this
static volatile size_t  g_tail = 0;      // consumer (writer task) writes this
static volatile bool    g_writeErr = false;
static volatile bool    g_draining = false;   // finalize asked the writer to finish

static File          g_file;
static SemaphoreHandle_t g_fileMux = nullptr;  // guards g_file open/close vs writer
static TaskHandle_t  g_writer = nullptr;
static char          g_tempPath[24];

// ---- ring helpers (single producer, single consumer: no lock needed) ----
static inline size_t ringUsed() {
  size_t h = g_head, t = g_tail;
  return (h >= t) ? (h - t) : (RING_SIZE - t + h);
}
static inline size_t ringFree() { return RING_SIZE - ringUsed() - 1; }

// ---- the writer task: lives on the other core, allowed to block ----
static void writerTask(void *) {
  for (;;) {
    size_t used = ringUsed();

    // Nothing to do (or no file open): idle politely.
    if (used == 0) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }

    // Wait until a decent chunk has piled up, unless we're draining at EOF.
    if (used < WRITE_CHUNK && !g_draining) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }

    xSemaphoreTake(g_fileMux, portMAX_DELAY);
    if (g_file) {
      size_t t     = g_tail;
      size_t chunk = used;
      if (chunk > WRITE_CHUNK) chunk = WRITE_CHUNK;
      // don't wrap past the end of the ring in one write
      if (t + chunk > RING_SIZE) chunk = RING_SIZE - t;

      size_t n = g_file.write(g_ring + t, chunk);   // <-- may block for 10-200 ms
      if (n != chunk) g_writeErr = true;
      g_tail = (t + n) % RING_SIZE;
    }
    xSemaphoreGive(g_fileMux);
  }
}

bool storageBegin() {
  if (!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3)) return false;

  bool ok = SD_MMC.begin("/sdcard", false, false, BOARD_MAX_SDMMC_FREQ);
  if (!ok) { SD_MMC.end(); delay(50);
             ok = SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT); }
  if (!ok) { SD_MMC.end(); delay(50);
             ok = SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_PROBING); }
  if (!ok) return false;

  if (!g_ring) {
    g_ring = (uint8_t *)ps_malloc(RING_SIZE);
    if (!g_ring) return false;              // PSRAM is required for the ring
  }
  g_head = g_tail = 0;
  g_writeErr = false;

  if (!g_fileMux) g_fileMux = xSemaphoreCreateMutex();

  storageCleanTemps();

  if (!g_writer) {
    // Pin the writer to core 0. Arduino's loop() (our receiver) runs on core 1,
    // so the SD stalls happen on a core that nobody is waiting on.
    xTaskCreatePinnedToCore(writerTask, "sdwriter", 4096, nullptr,
                            1 /* low priority */, &g_writer, 0);
    if (!g_writer) return false;
  }
  return true;
}

// ---- temp-file lifecycle ----
bool storageOpenTemp() {
  static uint16_t ctr = 0;
  for (int tries = 0; tries < 1000; tries++) {
    snprintf(g_tempPath, sizeof(g_tempPath), "/~t%05u.par",
             (unsigned)((millis() + ctr++) % 100000));
    if (!SD_MMC.exists(g_tempPath)) break;
  }

  xSemaphoreTake(g_fileMux, portMAX_DELAY);
  g_head = g_tail = 0;
  g_writeErr = false;
  g_draining = false;
  g_file = SD_MMC.open(g_tempPath, FILE_WRITE);
  bool ok = (bool)g_file;
  xSemaphoreGive(g_fileMux);
  return ok;
}

// NON-BLOCKING producer. Only touches RAM. Called from the receive task.
bool storageWrite(const uint8_t *data, size_t len) {
  if (!g_ring || g_writeErr) return false;

  // The ring is huge, so this should never actually spin. If it somehow does
  // (card wedged for many seconds), we wait rather than lose data.
  uint32_t t0 = millis();
  while (ringFree() < len) {
    if (millis() - t0 > 10000) return false;   // writer is truly stuck
    vTaskDelay(1);
  }

  size_t h = g_head;
  size_t first = RING_SIZE - h;
  if (first > len) first = len;
  memcpy(g_ring + h, data, first);
  if (len > first) memcpy(g_ring, data + first, len - first);  // wrap
  g_head = (h + len) % RING_SIZE;
  return true;
}

size_t storageBacklog() { return ringUsed(); }
bool   storageError()   { return g_writeErr; }

static void splitName(const char *name, char *base, size_t bsz, char *ext, size_t esz) {
  const char *dot = strrchr(name, '.');
  if (dot && dot != name) {
    size_t blen = (size_t)(dot - name);
    if (blen >= bsz) blen = bsz - 1;
    memcpy(base, name, blen); base[blen] = 0;
    snprintf(ext, esz, "%s", dot);
  } else {
    snprintf(base, bsz, "%s", name);
    ext[0] = 0;
  }
}

bool storageFinalize(const char *desiredName, char *finalNameOut, size_t outSize) {
  // Let the writer drain whatever is left in the ring.
  g_draining = true;
  uint32_t t0 = millis();
  while (ringUsed() > 0 && !g_writeErr) {
    if (millis() - t0 > 30000) break;          // don't hang forever
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  g_draining = false;
  if (g_writeErr) { storageAbort(); return false; }

  xSemaphoreTake(g_fileMux, portMAX_DELAY);
  if (!g_file) { xSemaphoreGive(g_fileMux); return false; }
  g_file.flush();
  g_file.close();
  xSemaphoreGive(g_fileMux);

  char base[80], ext[24], cand[128];
  splitName(desiredName, base, sizeof(base), ext, sizeof(ext));
  snprintf(cand, sizeof(cand), "/%s%s", base, ext);
  for (int idx = 1; SD_MMC.exists(cand); idx++) {
    snprintf(cand, sizeof(cand), "/%s_%d%s", base, idx, ext);
    if (idx > 100000) return false;
  }

  if (!SD_MMC.rename(g_tempPath, cand)) return false;
  snprintf(finalNameOut, outSize, "%s", cand[0] == '/' ? cand + 1 : cand);
  g_tempPath[0] = 0;
  return true;
}

void storageAbort() {
  g_draining = false;
  xSemaphoreTake(g_fileMux, portMAX_DELAY);
  g_head = g_tail = 0;                 // drop everything still buffered
  if (g_file) g_file.close();
  xSemaphoreGive(g_fileMux);
  if (g_tempPath[0]) { SD_MMC.remove(g_tempPath); g_tempPath[0] = 0; }
  g_writeErr = false;
}

// ---- housekeeping / info ----
int storageCleanTemps() {
  File root = SD_MMC.open("/");
  if (!root) return 0;
  char victims[16][24];
  int  n = 0;
  for (File f = root.openNextFile(); f && n < 16; f = root.openNextFile()) {
    if (!f.isDirectory()) {
      const char *nm = f.name(), *b = nm;
      for (const char *q = nm; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
      size_t len = strlen(b);
      if (b[0] == '~' && b[1] == 't' && len > 4 && !strcmp(b + len - 4, ".par")) {
        snprintf(victims[n], sizeof(victims[n]), "/%s", b);
        n++;
      }
    }
    f.close();
  }
  root.close();
  int removed = 0;
  for (int i = 0; i < n; i++) if (SD_MMC.remove(victims[i])) removed++;
  return removed;
}

uint64_t storageTotalBytes() { return SD_MMC.totalBytes(); }
uint64_t storageUsedBytes()  { return SD_MMC.usedBytes(); }

int storageListFiles(FileEntry *out, int maxN) {
  File root = SD_MMC.open("/");
  if (!root) return 0;
  int n = 0;
  for (File f = root.openNextFile(); f && n < maxN; f = root.openNextFile()) {
    if (!f.isDirectory()) {
      const char *nm = f.name(), *b = nm;
      for (const char *q = nm; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
      if (b[0] != '~') {
        snprintf(out[n].name, sizeof(out[n].name), "%s", b);
        out[n].size = (uint32_t)f.size();
        n++;
      }
    }
    f.close();
  }
  root.close();
  return n;
}
