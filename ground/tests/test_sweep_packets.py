"""End-to-end host test of the tether sweep downlink, without hardware.

1. Compiles the real firmware packing code (platform/sweep_packet.c,
   lib/ax25/ax25.c, platform/tether_sim_data.c) with gcc and gets the exact
   AX.25 frames the MSP430 would send for a sweep.
2. Wraps each frame the way the CC1101 does (preamble, sync word, length
   byte, CRC-16) and turns it into the float stream the GNU Radio FSK
   demodulator would produce.
3. Runs that through the real decoder block (lil_decoder.blk) and checks
   that readings.txt receives exactly the right 50 values.

GNU Radio is replaced by a tiny stub, so this runs with plain Python + gcc:

    python3 ground/tests/test_sweep_packets.py
"""

import re
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
GROUND = HERE.parent
REPO = GROUND.parent
PLATFORM = REPO / "platform"
AX25_LIB = REPO / "lib" / "ax25"

# --- Stub out gnuradio so lil_decoder imports without it -------------------
gr_stub = types.SimpleNamespace(sync_block=type("sync_block", (), {"__init__": lambda self, **kw: None}))
sys.modules.setdefault("gnuradio", types.SimpleNamespace(gr=gr_stub))
sys.modules.setdefault("gnuradio.gr", gr_stub)
sys.path.insert(0, str(GROUND))

import lil_decoder  # noqa: E402  (needs the stub above)

SPS = 4                     # samples per bit in the synthetic stream
SYNC = 0xD391


def load_table():
    """Parse tether_sim_data.c back into a list of sweeps."""
    text = (PLATFORM / "tether_sim_data.c").read_text()
    sweeps = []
    for block in re.findall(r"\{([^{}]*)\}", text.split("=", 1)[1]):
        values = [int(v) for v in re.findall(r"-?\d+(?=L)", block)]
        if values:
            sweeps.append(values)
    return sweeps


def build_helper(out_dir: Path) -> Path:
    exe = out_dir / "host_sweep_frames"
    subprocess.run(
        ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
         f"-I{PLATFORM}", f"-I{AX25_LIB}",
         str(HERE / "host_sweep_frames.c"),
         str(PLATFORM / "sweep_packet.c"),
         str(PLATFORM / "tether_sim_data.c"),
         str(AX25_LIB / "ax25.c"),
         "-o", str(exe)],
        check=True,
    )
    return exe


def frames_for(exe: Path, sweep_id: int, index: int) -> list[bytes]:
    out = subprocess.run([str(exe), str(sweep_id), str(index)],
                         check=True, capture_output=True, text=True).stdout
    return [bytes.fromhex(line) for line in out.split()]


def cc1101_stream(frame: bytes) -> list[float]:
    """What the demodulator outputs for one CC1101 packet: +1/-1 per bit."""
    body = bytes([len(frame)]) + frame
    crc = lil_decoder.crc16(body)
    packet = b"\xAA\xAA" + SYNC.to_bytes(2, "big") + body + crc.to_bytes(2, "big") + b"\x00\x00"
    bits = [(byte >> (7 - i)) & 1 for byte in packet for i in range(8)]
    return [1.0 if b else -1.0 for b in bits for _ in range(SPS)]


class SweepDownlinkTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.exe = build_helper(Path(cls.tmp.name))
        cls.table = load_table()

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def make_decoder(self, clock=None):
        tmp = Path(self.tmp.name)
        readings = tmp / f"readings_{self.id().rsplit('.', 1)[-1]}.txt"
        dec = lil_decoder.blk(sps=SPS, log_path=str(tmp / "log.txt"))
        dec.start()
        dec.sweeps = lil_decoder.SweepAssembler(
            str(readings), **({"clock": clock} if clock else {}))
        self.addCleanup(dec.stop)
        return dec, readings

    def feed(self, dec, frames):
        for frame in frames:
            # Leading noise-free silence (all -1) between packets.
            dec.work([[-1.0] * (SPS * 40) + cc1101_stream(frame)], None)

    def read_values(self, readings):
        if not readings.exists():
            return []
        return [int(v) for v in readings.read_text().split()]

    def test_frames_fit_cc1101(self):
        frames = frames_for(self.exe, 7, 0)
        self.assertEqual(len(frames), lil_decoder.SWEEP_CHUNKS)
        self.assertTrue(all(len(f) <= 61 for f in frames), [len(f) for f in frames])

    def test_full_sweep_round_trip(self):
        dec, readings = self.make_decoder()
        self.feed(dec, frames_for(self.exe, 3, 2))
        expected = [v * lil_decoder.RADIO_TO_READINGS for v in self.table[2]]
        self.assertEqual(self.read_values(readings), expected)

    def test_two_sweeps_append(self):
        dec, readings = self.make_decoder()
        self.feed(dec, frames_for(self.exe, 0, 0))
        self.feed(dec, frames_for(self.exe, 1, 1))
        values = self.read_values(readings)
        self.assertEqual(len(values), 100)
        self.assertEqual(values[50:], [v * 10**7 for v in self.table[1]])

    def test_missing_chunk_drops_only_that_sweep(self):
        dec, readings = self.make_decoder()
        frames = frames_for(self.exe, 10, 0)
        self.feed(dec, frames[:2] + frames[3:])         # chunk 3 lost
        self.feed(dec, frames_for(self.exe, 11, 1))     # next sweep is fine
        self.assertEqual(self.read_values(readings),
                         [v * 10**7 for v in self.table[1]])
        self.assertEqual(dec.sweeps.dropped, 1)

    def test_repeated_sweep_written_once(self):
        dec, readings = self.make_decoder()
        frames = frames_for(self.exe, 20, 4)
        self.feed(dec, frames)
        self.feed(dec, frames)
        self.assertEqual(len(self.read_values(readings)), 50)

    def test_obc_restart_sweeps_not_dropped(self):
        """After an OBC reset its ids restart at 0: those are new sweeps."""
        dec, readings = self.make_decoder()
        for sweep_id in (0, 1, 2):                      # first run
            self.feed(dec, frames_for(self.exe, sweep_id, sweep_id))
        for sweep_id in (0, 1):                         # OBC restarted
            self.feed(dec, frames_for(self.exe, sweep_id, sweep_id))
        self.assertEqual(len(self.read_values(readings)), 5 * 50)
        self.assertEqual(dec.sweeps.restarts, 1)

    def test_same_id_after_window_is_written(self):
        """Same id as the last sweep, but long after it: a restart, not a repeat."""
        now = [1000.0]
        dec, readings = self.make_decoder(clock=lambda: now[0])
        frames = frames_for(self.exe, 0, 0)
        self.feed(dec, frames)
        now[0] += lil_decoder.SweepAssembler.DUPLICATE_WINDOW_S + 5
        self.feed(dec, frames)
        self.assertEqual(len(self.read_values(readings)), 100)

    def test_counter_packets_still_decode(self):
        """Old-style text packets must still log, without touching readings."""
        dec, readings = self.make_decoder()
        hdr = frames_for(self.exe, 0, 0)[0][:16]
        body = hdr + b"TESTAX25 01"
        frame = body + lil_decoder.fcs_x25(body).to_bytes(2, "little")
        self.feed(dec, [frame])
        self.assertEqual(dec.count, 1)
        self.assertFalse(readings.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
