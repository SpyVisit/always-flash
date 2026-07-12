// ymodem.h — minimal YMODEM batch receiver for ALWAYS FLASH
//
// The receiver drives the transfer: it advertises readiness by emitting 'C'
// (CRC mode) and only advances to the next block after ACKing the current one.
// That ACK-per-block handshake is also our flow control — the sender physically
// cannot outrun how fast we commit data to the SD card.
#pragma once
#include <Arduino.h>

// Callbacks wire the protocol to whatever storage backend you use.
// Return false from any of them to abort the running file (we cancel the
// transfer and discard the partial).
typedef struct {
  bool (*onFileBegin)(const char *filename, uint32_t filesize); // open a sink
  bool (*onData)(const uint8_t *data, size_t len);              // append bytes
  bool (*onFileEnd)(void);                                      // finalize/commit
  void (*onAbort)(void);                                        // discard partial
  // Called only when the line is momentarily idle (right after an ACK went out).
  // This is the ONLY safe place to do slow work like repainting a display —
  // doing it inside onData() blocks the USB RX path and corrupts blocks.
  // May be NULL.
  void (*onIdle)(void);
} YmodemCallbacks;

// Probe for / receive one YMODEM batch on `io`.
// Returns: number of files received (>=0), or -1 on a hard error/cancel.
// Called repeatedly from loop(); when no sender is present it returns 0 quickly
// after a few 'C' probes so the main loop can re-probe.
int ymodemReceive(Stream &io, const YmodemCallbacks &cb);

// Diagnostics: how many blocks had to be re-requested (NAK'd) in this session.
// A healthy transfer keeps this at or near zero; a rising count means blocks are
// arriving corrupted (usually: something is blocking the USB read path).
extern volatile uint32_t g_ymRetries;
