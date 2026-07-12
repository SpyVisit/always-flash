// ymodem.cpp — YMODEM batch receiver
#include "ymodem.h"

// --- control characters ---
#define SOH   0x01  // start of 128-byte block
#define STX   0x02  // start of 1024-byte block
#define EOT   0x04  // end of transmission (one file)
#define ACK   0x06
#define NAK   0x15
#define CAN   0x18  // cancel
#define YM_C  0x43  // 'C' — request CRC mode / advertise readiness

volatile uint32_t g_ymRetries = 0;   // NAKs we had to send (see ymodem.h)

// Idle probing cadence (Phase A, when no transfer is running yet).
#ifndef YM_PROBE_ATTEMPTS
#define YM_PROBE_ATTEMPTS 1
#endif
#ifndef YM_PROBE_TIMEOUT_MS
#define YM_PROBE_TIMEOUT_MS 800
#endif

// Cadence while a batch is ALREADY running (waiting for the next file's header,
// or the empty end-of-batch header). Must be patient: after we ACK the final
// EOT the sender needs time to prepare that last block, and if we bail out here
// the transfer hangs with the sender waiting for an ACK that never comes.
#define YM_BATCH_ATTEMPTS   6
#define YM_BATCH_TIMEOUT_MS 3000

// --- block read result ---
enum BlockResult { BLK_OK, BLK_EOT, BLK_CAN, BLK_TIMEOUT, BLK_BADCRC };

// CRC-16/XMODEM (poly 0x1021, init 0x0000). Verified against vector 0x31C3.
static uint16_t crc16_ccitt(const uint8_t *data, size_t len) {
  uint16_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// Wait for one byte. Must yield — the USB-CDC bytes are delivered by a separate
// FreeRTOS task, and a tight busy-wait starves the very task we depend on.
// But we must not *sleep* on every micro-gap either (see readBlock): spin
// cheaply first, and only sleep once the line has been quiet for a couple of ms.
static int readByte(Stream &io, uint32_t timeout_ms) {
  uint32_t start = millis();
  uint32_t quietStart = start;
  for (;;) {
    int c = io.read();
    if (c >= 0) return c;
    uint32_t now = millis();
    if ((now - start) >= timeout_ms) return -1;
    if (now - quietStart < 2) taskYIELD();  // data may be one packet away
    else                      vTaskDelay(1);
  }
}

static void sendCancel(Stream &io) {
  for (int i = 0; i < 8; i++) io.write((uint8_t)CAN);
}

// NOTE: an earlier version had a purgeRx() here that drained the RX buffer for
// 60 ms before every NAK, meaning to "resynchronize". It did the opposite: the
// sender is already transmitting the next/retransmitted block during that
// window, so the purge ATE it. We then fell permanently behind, the sender ran
// ahead, and every subsequent block was rejected as out-of-sequence -> deadlock.
// Resynchronization is done properly in readBlock() instead, by hunting for a
// valid lead byte rather than throwing data away.

// Read one full block. Validates the seq/~seq complement and the CRC, but does
// NOT compare seq against what we expect — the caller does that so it can detect
// duplicate (already-ACKed) retransmits.
static BlockResult readBlock(Stream &io, uint8_t *seqOut,
                             uint8_t *payload, size_t *plen,
                             uint32_t firstByteTimeout) {
  // HUNT for a valid lead byte. Anything else on the line is leftover junk from
  // a failed block; silently skip it rather than declaring an error. This is how
  // we resynchronize — by finding the next real block boundary, never by
  // discarding data blindly.
  int c;
  uint32_t huntStart = millis();
  for (;;) {
    c = readByte(io, firstByteTimeout);
    if (c < 0) return BLK_TIMEOUT;
    if (c == SOH || c == STX || c == EOT || c == CAN) break;
    if (millis() - huntStart > firstByteTimeout) return BLK_TIMEOUT;
    // else: junk byte, keep hunting
  }

  size_t dataLen;
  if      (c == SOH) dataLen = 128;
  else if (c == STX) dataLen = 1024;
  else if (c == EOT) return BLK_EOT;
  else               return BLK_CAN;

  int seq  = readByte(io, 1000);
  int nseq = readByte(io, 1000);
  if (seq < 0 || nseq < 0) return BLK_TIMEOUT;
  if (((seq ^ nseq) & 0xFF) != 0xFF) return BLK_BADCRC;

  // Read the payload in BULK, with an ADAPTIVE wait.
  //
  // USB delivers a 1 KB block as ~16 packets of 64 bytes, so available() dips to
  // zero *between packets*. Sleeping a full RTOS tick on every such dip cost us
  // up to 16 sleeps per block (~90 ms) and throttled throughput to ~11 KB/s.
  // Instead: spin with a cheap taskYIELD() while the data is still streaming,
  // and only fall back to a real sleep once the line has been quiet for a while.
  size_t got = 0;
  uint32_t t0 = millis();
  uint32_t quietStart = 0;
  while (got < dataLen) {
    size_t avail = io.available();
    if (avail) {
      size_t want = dataLen - got;
      if (avail > want) avail = want;
      got += io.readBytes(payload + got, avail);
      t0 = millis();
      quietStart = 0;                       // data is flowing again
    } else {
      uint32_t now = millis();
      if (!quietStart) quietStart = now;
      if (now - t0 > 2000) return BLK_TIMEOUT;
      if (now - quietStart < 2) taskYIELD(); // brief gap: just let others run
      else                      vTaskDelay(1); // real lull: sleep properly
    }
  }

  int ch = readByte(io, 1000);
  int cl = readByte(io, 1000);
  if (ch < 0 || cl < 0) return BLK_TIMEOUT;

  uint16_t rxcrc = (uint16_t)((ch << 8) | cl);
  if (crc16_ccitt(payload, dataLen) != rxcrc) return BLK_BADCRC;

  *seqOut = (uint8_t)seq;
  *plen   = dataLen;
  return BLK_OK;
}

// Parse block-0 payload: "filename\0SIZE ...". Empty filename = end of batch.
static void parseHeader(const uint8_t *payload, char *fname, size_t fnsz, uint32_t *fsize) {
  size_t i = 0;
  while (payload[i] && i < fnsz - 1 && i < 1024) { fname[i] = (char)payload[i]; i++; }
  fname[i] = 0;
  size_t j = i + 1;
  uint32_t sz = 0; bool any = false;
  while (j < 1024 && payload[j] == ' ') j++;
  while (j < 1024 && payload[j] >= '0' && payload[j] <= '9') {
    sz = sz * 10 + (payload[j] - '0'); j++; any = true;
  }
  *fsize = any ? sz : 0;
}

int ymodemReceive(Stream &io, const YmodemCallbacks &cb) {
  static uint8_t payload[1024];
  int filesReceived = 0;

  // One iteration of this loop == one file (or the end-of-batch marker).
  while (true) {
    // ---- Phase A: wait for the header block (seq 0), advertising with 'C' ----
    uint8_t seq; size_t plen;
    bool haveHeader = false;

    // Idle (no file received yet): probe briefly, then hand control back to
    // loop(). Mid-batch (we already took a file): be patient — the sender still
    // owes us either the next header or the empty end-of-batch block, and
    // giving up here would leave it waiting for an ACK forever.
    const bool  midBatch = (filesReceived > 0);
    const int   attempts = midBatch ? YM_BATCH_ATTEMPTS   : YM_PROBE_ATTEMPTS;
    const uint32_t tmo   = midBatch ? YM_BATCH_TIMEOUT_MS : YM_PROBE_TIMEOUT_MS;

    for (int attempt = 0; attempt < attempts && !haveHeader; attempt++) {
      io.write((uint8_t)YM_C);
      BlockResult br = readBlock(io, &seq, payload, &plen, tmo);
      if (br == BLK_OK)       haveHeader = true;
      else if (br == BLK_EOT) io.write((uint8_t)ACK);  // stray EOT, ack & continue
      else if (br == BLK_CAN) return filesReceived;
      // timeout / badcrc -> loop and send 'C' again
    }
    if (!haveHeader) return filesReceived;

    // Empty filename in block 0 => end of the whole batch.
    if (payload[0] == 0x00) { io.write((uint8_t)ACK); return filesReceived; }

    char fname[128]; uint32_t fsize;
    parseHeader(payload, fname, sizeof(fname), &fsize);
    io.write((uint8_t)ACK);                  // ack the header block

    if (!cb.onFileBegin(fname, fsize)) { sendCancel(io); return -1; }

    // ---- Phase B: data blocks ----
    io.write((uint8_t)YM_C);                 // ask sender to start data (CRC mode)
    uint8_t expect = 1;
    uint32_t received = 0;
    bool fileOk = true, done = false;
    uint32_t lastProgress = millis();        // last time a block actually landed

    while (!done) {
      BlockResult r = readBlock(io, &seq, payload, &plen, 5000);
      switch (r) {
        case BLK_OK:
          lastProgress = millis();
          if (seq == expect) {
            // Trim the final block's padding using the declared size.
            size_t useLen = plen;
            if (fsize > 0 && received + useLen > fsize) useLen = fsize - received;
            if (!cb.onData(payload, useLen)) { fileOk = false; sendCancel(io); done = true; break; }
            received += useLen;
            io.write((uint8_t)ACK);
            io.flush();                      // make sure the ACK actually leaves
            expect++;
            // The ACK is out and the sender is now busy building the next block,
            // so the line is quiet. Hand control to the app for its slow work
            // (SD flush, repaint). Called after EVERY block — the storage layer
            // needs this window to drain its buffer; the app itself decides how
            // often to actually repaint.
            if (cb.onIdle) cb.onIdle();
          } else if ((uint8_t)(expect - seq) <= 8 && seq != expect) {
            // The sender is re-sending a block we already stored (our ACK for it
            // was lost). Just ACK it again — do NOT write it twice. A window of
            // 8 means several lost ACKs in a row are survivable, instead of the
            // single-block window that used to deadlock the transfer.
            io.write((uint8_t)ACK);
            io.flush();
          } else {
            // Sender is AHEAD of us: we missed a block. Ask for a resend. No
            // purging — the sender's retransmission must not be eaten.
            g_ymRetries++;
            io.write((uint8_t)NAK);
            io.flush();
          }
          break;

        case BLK_EOT:
          io.write((uint8_t)NAK);            // YMODEM: NAK the first EOT...
          readBlock(io, &seq, payload, &plen, 3000); // ...sender resends EOT...
          io.write((uint8_t)ACK);            // ...ACK the second.
          done = true;
          break;

        case BLK_CAN:
          fileOk = false; done = true;       // sender cancelled (Tera Term "Cancel")
          break;

        case BLK_BADCRC:
        case BLK_TIMEOUT:
        default:
          // A cancelled sender simply stops talking, so NAKing forever would
          // hang us in RECEIVING. Give up after a few dead attempts.
          // Give up on wall-clock silence, not on a retry count: a sender that
          // simply vanished should not keep us in RECEIVING for a minute.
          if (millis() - lastProgress > 12000) { fileOk = false; done = true; break; }
          g_ymRetries++;
          io.write((uint8_t)NAK);            // request retransmit (no purge!)
          io.flush();
          break;
      }
    }

    if (fileOk && cb.onFileEnd()) {
      filesReceived++;
    } else {
      cb.onAbort();                          // discards the temp file
      // Drain any trailing CAN/garbage from the cancelled sender so the next
      // transfer starts on a clean line.
      uint32_t t0 = millis();
      while (millis() - t0 < 300) { if (io.read() < 0) delay(5); }
      return filesReceived;                  // end the batch; loop() re-probes
    }
    // loop back: sender now sends the next file's header (or the empty one)
  }
}
