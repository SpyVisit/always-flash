// ymodem_diag.h — instrumented YMODEM receiver.
//
// Identical protocol logic to the production one, but it records a detailed
// trace of every anomaly and, on failure, freezes so the trace can be dumped.
#pragma once
#include <Arduino.h>

typedef struct {
  bool (*onFileBegin)(const char *filename, uint32_t filesize);
  bool (*onData)(const uint8_t *data, size_t len);
  bool (*onFileEnd)(void);
  void (*onAbort)(void);
  void (*onIdle)(void);
} YmodemCallbacks;

// ---- diagnostic trace ----
#define TRACE_MAX 64

typedef struct {
  uint32_t ms;          // when
  uint32_t blockNo;     // which block we were expecting
  uint64_t bytesIn;     // bytes received so far
  uint8_t  event;       // see EV_* below
  uint8_t  lead;        // the byte that started the "block" (or 0)
  uint16_t avail;       // bytes sitting in the RX buffer at that moment
  uint16_t got;         // how many payload bytes we actually managed to read
  uint8_t  seq, nseq;   // sequence bytes as received
  uint16_t crcRx, crcCalc;
} TraceEntry;

enum {
  EV_LEAD_TIMEOUT = 1,  // no lead byte at all — the line went silent
  EV_LEAD_GARBAGE,      // lead byte was not SOH/STX/EOT/CAN
  EV_SEQ_BAD,           // seq / ~seq mismatch
  EV_PAYLOAD_SHORT,     // payload didn't arrive in full (timeout mid-block)
  EV_CRC_BAD,           // payload arrived but CRC failed
  EV_SEQ_UNEXPECTED,    // valid block, wrong number
  EV_NAK_SENT,          // we asked for a retransmit
  EV_GAVE_UP            // we stopped trying
};

extern volatile uint32_t g_ymRetries;
extern volatile uint32_t g_ackSent;
extern volatile uint32_t g_ackSlow;
extern volatile uint32_t g_txWorstMs;
extern volatile uint32_t g_traceTotal;
extern TraceEntry        g_trace[TRACE_MAX];
extern volatile int      g_traceCount;
extern volatile bool     g_crashed;      // receiver gave up -> dump the trace
extern volatile uint64_t g_totalBytes;
extern volatile uint32_t g_totalBlocks;

int  ymodemReceive(Stream &io, const YmodemCallbacks &cb);
void ymodemDumpTrace(Stream &out);
