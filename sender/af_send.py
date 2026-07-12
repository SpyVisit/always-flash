"""
af_send.py — ALWAYS FLASH sender.

A single portable window: pick a file, watch it go, see the result.
No install, no admin rights, no extra files.

Build to one .exe:  see build.bat
"""
import os
import sys
import threading
import time
import queue
import tkinter as tk
from tkinter import filedialog, ttk

import serial
import serial.tools.list_ports

from ymodem_send import send_file, YmodemError, C

APP_NAME = "ALWAYS FLASH"
VERSION = "1.0"

# Colours (roughly matching the device's own screen)
BG = "#0b0f0b"
FG = "#e8e8e8"
GREEN = "#35d94b"
AMBER = "#f5a623"
RED = "#e5484d"
DIM = "#7a8a7a"


# ----------------------------------------------------------------- port finding
def find_device(timeout=2.0):
    """
    Find the ALWAYS FLASH device.

    We don't rely on VID/PID alone (dev boards report all sorts of IDs).
    Instead we listen: the receiver announces itself by emitting 'C' about once
    a second. That is a positive identification of a waiting YMODEM receiver.
    """
    candidates = list(serial.tools.list_ports.comports())

    # Try likely ones first: Espressif / common USB-serial bridges.
    def score(p):
        s = 0
        text = f"{p.description} {p.manufacturer or ''} {p.hwid}".lower()
        if p.vid == 0x303A:                 # Espressif
            s += 10
        if "esp32" in text or "espressif" in text:
            s += 5
        if "usb serial" in text or "cdc" in text:
            s += 2
        return -s

    for p in sorted(candidates, key=score):
        try:
            with serial.Serial(p.device, 115200, timeout=0.2) as ser:
                end = time.time() + timeout
                while time.time() < end:
                    b = ser.read(1)
                    if b and b[0] == C:      # heard the receiver's 'C'
                        return p.device
        except Exception:
            continue
    return None


# ----------------------------------------------------------------------- the app
class App:
    def __init__(self, root):
        self.root = root
        self.path = None
        self.port = None
        self.sending = False
        self.cancel_flag = False
        self.events = queue.Queue()

        root.title(f"{APP_NAME} — sender")
        root.configure(bg=BG)
        root.resizable(False, False)

        pad = dict(padx=16)

        tk.Label(root, text=APP_NAME, bg=BG, fg=GREEN,
                 font=("Consolas", 20, "bold")).pack(pady=(16, 0), **pad)
        tk.Label(root, text="append-only USB storage", bg=BG, fg=DIM,
                 font=("Consolas", 9)).pack(pady=(0, 12), **pad)

        # device row
        self.lbl_dev = tk.Label(root, text="Looking for device…", bg=BG, fg=AMBER,
                                font=("Consolas", 10))
        self.lbl_dev.pack(anchor="w", **pad)

        # file row
        f = tk.Frame(root, bg=BG)
        f.pack(fill="x", pady=(12, 4), **pad)
        self.btn_pick = tk.Button(f, text="Choose file…", command=self.pick,
                                  bg="#1c241c", fg=FG, activebackground="#2a352a",
                                  activeforeground=FG, relief="flat",
                                  font=("Consolas", 10), padx=12, pady=4,
                                  cursor="hand2")
        self.btn_pick.pack(side="left")
        self.lbl_file = tk.Label(f, text="no file selected", bg=BG, fg=DIM,
                                 font=("Consolas", 9), anchor="w")
        self.lbl_file.pack(side="left", padx=(10, 0))

        # progress
        style = ttk.Style()
        style.theme_use("default")
        style.configure("AF.Horizontal.TProgressbar", troughcolor="#1c241c",
                        background=GREEN, bordercolor=BG,
                        lightcolor=GREEN, darkcolor=GREEN)
        self.bar = ttk.Progressbar(root, style="AF.Horizontal.TProgressbar",
                                   length=440, mode="determinate")
        self.bar.pack(pady=(10, 4), **pad)

        self.lbl_stat = tk.Label(root, text="", bg=BG, fg=FG,
                                 font=("Consolas", 9))
        self.lbl_stat.pack(anchor="w", **pad)

        # send button
        self.btn_send = tk.Button(root, text="SEND", command=self.on_send,
                                  bg=GREEN, fg="#07120a", activebackground="#5ee870",
                                  relief="flat", font=("Consolas", 12, "bold"),
                                  padx=30, pady=6, cursor="hand2",
                                  state="disabled")
        self.btn_send.pack(pady=(12, 4))

        self.lbl_hint = tk.Label(root, text="Files can only be added. "
                                            "To delete, remove the card.",
                                 bg=BG, fg=DIM, font=("Consolas", 8))
        self.lbl_hint.pack(pady=(0, 14))

        self.root.after(100, self.pump)
        threading.Thread(target=self.scan, daemon=True).start()

    # ---------------------------------------------------------------- device scan
    def scan(self):
        port = find_device()
        self.events.put(("device", port))

    def rescan(self):
        self.lbl_dev.config(text="Looking for device…", fg=AMBER)
        threading.Thread(target=self.scan, daemon=True).start()

    # -------------------------------------------------------------------- actions
    def pick(self):
        p = filedialog.askopenfilename(title="Choose a file to send")
        if p:
            self.path = p
            name = os.path.basename(p)
            size = os.path.getsize(p)
            shown = name if len(name) <= 32 else name[:29] + "..."
            self.lbl_file.config(text=f"{shown}  ({self.human(size)})", fg=FG)
            self.update_send_button()

    def update_send_button(self):
        ok = bool(self.path) and bool(self.port) and not self.sending
        self.btn_send.config(state="normal" if ok else "disabled")

    def on_send(self):
        if self.sending:
            self.cancel_flag = True
            return
        self.sending = True
        self.cancel_flag = False
        self.btn_send.config(text="CANCEL", bg=AMBER)
        self.btn_pick.config(state="disabled")
        self.bar["value"] = 0
        threading.Thread(target=self.worker, daemon=True).start()

    def worker(self):
        t0 = time.time()
        try:
            with serial.Serial(self.port, 115200, timeout=0.5, write_timeout=10) as ser:
                def prog(sent, total, retries):
                    self.events.put(("progress", (sent, total, retries, time.time() - t0)))

                sent, retries = send_file(
                    ser, self.path,
                    progress=prog,
                    cancel=lambda: self.cancel_flag,
                )
            self.events.put(("done", (sent, retries, time.time() - t0)))
        except YmodemError as e:
            self.events.put(("error", str(e)))
        except serial.SerialException as e:
            self.events.put(("error", f"Serial port error:\n{e}"))
        except Exception as e:
            self.events.put(("error", f"{type(e).__name__}: {e}"))

    # ------------------------------------------------------------------- ui pump
    def pump(self):
        try:
            while True:
                kind, data = self.events.get_nowait()

                if kind == "device":
                    if data:
                        self.port = data
                        self.lbl_dev.config(text=f"Device ready on {data}", fg=GREEN)
                    else:
                        self.port = None
                        self.lbl_dev.config(
                            text="Device not found — plug it in, then click here",
                            fg=RED, cursor="hand2")
                        self.lbl_dev.bind("<Button-1>", lambda e: self.rescan())
                    self.update_send_button()

                elif kind == "progress":
                    sent, total, retries, dt = data
                    pct = 100.0 * sent / total if total else 0
                    self.bar["value"] = pct
                    speed = sent / dt / 1024 if dt > 0.3 else 0
                    eta = (total - sent) / (speed * 1024) if speed > 1 else 0
                    txt = f"{self.human(sent)} / {self.human(total)}   {pct:5.1f}%"
                    if speed:
                        txt += f"   {speed:.0f} KB/s"
                    if eta > 1:
                        txt += f"   ETA {int(eta // 60)}:{int(eta % 60):02d}"
                    if retries:
                        txt += f"   (retries {retries})"
                    self.lbl_stat.config(text=txt, fg=FG)

                elif kind == "done":
                    sent, retries, dt = data
                    self.bar["value"] = 100
                    speed = sent / dt / 1024 if dt else 0
                    self.lbl_stat.config(
                        text=f"Saved on device.  {self.human(sent)} in "
                             f"{int(dt // 60)}:{int(dt % 60):02d} "
                             f"({speed:.0f} KB/s, {retries} retries)",
                        fg=GREEN)
                    self.finish()

                elif kind == "error":
                    self.lbl_stat.config(text=data, fg=RED)
                    self.bar["value"] = 0
                    self.finish()
        except queue.Empty:
            pass
        self.root.after(80, self.pump)

    def finish(self):
        self.sending = False
        self.btn_send.config(text="SEND", bg=GREEN)
        self.btn_pick.config(state="normal")
        self.update_send_button()

    @staticmethod
    def human(n):
        for unit in ("B", "KB", "MB", "GB"):
            if n < 1024 or unit == "GB":
                return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
            n /= 1024


def main():
    root = tk.Tk()
    try:
        root.iconbitmap(default="")   # no icon file needed
    except Exception:
        pass
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
