// storage.h — asynchronous SD sink for ALWAYS FLASH
//
// ARCHITECTURE (this is the whole point):
// The SD card takes 10-25 ms per flush, occasionally ~200 ms. The USB-CDC
// receiver must NEVER wait for that — while it is blocked, the incoming block
// is dropped by the driver and the transfer corrupts. (Measured: the card
// itself sustains 1000 KB/s, so the card was never the bottleneck — our
// blocking on it was.)
//
// So the card write runs in its own FreeRTOS task, pinned to the OTHER core:
//
//   receive task -> storageWrite() -> [ ring buffer in PSRAM ] -> writer task -> SD
//   (never blocks)                     (megabytes of slack)       (may block freely)
//
// storageWrite() only memcpy's into the ring and returns immediately. A 2 MB
// ring at ~100 KB/s absorbs a card stall of ~20 seconds without a hiccup.
#pragma once
#include <Arduino.h>

bool storageBegin();          // mount card + start the writer task
int  storageCleanTemps();     // remove orphan ~t*.par partials

bool storageOpenTemp();       // begin a new file (writer drains into it)
bool storageWrite(const uint8_t *data, size_t len);   // NON-BLOCKING: ring only
bool storageFinalize(const char *desiredName, char *finalNameOut, size_t outSize);
void storageAbort();

// --- introspection (for the UI) ---
size_t   storageBacklog();    // bytes in the ring, not yet on the card
bool     storageError();      // a write failed inside the writer task
uint64_t storageTotalBytes();
uint64_t storageUsedBytes();  // walks the FAT — call sparingly

struct FileEntry { char name[64]; uint32_t size; };
int storageListFiles(FileEntry *out, int maxN);
