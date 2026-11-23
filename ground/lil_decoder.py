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

Tether sweeps: packets whose AX.25 info starts with 'T' are chunks of a
50-point tether I-V sweep (layout in platform/sweep_packet.h). They are
reassembled by SweepAssembler, and each complete sweep is appended to
iv_viewer/readings.txt for the I-V viewer.
"""

import os
import struct
import time

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


def parse_ax25_fields(frame):
    """Split an AX.25 frame into its fields. Returns a dict or None."""

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

    info = bytes(body[i:])

    path = src + ">" + dest + (
        "," + ",".join(digis) if digis else ""
    )

    return dict(path=path, ctrl=ctrl, pid=pid, fcs_ok=fcs_ok, info=info)


def format_ax25(fields, info_text=None):
    """One log line for a parsed frame. info_text replaces the raw info."""

    pid = fields['pid']
    pid_s = f" pid={pid:02X}" if pid is not None else ""

    if info_text is None:
        info_text = f"info={fields['info'].decode('ascii', errors='replace')!r}"

    return (
        f"{fields['path']} ctrl={fields['ctrl']:02X}{pid_s} "
        f"FCS={'OK' if fields['fcs_ok'] else 'BAD'} {info_text}"
    )


def parse_ax25(frame):
    """Decode an AX.25 frame. Returns str or None."""

    fields = parse_ax25_fields(frame)
    return format_ax25(fields) if fields else None


# ---------------------------------------------------------------------------
# Tether sweeps. Must match platform/sweep_packet.h:
#   'T' | sweep_id u16 | chunk_index u8 | chunk_count u8 | n x int32
# all little-endian. Currents are in units of 1e-10 A (uA x 10^4).
# ---------------------------------------------------------------------------

SWEEP_TYPE = 0x54             # 'T'
SWEEP_HEADER = struct.Struct('<BHBB')
SWEEP_SAMPLES = 50
SWEEP_VALUES_PER_CHUNK = 9
SWEEP_CHUNKS = 6              # ceil(50 / 9)

# readings.txt (iv_viewer/reader.py) uses 1e-17 A; the radio sends 1e-10 A.
RADIO_TO_READINGS = 10**7


def chunk_value_count(index):
    """How many currents chunk `index` carries (9, 9, 9, 9, 9, 5)."""
    return min(SWEEP_VALUES_PER_CHUNK, SWEEP_SAMPLES - index * SWEEP_VALUES_PER_CHUNK)


def parse_sweep_chunk(info):
    """Unpack one sweep chunk. Returns (sweep_id, index, values) or None."""

    if len(info) < SWEEP_HEADER.size:
        return None

    kind, sweep_id, index, count = SWEEP_HEADER.unpack_from(info)

    if kind != SWEEP_TYPE or count != SWEEP_CHUNKS or index >= SWEEP_CHUNKS:
        return None

    n = chunk_value_count(index)
    if len(info) != SWEEP_HEADER.size + 4 * n:
        return None

    values = struct.unpack_from(f'<{n}i', info, SWEEP_HEADER.size)
    return sweep_id, index, values


class SweepAssembler:
    """Collects the chunks of one sweep and writes it out when complete.

    The viewer groups readings.txt into sweeps purely by counting 50
    values, so writing a partial sweep would shift every later sweep.
    Only complete sweeps are written: a lost chunk loses that one sweep.

    Duplicates: only the sweep just written, arriving again within
    DUPLICATE_WINDOW_S, is treated as a repeat. Any other id is written,
    including a LOWER one: that is the OBC restarting (its ids begin again
    at 0), and those sweeps are new data. (An earlier version remembered
    the last 64 ids and silently dropped every sweep after a restart.)
    """

    TIMEOUT_S = 30.0          # give up on a sweep that stops arriving
    DUPLICATE_WINDOW_S = 10.0  # same id again this soon = the same sweep

    def __init__(self, readings_path, clock=time.monotonic):
        self.readings_path = readings_path
        self.clock = clock
        self.sweep_id = None
        self.chunks = {}
        self.started = 0.0
        self.last_written = None       # id of the last sweep written
        self.last_written_at = 0.0
        self.dropped = 0
        self.restarts = 0              # times the sweep ids went backwards

    def add(self, sweep_id, index, values):
        """Store one chunk. Returns a status string for the log."""

        now = self.clock()

        if (sweep_id == self.last_written
                and now - self.last_written_at < self.DUPLICATE_WINDOW_S):
            return "duplicate, already written"
        stale = self.sweep_id is not None and now - self.started > self.TIMEOUT_S

        if sweep_id != self.sweep_id or stale:
            if self.chunks:
                self.dropped += 1   # previous sweep never completed
            self.sweep_id = sweep_id
            self.chunks = {}
            self.started = now

        self.chunks[index] = values

        if len(self.chunks) < SWEEP_CHUNKS:
            return f"{len(self.chunks)}/{SWEEP_CHUNKS} chunks"

        currents = [v for i in range(SWEEP_CHUNKS) for v in self.chunks[i]]
        self._append(currents)

        note = ""
        if self.last_written is not None and sweep_id < self.last_written:
            self.restarts += 1
            note = " (sweep id went back: OBC restarted?)"

        self.last_written = sweep_id
        self.last_written_at = now
        self.sweep_id = None
        self.chunks = {}
        return f"complete -> {SWEEP_SAMPLES} values written{note}"

    def _append(self, currents):
        path = self.readings_path
        os.makedirs(os.path.dirname(path), exist_ok=True)

        # Same separator rule as iv_viewer/simulator.py: never glue a new
        # value onto an unterminated last line.
        needs_newline = False
        if os.path.exists(path) and os.path.getsize(path) > 0:
            with open(path, 'rb') as f:
                f.seek(-1, os.SEEK_END)
                needs_newline = f.read(1) != b'\n'

        # Opened and closed per sweep, so the viewer's "Archive & clear"
        # can rotate the file between sweeps.
        with open(path, 'a', encoding='utf-8', newline='\n') as f:
            if needs_newline:
                f.write('\n')
            for value in currents:
                f.write(f"{value * RADIO_TO_READINGS}\n")


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


def default_readings_path():
    """iv_viewer/readings.txt beside this code (see default_log_path)."""
    here = globals().get('__file__')
    base = os.path.dirname(os.path.abspath(here)) if here else os.getcwd()
    return os.path.join(base, 'iv_viewer', 'readings.txt')


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

        self.sweeps = SweepAssembler(default_readings_path())

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

                    decoded = self._describe(payload)

                    message = (
                        f"{time.strftime('%H:%M:%S')} [pkt {self.count}] "
                        f"CC1101 CRC=OK | {decoded}"
                    )

                    print(message)

                    if self.log_file:
                        self.log_file.write(message + "\n")

        return len(input_items[0])

    def _describe(self, payload):
        """Log text for one packet; also feeds tether sweep chunks on."""

        fields = parse_ax25_fields(payload)
        if not fields:
            return 'not AX.25: ' + repr(payload)

        if not fields['fcs_ok']:
            # Full hex for debugging: the ASCII repr hides bytes >= 0x80.
            return format_ax25(fields) + f" raw={bytes(payload).hex()}"

        chunk = parse_sweep_chunk(fields['info'])
        if not chunk:
            return format_ax25(fields)

        sweep_id, index, values = chunk
        status = self.sweeps.add(sweep_id, index, values)
        return format_ax25(
            fields,
            f"sweep {sweep_id} chunk {index + 1}/{SWEEP_CHUNKS} "
            f"({len(values)} values) {status}",
        )

    # Close file when GNU Radio stops the block
    def stop(self):

        if self.log_file and not self.log_file.closed:

            self.log_file.close()

        return True
