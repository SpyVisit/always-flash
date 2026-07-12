# ALWAYS FLASH 2.5 — Waveshare ESP32-S3-LCD-1.47

An **append-only USB "flash drive."** The board shows up on the host as a USB
serial (COM) port; files pushed over it via **YMODEM** land on the microSD card
as normal FAT32 files, names preserved. The host **cannot delete or modify**
anything — no Mass Storage volume is exposed. Removal = pull the card.

This is the **base build**: automatic receive + on-screen status + an RGB status
LED. No buttons/keyboard (this board only has BOOT + RGB). A single-button layer
(lock toggle / file browser) can be added later.

---

## Board & flashing (Arduino IDE)

1. Install **arduino-esp32** core **≥ 3.0.2** and the **LovyanGFX** library.
2. Board: **ESP32S3 Dev Module**. Under **Tools**:
   - **USB CDC On Boot: Enabled**   ← makes `Serial` the native USB CDC
   - **USB Mode:** Hardware CDC and JTAG (USB-OTG also works)
   - **PSRAM: OPI PSRAM**           ← required (chip is ESP32-S3**R8**)
   - **Flash Size: 16MB (128Mb)**
   - **CPU Frequency: 240MHz**, **Flash Mode: QIO 80MHz**
   - **Partition Scheme:** any 16M scheme
3. Put all files (`AlwaysFlash.ino`, `LGFX_Config.h`, `ymodem.*`, `storage.*`)
   in one folder named `AlwaysFlash` and upload.

Upload won't start? Hold **BOOT**, tap **RESET**, release **BOOT** (download
mode), then upload again.

---

## Hardware map (Waveshare wiki)

| Function        | ESP32-S3 GPIO                            |
|-----------------|------------------------------------------|
| microSD (SDMMC) | CLK 14, CMD 15, D0 16, D1 18, D2 17, D3 21 (4-bit) |
| LCD (ST7789)    | SCLK 40, MOSI 45, CS 42, DC 41, RST 39, BL 48 |
| RGB LED (WS2812)| 38                                       |
| Button          | BOOT (GPIO0)                             |

The card is on the **SDMMC** peripheral, so this build uses the **`SD_MMC`**
library in **4-bit** mode (`storage.cpp`) — this is what the factory firmware
uses and what the diagnostic confirmed works on this board. The LCD is on its
own SPI bus, separate from the card (no bus contention).

> **Critical:** **USB CDC On Boot must be Enabled.** With it disabled, `Serial`
> goes to UART0 instead of USB — the board can't receive files and prints
> nothing to the Serial Monitor. This was the root cause of early "nothing
> works" symptoms.

---

## Use it (no client code)

The board appears as a new COM port. Any terminal with YMODEM send works.

**Windows — Tera Term:** open the port (any baud), then
**File → Transfer → YMODEM → Send…**, pick file(s), Send. Watch the screen.

**Linux/macOS — lrzsz:** `sb -k file.bin < /dev/ttyACM0 > /dev/ttyACM0`

Duplicate names are saved as `name_1.ext`, `name_2.ext`, …

### On screen / LED

The display shows state, session count, the current file's progress bar, and a
**card-fullness bar** (green → amber >75% → red >90%). The RGB LED mirrors state:
blue = idle/ready, amber = receiving, green = saved, red = error / DISK FULL /
no card. If a file's declared size won't fit, it's refused (**DISK FULL**) before
any data is written.

---

## Behavior notes

- **Atomic save:** data streams into `/~t*.par` and is renamed to the final name
  only on a clean EOT. A cancelled or power-lost transfer never produces a
  corrupt target file — just an orphan `.par`, which is swept at next boot.
- **Cancel is handled.** If the sender cancels or vanishes, the receiver gives up
  after a few dead attempts, deletes the temp file, drains the line, and returns
  to READY — it cannot hang in RECEIVING.
- **Collisions:** a duplicate name is saved as `name_1.ext`, `name_2.ext`, …
- **Pre-flight fit check:** the YMODEM header carries the file size, so a file
  that will not fit is rejected up front (DISK FULL) before any data is written.
- **`rtx` counter** on screen shows re-requested blocks. It should stay at 0;
  a rising red count means blocks are arriving corrupted.

## Performance: the three things that mattered

Throughput started at ~200 B/s and ended at ~90 KB/s. Three separate bugs, each
masking the next:

1. **Screen redraw inside the data path.** Pushing the 320x172 sprite blocks SPI
   for ~22 ms; the USB-CDC RX ring overflows in <1 ms. Incoming 1 KB blocks were
   shredded → CRC fail → NAK → endless retries. *Fix:* all slow work moved to an
   `onIdle` callback the receiver invokes only after an ACK is out — inside the
   protocol's own quiet window.
2. **No yield in the read loop.** The USB bytes are delivered by a separate
   FreeRTOS task; a tight busy-wait starved the very task we were waiting on.
   *Fix:* yield while waiting.
3. **Sleeping on every micro-gap.** USB delivers a 1 KB block as ~16 packets of
   64 B, so `available()` briefly hits zero *between packets*. A full
   `vTaskDelay(1)` on each dip cost up to 16 sleeps per block (~90 ms → 11 KB/s),
   and got worse as the card got busier. *Fix:* adaptive wait — cheap
   `taskYIELD()` while data is streaming, real sleep only on a genuine lull.

**Deferred SD writes.** `storageWrite()` only memcpy's into a 32 KB PSRAM buffer
(fast, never blocks). The real card write happens in `storageService()`, called
from `onIdle` — so the slow I/O lands in a protocol pause instead of racing the
incoming data. Large chunks also keep the card off its slow read-modify-write
path.

## Tuning notes

- **Display offset:** `LGFX_Config.h` uses `offset_x = 34` for the 172-wide
  ST7789. If the image is shifted, adjust it (33/34/35); toggle `invert` /
  `rgb_order` if colors look wrong.
- **Upside down?** Change `lcd.setRotation(1)` to `setRotation(3)` in
  `AlwaysFlash.ino`.
- **Card mode:** mounts 4-bit at max SDMMC speed (confirmed working on this
  board), with slower retries as fallback.

## Next step (optional)

A one-button (BOOT) layer: short press = ARMED⇄LOCKED (LOCKED ignores the wire),
long press = file browser using the screen. Say the word and I'll add it.
