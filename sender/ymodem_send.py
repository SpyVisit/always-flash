"""
ymodem_send.py — YMODEM (1K) sender for ALWAYS FLASH.

Protocol notes (learned the hard way while building the receiver):
  * The RECEIVER drives the handshake: it emits 'C' when it is ready.
  * Strict stop-and-wait: we send one block, then wait for ACK before the next.
    That is also our flow control - the device can take as long as it needs.
  * On NAK we resend the SAME block. We must NOT skip ahead, and we must not
    throw away buffered data on either side (that bug cost us a whole evening).
"""
import os
import time

SOH = 0x01   # 128-byte block
STX = 0x02   # 1024-byte block
EOT = 0x04
ACK = 0x06
NAK = 0x15
CAN = 0x18
C   = 0x43   # 'C' - receiver is ready, CRC mode

BLOCK = 1024
MAX_RETRY = 20


def crc16(data: bytes) -> int:
    """CRC-16/XMODEM (poly 0x1021, init 0x0000)."""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def make_block(seq: int, payload: bytes, size: int = BLOCK) -> bytes:
    lead = STX if size == 1024 else SOH
    p = payload.ljust(size, b'\x1a')          # pad with SUB, per YMODEM
    c = crc16(p)
    return bytes([lead, seq & 0xFF, (255 - (seq & 0xFF)) & 0xFF]) + p + bytes([c >> 8, c & 0xFF])


class YmodemError(Exception):
    pass


def _wait_for(ser, wanted, timeout):
    """Wait for one of the bytes in `wanted`. Returns it, or None on timeout."""
    end = time.time() + timeout
    while time.time() < end:
        b = ser.read(1)
        if b:
            v = b[0]
            if v in wanted:
                return v
            if v == CAN:
                return CAN
    return None


def send_file(ser, path, progress=None, cancel=None):
    """
    Send one file over an open serial port using YMODEM-1K.
    progress(sent_bytes, total_bytes, retries) - optional callback
    cancel() -> bool                            - optional, abort if it returns True
    """
    name = os.path.basename(path)
    size = os.path.getsize(path)

    def stopped():
        return cancel is not None and cancel()

    # --- wait for the receiver to say it's ready ---
    ser.reset_input_buffer()
    if _wait_for(ser, (C,), 15) != C:
        raise YmodemError("Device is not responding.\n"
                          "Is ALWAYS FLASH plugged in and showing READY?")

    retries = 0

    # --- block 0: filename + size ---
    header = name.encode('utf-8', 'replace') + b'\x00' + str(size).encode() + b' '
    blk0 = make_block(0, header)
    for attempt in range(MAX_RETRY):
        if stopped():
            _cancel(ser); raise YmodemError("Cancelled.")
        ser.write(blk0); ser.flush()
        r = _wait_for(ser, (ACK, NAK, CAN), 10)
        if r == ACK:
            break
        if r == CAN:
            raise YmodemError("Device refused the file (out of space?).")
        retries += 1
    else:
        raise YmodemError("Device never acknowledged the file header.")

    # receiver now asks for the data with another 'C'
    if _wait_for(ser, (C,), 10) != C:
        raise YmodemError("Device did not start the data phase.")

    # --- data blocks ---
    sent = 0
    seq = 1
    with open(path, 'rb') as f:
        while True:
            if stopped():
                _cancel(ser); raise YmodemError("Cancelled.")
            chunk = f.read(BLOCK)
            if not chunk:
                break
            blk = make_block(seq, chunk)

            for attempt in range(MAX_RETRY):
                if stopped():
                    _cancel(ser); raise YmodemError("Cancelled.")
                ser.write(blk); ser.flush()
                r = _wait_for(ser, (ACK, NAK, CAN), 10)
                if r == ACK:
                    break
                if r == CAN:
                    raise YmodemError("Device aborted the transfer.")
                # NAK or timeout -> resend the SAME block, never skip
                retries += 1
            else:
                raise YmodemError(f"Block {seq} failed after {MAX_RETRY} attempts.")

            sent += len(chunk)
            seq = (seq + 1) & 0xFF
            if progress:
                progress(sent, size, retries)

    # --- end of file: EOT, NAK, EOT, ACK ---
    for attempt in range(MAX_RETRY):
        ser.write(bytes([EOT])); ser.flush()
        r = _wait_for(ser, (ACK, NAK), 10)
        if r == NAK:
            ser.write(bytes([EOT])); ser.flush()
            if _wait_for(ser, (ACK,), 10) == ACK:
                break
        elif r == ACK:
            break
        retries += 1
    else:
        raise YmodemError("Device did not confirm the end of the file.")

    # --- end of batch: empty header block ---
    if _wait_for(ser, (C,), 10) == C:
        ser.write(make_block(0, b'')); ser.flush()
        _wait_for(ser, (ACK,), 10)

    return sent, retries


def _cancel(ser):
    try:
        ser.write(bytes([CAN]) * 8)
        ser.flush()
    except Exception:
        pass
