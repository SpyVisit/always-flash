# ALWAYS FLASH — sender

A small portable Windows app that sends a file to the ALWAYS FLASH device over
YMODEM. Pick a file, press SEND, watch it go.

- **One .exe.** No installer, no admin rights, no DLLs, no Python needed.
- **Finds the device by itself** — no COM port to choose.
- Shows progress, speed, ETA, and the final result.

## Using it

1. Plug in ALWAYS FLASH. Wait until its screen says **READY**.
2. Run `af_send.exe`.
3. It should say *"Device ready on COMx"*. (If not, plug the device in and click
   the red line to search again.)
4. **Choose file…**, then **SEND**.

Files can only be **added**. Sending a file whose name already exists saves it as
`name_1.ext`, `name_2.ext`, … To delete anything, pull the microSD card out and
use a card reader — the device deliberately never exposes itself as a disk.

## Building the .exe

On any Windows machine with Python 3.8+:

```
build.bat
```

The result is `dist\af_send.exe` (~10 MB). Copy it anywhere.

To make it smaller, install [UPX](https://upx.github.io/) and put it on your
PATH before building — PyInstaller will use it automatically and typically cuts
the size roughly in half.

## Running from source instead

```
pip install pyserial
python af_send.py
```

## How it finds the device

It doesn't trust USB IDs (dev boards report all sorts). It *listens*: an idle
ALWAYS FLASH emits the character `C` about once a second — that's the YMODEM
receiver announcing it is ready. The app opens each serial port in turn and
picks the one that says `C`. That's a positive identification, not a guess.

## Files

- `af_send.py`     — the GUI app
- `ymodem_send.py` — the YMODEM-1K sender (no GUI, reusable)
- `build.bat`      — builds the single-file .exe
