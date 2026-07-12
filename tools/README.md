# Diagnostics

Three throwaway sketches that were essential to debugging ALWAYS FLASH. Kept
because they're genuinely useful if you fork this or hit the same walls.

Each is a standalone Arduino sketch — open the folder, flash it, read the output.
Same board settings as the firmware (ESP32S3 Dev Module, **USB CDC On Boot:
Enabled**, PSRAM: OPI PSRAM).

## `SD_Diagnostic/`
Console only. Tries to mount the microSD in every SDMMC mode and speed (1-bit and
4-bit, from max clock down to 400 kHz probing) and reports which combination
works, plus card type and size. Answers *"is the card even mounting, and how?"*.
This is what revealed the card wanted 4-bit mode.

## `SD_WriteBench/`
Console only. Writes 24 MB to the card using the exact same buffering pattern as
the firmware, but with **no USB involved**, and prints a latency histogram of
every flush. Answers *"is the SD card the bottleneck?"* — it wasn't (a flat
~1015 KB/s), which redirected the whole investigation to the USB path.

## `AF_Diag/`
A full instrumented receiver. Receives like the real firmware but records a
ring-buffer trace of every protocol anomaly. When a transfer stalls it freezes
and prints the trace repeatedly to the serial port — showing whether the failure
is a silent line, corrupted data, or a sequence-number desync. This is what
finally pinned the deadlock on `purgeRx`.

**How to read a trace:** cancel the stuck transfer to reveal the terminal, and
look at the *first* anomaly. `LEAD_TIMEOUT`/`got=0` means the sender went silent
(our ACK never arrived); `CRC_BAD` means bytes are being mangled; a run of
`SEQ_UNEXPECT` means sender and receiver have lost sync.
