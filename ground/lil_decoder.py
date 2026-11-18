"""
CC1101 packet decoder - GNU Radio Embedded Python Block.

This file is the canonical copy of the "CC1101 Packet Printer" block in
cc1101_rx.grc. GRC stores embedded-block code *inside* the .grc file, so
after editing this file, re-paste it into the block (double-click it ->
"Open in Editor"), or the two copies drift apart.

Input:  demodulated FSK as a float stream, `sps` samples per bit
        (positive = '1', negative = '0').
Output: none - decoded packets are printed and appended to a log file.

No clock recovery: instead, one independent decoder runs per sample
phase (sps of them), and whichever phase lands near the bit centre
decodes cleanly. Duplicates from neighbouring phases are suppressed.
"""

import os

import numpy as np
from gnuradio import gr


def crc16(data, c=0xFFFF):
    # CC1101 CRC-16, poly 0x8005
    for byte in data:
        c ^= byte << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def fcs_x25(data):
    # AX.25 FCS: CRC-16/X-25
    c = 0xFFFF
    for byte in data:
        c ^= byte
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c ^ 0xFFFF


def parse_ax25(frame):
    """Decode an AX.25 frame. Returns str or None."""

    if len(frame) < 17:
        return None

    body, rx_fcs = frame[:-2], frame[-2] | (frame[-1] << 8)
    fcs_ok = rx_fcs == fcs_x25(body)

    addrs, i = [], 0

    while True:
        if i + 7 > len(body):
            return None

        a = body[i:i + 7]

        call = ''.join(chr(b >> 1) for b in a[:6]).strip()
        ssid = (a[6] >> 1) & 0x0F

        addrs.append(call + (f"-{ssid}" if ssid else ""))

        i += 7

        if a[6] & 1 or len(addrs) >= 10:
            break

    if len(addrs) < 2 or i >= len(body):
        return None

    dest, src, digis = addrs[0], addrs[1], addrs[2:]

    ctrl = body[i]
    i += 1

    pid = None

    if (ctrl & 0x01) == 0 or (ctrl & 0xEF) == 0x03:
        if i < len(body):
            pid = body[i]
            i += 1

    info = body[i:]

    path = src + ">" + dest + (
        "," + ",".join(digis) if digis else ""
    )

    pid_s = f" pid={pid:02X}" if pid is not None else ""

    return (
        f"{path} ctrl={ctrl:02X}{pid_s} "
        f"FCS={'OK' if fcs_ok else 'BAD'} "
        f"info={info.decode('ascii', errors='replace')!r}"
    )


def default_log_path():
    """decoded_packets.txt next to this code, on whatever machine it runs.

    When GRC runs the flowgraph it writes this block out as
    <flowgraph>_epy_block_0.py beside the .grc file, so __file__ points
    into the ground/ folder on Linux, macOS or Windows alike. If __file__
    isn't defined (code exec'd some other way), use the current directory.
    """
    here = globals().get('__file__')
    base = os.path.dirname(os.path.abspath(here)) if here else os.getcwd()
    return os.path.join(base, 'decoded_packets.txt')


class blk(gr.sync_block):
    """CC1101 Packet Printer

    sps:       samples per bit of the input stream
    sync_word: 16-bit CC1101 sync word (SYNC1:SYNC0)
    log_path:  file to append decoded packets to. Empty = decoded_packets.txt
               next to the flowgraph. ~ and $VARS / %VARS% are expanded.
    """

    def __init__(self, sps=10, sync_word=0xD391, log_path=''):

        gr.sync_block.__init__(
            self,
            name='CC1101 Packet Printer',
            in_sig=[np.float32],
            out_sig=None
        )

        self.sps = int(sps)
        self.sync = sync_word
        self.log_path = log_path
        self.log_file = None

        self.n = 0

        self.st = [
            dict(sr=0, hunt=True, buf=[], need=8)
            for _ in range(self.sps)
        ]

        self.count = 0

        self.last = (None, -10**9)

    # The log is opened in start(), not __init__: GRC instantiates the
    # block while you edit the flowgraph (to read its ports and params),
    # and opening the file there would log a fake session on every edit.
    def start(self):

        if self.log_path:
            path = os.path.expandvars(os.path.expanduser(self.log_path))
        else:
            path = default_log_path()

        path = os.path.abspath(path)
        os.makedirs(os.path.dirname(path), exist_ok=True)

        # buffering=1 -> line buffered, so each packet hits the disk
        # immediately (nothing lost if the flowgraph is killed)
        self.log_file = open(path, "a", encoding="utf-8", buffering=1)
        self.log_file.write("\n========== NEW GNU RADIO SESSION ==========\n")

        print(f"[CC1101 decoder] logging packets to {path}")

        return True

    @staticmethod
    def _bytes(bits):

        return [
            int(''.join(map(str, bits[i:i+8])), 2)
            for i in range(0, len(bits), 8)
        ]

    def work(self, input_items, output_items):

        for v in input_items[0]:

            s = self.st[self.n % self.sps]

            self.n += 1

            b = 1 if v > 0 else 0

            # Search for synchronization word
            if s['hunt']:

                s['sr'] = ((s['sr'] << 1) | b) & 0xFFFF

                if s['sr'] == self.sync:

                    s['hunt'] = False
                    s['buf'] = []
                    s['need'] = 8

                continue

            # Collect packet bits
            s['buf'].append(b)

            # Read packet length
            if len(s['buf']) == 8:

                length = self._bytes(s['buf'])[0]

                if length > 61:

                    s['hunt'] = True
                    s['sr'] = 0

                    continue

                s['need'] = 8 + 8 * length + 16

            # Complete packet received
            if len(s['buf']) == s['need']:

                by = self._bytes(s['buf'])

                length = by[0]

                payload = bytes(by[1:1 + length])

                # CC1101 CRC check
                ok = (
                    ((by[-2] << 8) | by[-1])
                    == crc16(by[:1 + length])
                )

                s['hunt'] = True
                s['sr'] = 0

                # Save valid packets only (and skip the same packet
                # decoded again by a neighbouring sample phase)
                if ok and not (
                    payload == self.last[0]
                    and self.n - self.last[1] < 50 * self.sps * 8
                ):

                    self.count += 1

                    self.last = (payload, self.n)

                    decoded = (
                        parse_ax25(payload)
                        or 'not AX.25: ' + repr(payload)
                    )

                    message = (
                        f"[pkt {self.count}] "
                        f"CC1101 CRC=OK | {decoded}"
                    )

                    print(message)

                    if self.log_file:
                        self.log_file.write(message + "\n")

        return len(input_items[0])

    # Close file when GNU Radio stops the block
    def stop(self):

        if self.log_file and not self.log_file.closed:

            self.log_file.close()

        return True
