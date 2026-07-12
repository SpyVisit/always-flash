// AlwaysFlash.ino — ALWAYS FLASH 2.0  (base build, no buttons)
// append-only SD "flash drive" over USB-CDC (YMODEM)
// Target: Waveshare ESP32-S3-LCD-1.47 (ESP32-S3R8, ST7789 172x320, SDMMC card)
//
// The host can ONLY add files. The device never presents USB Mass Storage, so
// the OS cannot mount the card — deletion requires pulling the microSD.
//
// Channels:
//   Serial   = native USB CDC -> YMODEM data channel (host sends files here)
//   Display  = ST7789 172x320 -> status, per-file progress, card-fullness bar
//   RGB LED  = GPIO38          -> ambient state indicator
//
// Arduino IDE: board "ESP32S3 Dev Module", USB CDC On Boot: Enabled,
//   PSRAM: OPI PSRAM, Flash Size: 16MB.  Library: LovyanGFX.
#include <Arduino.h>
#include "LGFX_Config.h"
#include "ymodem.h"
#include "storage.h"

#define AF_BUILD 9          // bump this on every firmware revision
#define RGB_PIN  38         // onboard WS2812 RGB LED

static LGFX        lcd;
static LGFX_Sprite canvas(&lcd);

// 565 colors defined locally (avoids clashes with library color macros)
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GREEN  0x07E0
#define C_RED    0xF800
#define C_GRAY   0x7BEF
#define C_AMBER  0xFD20
#define C_BLUE   0x03FF

static char        g_curName[128] = "";
static uint32_t    g_curSize = 0, g_curRecv = 0;
static int         g_count = 0;
static bool        g_receiving = false;
static const char *g_state = "INIT";
static uint32_t    g_lastDraw = 0;
static uint32_t    g_stateSince = 0;   // when a terminal status was set (0 = none)

// --- transfer rate / ETA (sampled, not averaged from the start) ---
static uint32_t    g_startMs   = 0;    // when this file's transfer began
static uint32_t    g_rateMs    = 0;    // last sample time
static uint32_t    g_rateBytes = 0;    // bytes at last sample
static float       g_bps       = 0;    // smoothed bytes/sec
static uint64_t    g_total = 0, g_used = 0;

// ============================ helpers ============================

static void sanitize(char *s) {
  char *p = s, *base = s;
  for (; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
  if (base != s) memmove(s, base, strlen(base) + 1);
  for (p = s; *p; p++)
    if (strchr("\\/:*?\"<>|", *p) || (unsigned char)*p < 0x20) *p = '_';
  if (s[0] == 0) strcpy(s, "noname");
}

// Map the current state to the RGB LED (kept dim so it doesn't blind).
static void setLed() {
  if (g_receiving)                         rgbLedWrite(RGB_PIN, 30, 18, 0);   // amber
  else if (!strcmp(g_state, "SAVED"))      rgbLedWrite(RGB_PIN, 0, 40, 0);    // green
  else if (!strncmp(g_state, "ERR", 3) ||
           !strcmp(g_state, "DISK FULL") ||
           !strcmp(g_state, "NO SD CARD")) rgbLedWrite(RGB_PIN, 45, 0, 0);    // red
  else                                     rgbLedWrite(RGB_PIN, 0, 0, 30);    // blue idle
}

static void drawBar(int x, int y, int w, int h, float frac, uint16_t col) {
  if (frac < 0) frac = 0; if (frac > 1) frac = 1;
  canvas.drawRect(x, y, w, h, C_WHITE);
  canvas.fillRect(x + 1, y + 1, (int)((w - 2) * frac), h - 2, col);
}

// ============================ drawing ============================

static void drawUI(bool force) {
  uint32_t now = millis();
  // Throttle redraws: each push sends the whole 320x172 sprite over SPI, which
  // sits right between incoming blocks and delays our ACK. Keep it rare.
  if (!force && now - g_lastDraw < 250) return;
  g_lastDraw = now;

  canvas.fillScreen(C_BLACK);

  // header (fits on one line in landscape)
  canvas.setTextColor(C_GREEN, C_BLACK);
  canvas.setTextSize(2); canvas.setCursor(6, 6); canvas.printf("ALWAYS FLASH 2.%d", AF_BUILD);
  canvas.drawFastHLine(0, 30, 320, C_GRAY);

  // --- status line (size 2) with the ETA right beside it ---
  canvas.setTextSize(2);
  canvas.setTextColor(C_WHITE, C_BLACK);
  canvas.setCursor(6, 38);
  canvas.printf("St: %s", g_state);

  // ETA sits on the same big line, amber, right-aligned area
  if (g_receiving && g_curSize > 0 && g_bps > 100) {
    uint32_t left = (g_curSize > g_curRecv) ? (g_curSize - g_curRecv) : 0;
    uint32_t eta  = (uint32_t)(left / g_bps);
    char buf[16];
    if      (eta >= 3600) snprintf(buf, sizeof buf, "%lu:%02lu:%02lu",
                                   (unsigned long)(eta/3600), (unsigned long)((eta%3600)/60), (unsigned long)(eta%60));
    else                  snprintf(buf, sizeof buf, "%lu:%02lu",
                                   (unsigned long)(eta/60), (unsigned long)(eta%60));
    // right-align: size-2 glyphs are 12 px wide
    int w = strlen(buf) * 12;
    canvas.setTextColor(C_AMBER, C_BLACK);
    canvas.setCursor(314 - w, 38);
    canvas.print(buf);
  }

  // --- file name (size 1, white) ---
  canvas.setTextSize(1);
  canvas.setTextColor(C_WHITE, C_BLACK);
  if (g_curName[0]) {
    canvas.setCursor(6, 60);
    canvas.printf("%s%.40s", g_receiving ? "" : "Last: ", g_curName);
  } else {
    canvas.setCursor(6, 60);
    canvas.printf("Saved this session: %d", g_count);
  }

  // --- transfer progress ---
  if (g_receiving) {
    float f = (g_curSize > 0) ? (float)g_curRecv / g_curSize : 0;
    drawBar(6, 72, 308, 14, f, C_GREEN);
    canvas.setTextSize(1);
    canvas.setTextColor(C_WHITE, C_BLACK);
    canvas.setCursor(6, 92);
    if (g_curSize > 0) canvas.printf("%.0f / %.0f KB", g_curRecv / 1024.0, g_curSize / 1024.0);
    else               canvas.printf("%lu B (size?)", (unsigned long)g_curRecv);
    if (g_bps > 100) {
      canvas.setTextColor(C_AMBER, C_BLACK);
      canvas.setCursor(150, 92);
      canvas.printf("%.0f KB/s", g_bps / 1024.0);
    }
    // rtx = re-requested blocks (should stay 0)
    // buf = data still queued in RAM waiting for the card (the ring at work)
    canvas.setTextColor(g_ymRetries ? C_RED : C_GRAY, C_BLACK);
    canvas.setCursor(232, 92);
    canvas.printf("rtx %lu", (unsigned long)g_ymRetries);
    canvas.setTextColor(C_GRAY, C_BLACK);
    canvas.setCursor(232, 104);
    canvas.printf("buf %uK", (unsigned)(storageBacklog() / 1024));
  } else {
    canvas.setTextSize(1);
    canvas.setTextColor(C_WHITE, C_BLACK);
    canvas.setCursor(6, 92);
    canvas.printf("Saved: %d", g_count);
  }

  // --- card fullness (size 2 label) ---
  canvas.setTextSize(2);
  canvas.setTextColor(C_WHITE, C_BLACK);
  canvas.setCursor(6, 112);
  if (g_total > 0) {
    int pct = (int)((uint64_t)100 * g_used / g_total);
    canvas.printf("Card %d%%", pct);
    canvas.setTextSize(1);
    canvas.setTextColor(C_WHITE, C_BLACK);
    canvas.setCursor(150, 119);
    canvas.printf("%.1f / %.1f GB", g_used / 1e9, g_total / 1e9);
    uint16_t col = (pct >= 90) ? C_RED : (pct >= 75 ? C_AMBER : C_GREEN);
    drawBar(6, 136, 308, 16, (float)((double)g_used / g_total), col);
  } else {
    canvas.printf("Card --");
  }

  // footer
  canvas.setTextSize(1);
  canvas.setTextColor(C_GRAY, C_BLACK);
  canvas.setCursor(6, 160); canvas.print("YMODEM in. Pull card to delete.");

  canvas.pushSprite(0, 0);
  setLed();
}

// ============================ YMODEM callbacks ============================

static bool cbBegin(const char *filename, uint32_t filesize) {
  snprintf(g_curName, sizeof(g_curName), "%s", filename);
  sanitize(g_curName);
  g_curSize = filesize; g_curRecv = 0; g_receiving = true; g_state = "RECEIVING";
  g_startMs = millis(); g_rateMs = g_startMs; g_rateBytes = 0; g_bps = 0;

  if (g_total && filesize && (uint64_t)filesize > (g_total - g_used)) {
    g_state = "DISK FULL"; g_receiving = false; g_stateSince = millis(); return false;
  }
  if (!storageOpenTemp()) { g_state = "ERR: open"; g_receiving = false; g_stateSince = millis(); return false; }
  return true;
}

// The ONLY thing the receive path does with data: copy it into the PSRAM ring.
// No card I/O, no screen, no filesystem — those live on the other core now, so
// nothing here can ever stall the USB read and drop an incoming block.
static bool cbData(const uint8_t *data, size_t len) {
  if (!storageWrite(data, len)) return false;
  g_curRecv += len;
  g_used    += len;              // live fullness estimate (reconciled on cbEnd)
  return true;
}

// Called by the receiver only when the line is idle (ACK already sent, next
// block not yet arriving) — the one safe moment to touch the SPI bus.
// Called after each ACK. Now that BOTH the card write and the screen live on
// the other core, there is nothing slow left to do here — we only sample the
// throughput (pure arithmetic, microseconds).
static void cbIdle(void) {
  uint32_t now = millis();
  uint32_t dt  = now - g_rateMs;
  if (dt >= 500) {
    float inst = (float)(g_curRecv - g_rateBytes) * 1000.0f / (float)dt;
    g_bps = (g_bps <= 0) ? inst : (g_bps * 0.7f + inst * 0.3f);   // EMA
    g_rateMs = now; g_rateBytes = g_curRecv;
  }
}

static bool cbEnd(void) {
  char finalName[160];
  bool ok = storageFinalize(g_curName, finalName, sizeof(finalName));
  g_used = storageUsedBytes();   // reconcile with real FAT usage
  g_receiving = false;
  if (ok) {
    snprintf(g_curName, sizeof(g_curName), "%s", finalName);
    g_count++; g_state = "SAVED"; g_stateSince = millis();
    return true;
  }
  g_state = "ERR: save"; g_stateSince = millis();
  return false;
}

static void cbAbort(void) {
  g_receiving = false;
  storageAbort();
  g_used = storageUsedBytes();
  g_state = "ABORTED";
  g_stateSince = millis();
}

// ============================ setup / loop ============================

// The display task. Owns the LCD entirely and runs on core 0, so the ~22 ms
// SPI sprite push can never sit in the middle of a USB read on core 1.
static void displayTask(void *) {
  for (;;) {
    drawUI(true);
    vTaskDelay(pdMS_TO_TICKS(250));    // 4 fps is plenty for a status screen
  }
}

void setup() {
  lcd.init();
  lcd.setRotation(1);          // landscape 320x172
  lcd.setScrollRect(0, 0, 320, 172);   // reset any stale scroll region
  lcd.fillScreen(C_BLACK);
  lcd.setBrightness(140);
  canvas.setColorDepth(16);
  canvas.createSprite(320, 172);

  // Give the CDC driver a much bigger RX ring BEFORE begin() (it's ignored
  // afterwards). The default (~256 B) overflows the moment anything blocks for
  // a fraction of a millisecond, which is what corrupted incoming 1 KB blocks.
  Serial.setRxBufferSize(32768);
  Serial.begin(115200);        // native USB CDC (baud ignored on real USB)

  if (storageBegin()) {
    g_total = storageTotalBytes();
    g_used  = storageUsedBytes();
    g_state = "READY";
  } else {
    g_state = "NO SD CARD";
  }

  // From here on the display belongs to displayTask ALONE. Nothing else may
  // touch lcd/canvas: two tasks writing the SPI bus at once corrupts the
  // ST7789's address/scroll registers, which is what tore the screen in half.
  xTaskCreatePinnedToCore(displayTask, "display", 4096, nullptr, 1, nullptr, 0);
}

void loop() {
  // Core 1: nothing but YMODEM. No SD, no LCD, no blocking calls — so an
  // incoming block can never be missed because we were busy elsewhere.
  static const YmodemCallbacks cb = { cbBegin, cbData, cbEnd, cbAbort, cbIdle };
  ymodemReceive(Serial, cb);

  if (!g_receiving && g_stateSince && millis() - g_stateSince > 2500) {
    if (g_total > 0) g_state = "READY";
    g_stateSince = 0;
  }
  delay(2);
}
