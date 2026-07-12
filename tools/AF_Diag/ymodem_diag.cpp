// ymodem_diag.cpp — instrumented YMODEM receiver
#include "ymodem_diag.h"

#define SOH   0x01
#define STX   0x02
#define EOT   0x04
#define ACK   0x06
#define NAK   0x15
#define CAN   0x18
#define YM_C  0x43

volatile uint32_t g_ymRetries  = 0;
TraceEntry        g_trace[TRACE_MAX];
volatile int      g_traceCount = 0;
volatile bool     g_crashed    = false;
volatile uint64_t g_totalBytes = 0;
volatile uint32_t g_totalBlocks= 0;

static uint32_t g_expectBlock = 0;
static volatile bool g_inTransfer = false;   // true only while receiving a file

// ---- ACK/TX instrumentation ----
volatile uint32_t g_ackSent   = 0;   // ACKs we wrote
volatile uint32_t g_ackSlow   = 0;   // ACKs where the TX buffer was not empty
volatile uint32_t g_txWorstMs = 0;   // worst time an ACK took to flush

// Ring buffer: keeps the LAST TRACE_MAX events. (The first version kept the
// FIRST ones and filled up with idle 'C' probes before the transfer even began.)
static int g_traceHead = 0;
volatile uint32_t g_traceTotal = 0;

static void trace(uint8_t ev, uint8_t lead, uint16_t avail, uint16_t got,
                  uint8_t seq, uint8_t nseq, uint16_t crcRx, uint16_t crcCalc) {
  // Don't record idle probing (no transfer running yet) - it's just noise.
  if (!g_inTransfer) return;
  g_traceTotal++;
  TraceEntry &e = g_trace[g_traceHead];
  g_traceHead = (g_traceHead + 1) % TRACE_MAX;
  if (g_traceCount < TRACE_MAX) g_traceCount++;
  e.ms      = millis();
  e.blockNo = g_expectBlock;
  e.bytesIn = g_totalBytes;
  e.event   = ev;
  e.lead    = lead;
  e.avail   = avail;
  e.got     = got;
  e.seq     = seq;
  e.nseq    = nseq;
  e.crcRx   = crcRx;
  e.crcCalc = crcCalc;
}

static uint16_t crc16_ccitt(const uint8_t *d, size_t n) {
  uint16_t crc = 0;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static int readByte(Stream &io, uint32_t tmo) {
  uint32_t start = millis(), quiet = start;
  for (;;) {
    int c = io.read();
    if (c >= 0) return c;
    uint32_t now = millis();
    if (now - start >= tmo) return -1;
    if (now - quiet < 2) taskYIELD(); else vTaskDelay(1);
  }
}

static void purgeRx(Stream &io) {
  uint32_t quiet = millis();
  while (millis() - quiet < 60) {
    if (io.read() >= 0) quiet = millis();
    else                taskYIELD();
  }
}

enum BlockResult { BLK_OK, BLK_EOT, BLK_CAN, BLK_TIMEOUT, BLK_BADCRC };

static BlockResult readBlock(Stream &io, uint8_t *seqOut, uint8_t *payload,
                             size_t *plen, uint32_t firstTmo) {
  uint16_t availAtStart = (uint16_t)io.available();

  int c = readByte(io, firstTmo);
  if (c < 0) { trace(EV_LEAD_TIMEOUT, 0, availAtStart, 0, 0, 0, 0, 0); return BLK_TIMEOUT; }

  size_t dataLen;
  if      (c == SOH) dataLen = 128;
  else if (c == STX) dataLen = 1024;
  else if (c == EOT) return BLK_EOT;
  else if (c == CAN) return BLK_CAN;
  else { trace(EV_LEAD_GARBAGE, (uint8_t)c, availAtStart, 0, 0, 0, 0, 0); return BLK_BADCRC; }

  int seq  = readByte(io, 1000);
  int nseq = readByte(io, 1000);
  if (seq < 0 || nseq < 0) { trace(EV_PAYLOAD_SHORT, (uint8_t)c, availAtStart, 0, 0, 0, 0, 0); return BLK_TIMEOUT; }
  if (((seq ^ nseq) & 0xFF) != 0xFF) {
    trace(EV_SEQ_BAD, (uint8_t)c, availAtStart, 0, (uint8_t)seq, (uint8_t)nseq, 0, 0);
    return BLK_BADCRC;
  }

  size_t got = 0;
  uint32_t t0 = millis(), quiet = 0;
  while (got < dataLen) {
    size_t a = io.available();
    if (a) {
      size_t want = dataLen - got;
      if (a > want) a = want;
      got += io.readBytes(payload + got, a);
      t0 = millis(); quiet = 0;
    } else {
      uint32_t now = millis();
      if (!quiet) quiet = now;
      if (now - t0 > 2000) {
        trace(EV_PAYLOAD_SHORT, (uint8_t)c, availAtStart, (uint16_t)got,
              (uint8_t)seq, (uint8_t)nseq, 0, 0);
        return BLK_TIMEOUT;
      }
      if (now - quiet < 2) taskYIELD(); else vTaskDelay(1);
    }
  }

  int ch = readByte(io, 1000), cl = readByte(io, 1000);
  if (ch < 0 || cl < 0) {
    trace(EV_PAYLOAD_SHORT, (uint8_t)c, availAtStart, (uint16_t)got, (uint8_t)seq, (uint8_t)nseq, 0, 0);
    return BLK_TIMEOUT;
  }

  uint16_t rx = (uint16_t)((ch << 8) | cl);
  uint16_t cc = crc16_ccitt(payload, dataLen);
  if (rx != cc) {
    trace(EV_CRC_BAD, (uint8_t)c, availAtStart, (uint16_t)got, (uint8_t)seq, (uint8_t)nseq, rx, cc);
    return BLK_BADCRC;
  }

  *seqOut = (uint8_t)seq;
  *plen   = dataLen;
  return BLK_OK;
}

static void parseHeader(const uint8_t *p, char *fn, size_t fnsz, uint32_t *fsz) {
  size_t i = 0;
  while (p[i] && i < fnsz - 1 && i < 1024) { fn[i] = (char)p[i]; i++; }
  fn[i] = 0;
  size_t j = i + 1;
  uint32_t sz = 0; bool any = false;
  while (j < 1024 && p[j] == ' ') j++;
  while (j < 1024 && p[j] >= '0' && p[j] <= '9') { sz = sz*10 + (p[j]-'0'); j++; any = true; }
  *fsz = any ? sz : 0;
}

int ymodemReceive(Stream &io, const YmodemCallbacks &cb) {
  static uint8_t payload[1024];
  int files = 0;

  while (true) {
    uint8_t seq; size_t plen;
    bool haveHeader = false;
    const bool mid = (files > 0);
    const int  attempts = mid ? 6 : 1;
    const uint32_t tmo  = mid ? 3000 : 800;

    for (int a = 0; a < attempts && !haveHeader; a++) {
      io.write((uint8_t)YM_C);
      BlockResult br = readBlock(io, &seq, payload, &plen, tmo);
      if      (br == BLK_OK)  haveHeader = true;
      else if (br == BLK_EOT) io.write((uint8_t)ACK);
      else if (br == BLK_CAN) return files;
      else if (br == BLK_BADCRC) purgeRx(io);
    }
    if (!haveHeader) return files;
    if (payload[0] == 0) { io.write((uint8_t)ACK); return files; }

    char fname[128]; uint32_t fsize;
    parseHeader(payload, fname, sizeof fname, &fsize);
    io.write((uint8_t)ACK);
    if (!cb.onFileBegin(fname, fsize)) { for (int i=0;i<8;i++) io.write((uint8_t)CAN); return -1; }

    io.write((uint8_t)YM_C);
    uint8_t expect = 1;
    uint32_t received = 0;
    bool fileOk = true, done = false;
    uint32_t lastProgress = millis();
    g_expectBlock = 1;
    g_totalBytes = 0;
    g_totalBlocks = 0;
    g_inTransfer = true;

    while (!done) {
      BlockResult r = readBlock(io, &seq, payload, &plen, 5000);
      switch (r) {
        case BLK_OK:
          lastProgress = millis();
          if (seq == expect) {
            size_t use = plen;
            if (fsize > 0 && received + use > fsize) use = fsize - received;
            if (!cb.onData(payload, use)) { fileOk = false; done = true; break; }
            received += use;
            g_totalBytes = received;
            g_totalBlocks++;

            // === THE SUSPECT ===
            // A single-byte ACK can sit in the USB-CDC TX buffer instead of
            // going out on the wire. The sender then times out and resends the
            // block -> we see a duplicate -> retries pile up. Measure it, and
            // force it out with flush().
            {
              uint32_t ta = millis();
              io.write((uint8_t)ACK);
              io.flush();                     // <-- push the ACK to the host NOW
              uint32_t dt = millis() - ta;
              g_ackSent++;
              if (dt > 0) g_ackSlow++;
              if (dt > g_txWorstMs) g_txWorstMs = dt;
            }
            expect++;
            g_expectBlock = expect;
            if (cb.onIdle) cb.onIdle();
          } else if (seq == (uint8_t)(expect - 1)) {
            io.write((uint8_t)ACK);
          } else {
            trace(EV_SEQ_UNEXPECTED, 0, (uint16_t)io.available(), 0, seq, 0, 0, expect);
            g_ymRetries++;
            purgeRx(io);
            io.write((uint8_t)NAK);
          }
          break;

        case BLK_EOT:
          io.write((uint8_t)NAK);
          readBlock(io, &seq, payload, &plen, 3000);
          io.write((uint8_t)ACK);
          done = true;
          break;

        case BLK_CAN:
          fileOk = false; done = true;
          break;

        default:
          if (millis() - lastProgress > 12000) {
            trace(EV_GAVE_UP, 0, (uint16_t)io.available(), 0, 0, 0, 0, 0);
            g_crashed = true;              // <-- freeze: main loop will dump
            fileOk = false; done = true;
            break;
          }
          g_ymRetries++;
          trace(EV_NAK_SENT, 0, (uint16_t)io.available(), 0, 0, 0, 0, 0);
          purgeRx(io);
          io.write((uint8_t)NAK);
          break;
      }
    }

    g_inTransfer = false;
    if (fileOk && cb.onFileEnd()) files++;
    else { cb.onAbort(); return files; }
  }
}

static const char *evName(uint8_t e) {
  switch (e) {
    case EV_LEAD_TIMEOUT:   return "LEAD_TIMEOUT  (line went SILENT)";
    case EV_LEAD_GARBAGE:   return "LEAD_GARBAGE  (unexpected start byte)";
    case EV_SEQ_BAD:        return "SEQ_BAD       (seq/~seq mismatch)";
    case EV_PAYLOAD_SHORT:  return "PAYLOAD_SHORT (block did not arrive fully)";
    case EV_CRC_BAD:        return "CRC_BAD       (data corrupted)";
    case EV_SEQ_UNEXPECTED: return "SEQ_UNEXPECT  (wrong block number)";
    case EV_NAK_SENT:       return "NAK_SENT";
    case EV_GAVE_UP:        return "GAVE_UP";
    default:                return "?";
  }
}

void ymodemDumpTrace(Stream &out) {
  out.println();
  out.println("=========== ALWAYS FLASH - CRASH TRACE ===========");
  out.printf ("Blocks OK     : %lu\n", (unsigned long)g_totalBlocks);
  out.printf ("Bytes received: %llu (%.2f MB)\n",
              (unsigned long long)g_totalBytes, g_totalBytes / 1048576.0);
  out.printf ("Retries (NAK) : %lu\n", (unsigned long)g_ymRetries);
  out.println();
  out.println("--- ACK / TX health (the prime suspect) ---");
  out.printf ("ACKs sent     : %lu\n", (unsigned long)g_ackSent);
  out.printf ("ACKs delayed  : %lu  (flush took >0 ms)\n", (unsigned long)g_ackSlow);
  out.printf ("Worst ACK time: %lu ms\n", (unsigned long)g_txWorstMs);
  if (g_totalBlocks && g_ymRetries > g_totalBlocks / 4)
    out.println("  !! retries are a large fraction of blocks - the link is unhealthy");
  out.println();
  out.printf ("Events recorded: %lu total, showing last %d\n",
              (unsigned long)g_traceTotal, g_traceCount);
  out.println("-------------------------------------------------------------");
  out.println("   ms     blk   bytesIn  event            lead avail  got  crcRx/calc");

  // walk the ring oldest -> newest
  int cnt   = g_traceCount;
  int start = (cnt < TRACE_MAX) ? 0 : g_traceHead;
  for (int i = 0; i < cnt; i++) {
    TraceEntry &e = g_trace[(start + i) % TRACE_MAX];
    out.printf("%7lu %7lu %9llu  %-16s 0x%02X %5u %4u  %04X/%04X\n",
               (unsigned long)e.ms, (unsigned long)e.blockNo,
               (unsigned long long)e.bytesIn, evName(e.event),
               e.lead, e.avail, e.got, e.crcRx, e.crcCalc);
  }
  out.println("=============================================================");
  out.println("READ IT LIKE THIS:");
  out.println(" LEAD_TIMEOUT / got=0  -> sender went SILENT (our ACK never arrived)");
  out.println(" CRC_BAD / LEAD_GARBAGE-> bytes are being MANGLED on the wire");
  out.println(" many 'ACKs delayed'   -> the ACK is stuck in the USB TX buffer");
  out.println();
}
