# ALWAYS FLASH
![ALWAYS FLASH](d:/device.jpg)

**An append-only USB flash drive.**

Plug it into any computer and it appears as a serial (COM) port — not as a disk.
Files are pushed to it over YMODEM and land on a microSD card as ordinary FAT32
files, names and extensions preserved. The host **cannot delete or modify
anything**: no Mass Storage volume is ever exposed, so the operating system has
nothing to mount, nothing to format, and nothing to erase.

To remove a file you must physically pull the card out and put it in a reader.

The guarantee is structural, not a permission check. There is no delete command
in the protocol and no disk for the OS to touch — so there is nothing to bypass.

## Why this exists

Sometimes you need to get a file *onto* storage on a machine where mounting a USB
drive is blocked — locked-down corporate images, kiosk PCs, restrictive group
policy — and reconfiguring the machine isn't an option (or isn't yours to do).

ALWAYS FLASH sidesteps that entirely: it never asks to be mounted. To the OS it
is a serial device, not a disk, so USB-storage restrictions simply don't apply to
it. The file still ends up written to a real FAT32 card that you can later read in
any card reader. It's a data-loading path that goes *around* disk mounting rather
than through it.

(The same property is why deletion is impossible from the host — the two are the
same coin. If you want the convenience of a mountable disk, this isn't it, by
design.)

```
   ┌──────────┐   USB-CDC (a COM port)   ┌───────────────┐
   │   PC     │ ───────  YMODEM  ──────► │  ESP32-S3     │──► microSD (FAT32)
   │ af_send  │                          │  ALWAYS FLASH │
   └──────────┘   ◄── no disk exposed ── └───────────────┘
```

---

## What's in here

| Folder | What it is |
|--------|------------|
| `firmware/AlwaysFlash/` | The device firmware (Arduino sketch, ESP32-S3) |
| `sender/`   | The Windows sender — one portable `.exe` |
| `tools/` | Diagnostic sketches used to debug the thing (kept: they're useful) |

---

## The hardware

**Waveshare ESP32-S3-LCD-1.47** (SKU 28317) — nothing to solder.

- ESP32-S3R8: dual core, 8 MB PSRAM, 16 MB flash
- 1.47" ST7789 LCD, 172×320
- microSD slot on the **SDMMC** bus (not SPI)
- Native full-speed USB
- One RGB LED, one usable button (BOOT)

| Function | GPIO |
|----------|------|
| microSD (SDMMC, 4-bit) | CLK 14, CMD 15, D0 16, D1 18, D2 17, D3 21 |
| LCD (ST7789, SPI) | SCLK 40, MOSI 45, CS 42, DC 41, RST 39, BL 48 |
| RGB LED | 38 |

The card is on SDMMC, so the firmware uses `SD_MMC` in **4-bit mode** — not the
`SD.h`/SPI library. The LCD sits on its own SPI bus and does not contend with it.

---

## Part 1 — the firmware

### Building it

1. Arduino IDE + **arduino-esp32 core ≥ 3.0.2**
2. Library: **LovyanGFX** (Library Manager)
3. Board: **ESP32S3 Dev Module**, and under **Tools**:

| Setting | Value |
|---------|-------|
| **USB CDC On Boot** | **Enabled** ← without this, nothing works |
| USB Mode | Hardware CDC and JTAG |
| PSRAM | **OPI PSRAM** ← the chip is an S3**R8** |
| Flash Size | 16MB (128Mb) |
| CPU Frequency | 240 MHz |

4. Put `AlwaysFlash.ino`, `ymodem.cpp/.h`, `storage.cpp/.h`, `LGFX_Config.h` in
   one folder named `AlwaysFlash`, and upload.

If the upload won't start: hold **BOOT**, tap **RESET**, release **BOOT**.

> **USB CDC On Boot is the single most important setting.** With it disabled,
> `Serial` goes to UART0 instead of USB: the device can't receive files and
> prints nothing to a terminal. It looks exactly like broken hardware. It isn't.

### The card

Format the microSD as **FAT32**. `SD_MMC` does not mount exFAT, and cards over
32 GB ship as exFAT from the factory — Windows won't even offer FAT32 for them
in its format dialog. Use [guiformat](http://ridgecrop.co.uk/index.htm?guiformat.htm)
or Rufus. (A 64 GB card, formatted FAT32, works fine.)

### The screen

```
ALWAYS FLASH 2.9
─────────────────────────────────────
St: RECEIVING                   2:16     ← state, and ETA in amber
teraterm-5.6.1-x64.zip
[██████████░░░░░░░░░░░░░░░░░░░░░░░]
3039 / 15762 KB   96 KB/s   rtx 0        ← rtx = re-requested blocks
Card 12%          7.6 / 63.8 GB
[████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]
YMODEM in. Pull card to delete.
```

The RGB LED mirrors the state: blue = idle, amber = receiving, green = saved,
red = error / disk full / no card.

**`rtx` should stay at 0.** It counts blocks the device had to ask for again. A
rising red count means the link is unhealthy.

---

## Part 2 — the sender

A single portable `.exe`. No installer, no admin rights, no DLLs, no Python.

### Building it

On any Windows box with Python 3.8+:

```
cd sender
build.bat
```

Result: **`dist\af_send.exe`** (~10 MB). Copy it anywhere. Install
[UPX](https://upx.github.io/) on your PATH first and it comes out roughly half
that size.

### Using it

1. Plug in the device, wait for **READY** on its screen.
2. Run `af_send.exe`. It says *"Device ready on COMx"*.
3. **Choose file…** → **SEND**. Watch progress, speed, ETA.

**It finds the device by itself.** Not by USB VID/PID — dev boards report all
sorts of nonsense (this one enumerates as an "Ozobot circuit kit"). Instead it
*listens*: an idle ALWAYS FLASH emits the character `C` once a second — that is
the YMODEM receiver announcing it's ready. The app opens each serial port and
picks the one that says `C`. Positive identification, not a guess.

You can also use any terminal that speaks YMODEM (Tera Term: *File → Transfer →
YMODEM → Send*), or `sb -k file` from `lrzsz` on Linux/macOS. No client is
required — the protocol is standard.

---

## How it works

### Why YMODEM

YMODEM is a file-transfer protocol from the BBS era (Chuck Forsberg, early '80s),
designed for a dumb, noisy, one-byte-at-a-time channel. It fits this project
almost by accident:

- **Integrity.** Every block carries a CRC-16. A corrupted block is re-requested.
- **Flow control, for free.** It is strictly stop-and-wait: the sender transmits
  one block and *waits for an ACK*. It physically cannot outrun the SD card. The
  device can take as long as it likes; the sender simply waits.
- **Metadata.** Filename and size travel in block 0, so names and extensions
  survive, and the device can refuse a file that won't fit before writing a byte.
- **It already exists.** Tera Term and lrzsz can drive it out of the box, so the
  firmware could be tested before any client was written.

The `C` character (0x43) is the receiver announcing *"I'm ready, CRC mode"*. In
YMODEM the **receiver** starts the conversation — which is why an idle device
trickles `C`s into the port, and why the sender can use that to find it.

### On the device

```
core 1                          core 0
──────                          ──────
YMODEM receiver                 SD writer task ──► microSD
  reads USB                       drains the ring, may block freely
  memcpy → ring buffer   ────►  [ 2 MB ring in PSRAM ]
  sends ACK
                                display task ──► LCD
(never blocks on anything)        repaints 4×/sec
```

The receive path touches **nothing slow**. No filesystem, no SPI, no display —
only a memcpy into a ring buffer. The card write and the screen live on the other
core, where they can stall for as long as they want without anyone waiting.

### Saving a file

1. Data streams into a temp file `/~t*.par`.
2. On a clean EOT, and only then, it is **renamed** to the final name.
3. If that name is taken, it becomes `name_1.ext`, `name_2.ext`, …

A cancelled transfer or a power loss therefore never produces a corrupt target
file — just an orphan `.par`, which is swept away on the next boot.

---

## Engineering log

Throughput went from 200 B/s to 108 KB/s, and "dies after 2 minutes" became
"107 MB in 16:29". Five separate bugs, each hiding the next. Worth recording,
because most of them are traps anyone doing this would fall into.

**1. `Serial` wasn't USB.** *USB CDC On Boot* was disabled, so everything went to
UART0. Nothing worked and nothing explained why. Always check this first.

**2. Redrawing the screen inside the data path.** Pushing the 320×172 sprite
blocks the SPI bus for ~22 ms; the USB-CDC receive buffer overflows in under a
millisecond. Incoming 1 KB blocks were being shredded mid-flight → CRC failure →
NAK → endless retries. **~200 B/s.**

**3. No yield in the read loop.** A tight `while (!available())` busy-wait on
ESP32 starves the FreeRTOS task that *delivers the USB bytes you are waiting
for*. You block the very thing you're waiting on. Adding a yield took it to
~90 KB/s.

**4. Yielding too eagerly.** USB hands over a 1 KB block as ~16 packets of 64
bytes, so `available()` dips to zero *between packets*. Sleeping a full RTOS tick
on each dip cost up to 16 sleeps per block (~90 ms) and throttled it to 11 KB/s —
and it got worse as the run went on. Fix: spin cheaply while data is streaming,
sleep only on a genuine lull.

**5. The cure that was the disease.** Transfers still died around 12–23 MB. The
culprit was a `purgeRx()` I had added *to fix desynchronization*: it drained the
receive buffer for 60 ms before every NAK. But the sender is already transmitting
its retransmission during that window — so the purge **ate it**. We fell
permanently behind, the sender ran ahead, every subsequent block was rejected as
out-of-sequence, and the transfer deadlocked.

The instrumented trace caught it: 23 633 blocks received, expecting block 82,
while Tera Term was already on packet 23 636. The sender was three blocks ahead.
Deleting `purgeRx` and resynchronizing properly — by *hunting for a valid lead
byte* instead of throwing data away — fixed it for good.

**What was never the problem:** the SD card. A standalone benchmark wrote 24 MB
at a flat **1015 KB/s** with 87 % of flushes under 10 ms. Ten times faster than
we needed. Every theory about card stalls was a dead end — which is exactly why
the benchmark was worth writing.

**Moral:** measure, don't guess. The two diagnostics (a card benchmark that
excluded USB entirely, and a receiver that dumps a trace of the failure) each
answered a question that days of reasoning had not.

---

## Limitations

- Files land in the card root; no subdirectories.
- No on-device file browser (there is only the BOOT button; the listing code
  exists in `storage.cpp` if you ever add buttons).
- The device cannot hand you the sender `.exe` — it doesn't present a disk, which
  is the whole point. Keep `af_send.exe` on the microSD next to your files, and
  fetch it with a card reader.
- One transfer at a time.

## Contributing

Issues and pull requests welcome. If you hit a transfer problem, the `tools/`
sketches (especially `AF_Diag`, which dumps a trace of exactly where a transfer
fails) will tell you far more than guesswork — they're the reason the engineering
log above is as specific as it is.

## Possible next steps

- A one-button UI on BOOT: arm/lock the receiver, browse what's on the card.
- A composite USB device (CDC + a tiny read-only MSC volume holding just the
  sender). Doable, but it puts a mountable disk in front of the OS and would
  fight antivirus policy — deliberately deferred.
