// AF_Diag.ino — ALWAYS FLASH diagnostic build
//
// Receives exactly like the real firmware, but instruments every anomaly.
// When the transfer dies, it STOPS the YMODEM receiver (no more 'C') and
// repeatedly prints a crash trace to the USB serial port every 3 seconds.
//
// HOW TO USE:
//   1. Flash this. Board: ESP32S3 Dev Module, USB CDC On Boot: Enabled,
//      PSRAM: OPI PSRAM, Flash 16MB.
//   2. Open Tera Term on the COM port. Send the big file via YMODEM.
//   3. When it stalls, click Cancel to close the transfer dialog.
//   4. The black Tera Term window will now show the trace, repeating.
//      Select one full report and paste it back.
#include <Arduino.h>
#include "LGFX_Config.h"
#include "ymodem_diag.h"
#include "storage.h"

#define RGB_PIN 38

static LGFX        lcd;
static LGFX_Sprite canvas(&lcd);

#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_GREEN 0x07E0
#define C_RED   0xF800
#define C_GRAY  0x7BEF
#define C_AMBER 0xFD20

static char        g_curName[128] = "";
static uint32_t    g_curSize = 0, g_curRecv = 0;
static bool        g_receiving = false;
static const char *g_state = "INIT";
static uint32_t    g_peakBacklog = 0;

static void sanitize(char *s) {
  char *p = s, *b = s;
  for (; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
  if (b != s) memmove(s, b, strlen(b) + 1);
  for (p = s; *p; p++)
    if (strchr("\\/:*?\"<>|", *p) || (unsigned char)*p < 0x20) *p = '_';
  if (!s[0]) strcpy(s, "noname");
}

static void drawUI() {
  canvas.fillScreen(C_BLACK);
  canvas.setTextColor(C_GREEN, C_BLACK);
  canvas.setTextSize(2); canvas.setCursor(6, 4); canvas.print("AF DIAGNOSTIC");
  canvas.drawFastHLine(0, 26, 320, C_GRAY);

  canvas.setTextSize(2);
  canvas.setTextColor(g_crashed ? C_RED : C_WHITE, C_BLACK);
  canvas.setCursor(6, 34);
  canvas.printf("%s", g_crashed ? "CRASHED" : g_state);

  canvas.setTextSize(1);
  canvas.setTextColor(C_WHITE, C_BLACK);
  canvas.setCursor(6, 58);
  canvas.printf("%.40s", g_curName);

  if (g_curSize) {
    float f = (float)g_curRecv / g_curSize;
    canvas.drawRect(6, 70, 308, 12, C_WHITE);
    canvas.fillRect(7, 71, (int)(306 * (f > 1 ? 1 : f)), 10, C_GREEN);
  }
  canvas.setCursor(6, 88);
  canvas.printf("%.0f / %.0f KB", g_curRecv / 1024.0, g_curSize / 1024.0);

  canvas.setTextColor(g_ymRetries ? C_RED : C_GRAY, C_BLACK);
  canvas.setCursor(6, 102);
  canvas.printf("rtx %lu   blocks %lu", (unsigned long)g_ymRetries,
                (unsigned long)g_totalBlocks);

  canvas.setTextColor(C_AMBER, C_BLACK);
  canvas.setCursor(6, 114);
  canvas.printf("ring now %uK  peak %uK",
                (unsigned)(storageBacklog() / 1024), (unsigned)(g_peakBacklog / 1024));

  canvas.setTextColor(C_GRAY, C_BLACK);
  canvas.setCursor(6, 130);
  canvas.printf("trace entries: %d", g_traceCount);

  if (g_crashed) {
    canvas.setTextColor(C_RED, C_BLACK);
    canvas.setCursor(6, 148);
    canvas.print("Cancel the transfer -> read the");
    canvas.setCursor(6, 158);
    canvas.print("report in the terminal window.");
  }
  canvas.pushSprite(0, 0);
}

static void displayTask(void *) {
  for (;;) {
    size_t b = storageBacklog();
    if (b > g_peakBacklog) g_peakBacklog = b;
    drawUI();
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

static bool cbBegin(const char *fn, uint32_t sz) {
  snprintf(g_curName, sizeof g_curName, "%s", fn);
  sanitize(g_curName);
  g_curSize = sz; g_curRecv = 0; g_receiving = true; g_state = "RECEIVING";
  g_peakBacklog = 0;
  return storageOpenTemp();
}
static bool cbData(const uint8_t *d, size_t n) {
  if (!storageWrite(d, n)) return false;
  g_curRecv += n;
  return true;
}
static bool cbEnd(void) {
  char fin[160];
  bool ok = storageFinalize(g_curName, fin, sizeof fin);
  g_receiving = false;
  g_state = ok ? "SAVED" : "ERR SAVE";
  return ok;
}
static void cbAbort(void) {
  g_receiving = false;
  storageAbort();
  if (!g_crashed) g_state = "ABORTED";
}
static void cbIdle(void) {}

void setup() {
  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(140);
  canvas.setColorDepth(16);
  canvas.createSprite(320, 172);

  Serial.setRxBufferSize(32768);
  Serial.begin(115200);

  g_state = storageBegin() ? "READY" : "NO SD";
  drawUI();
  xTaskCreatePinnedToCore(displayTask, "disp", 4096, nullptr, 1, nullptr, 0);
}

void loop() {
  if (g_crashed) {
    // Transfer is dead. Stop talking YMODEM entirely (no more 'C') and just
    // print the report over and over so it can be captured in the terminal.
    rgbLedWrite(RGB_PIN, 60, 0, 0);
    ymodemDumpTrace(Serial);
    delay(3000);
    return;
  }

  rgbLedWrite(RGB_PIN, g_receiving ? 30 : 0, g_receiving ? 18 : 0, g_receiving ? 0 : 25);
  static const YmodemCallbacks cb = { cbBegin, cbData, cbEnd, cbAbort, cbIdle };
  ymodemReceive(Serial, cb);
  delay(2);
}
