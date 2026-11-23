"""Debug helper: diff a received sweep frame against what the firmware should send.

Paste the raw=... hex from a FCS=BAD line in decoded_packets.txt:

    python3 ground/tests/compare_frame.py <hex>

It reads the sweep id and chunk index from the frame's own header, rebuilds
the expected frame with the real firmware C code (via host_sweep_frames.c,
assuming table index = sweep_id % TETHER_SIM_SWEEP_COUNT, which only holds
if the board has not reset since sweep 0), and prints every byte position
with the XOR of the two, so you can see whether errors are random bit flips,
a bit shift, or start at a fixed position.
"""

import sys
import tempfile
from pathlib import Path

from test_sweep_packets import build_helper, frames_for, load_table


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    rx = bytes.fromhex(sys.argv[1])
    if len(rx) < 21 or rx[16] != 0x54:
        sys.exit("not a sweep frame (no 'T' after the 16-byte AX.25 header)")

    sweep_id = rx[17] | (rx[18] << 8)
    chunk = rx[19]
    index = sweep_id % len(load_table())

    with tempfile.TemporaryDirectory() as tmp:
        expected = frames_for(build_helper(Path(tmp)), sweep_id, index)[chunk]

    print(f"sweep {sweep_id} (table index {index}) chunk {chunk}: "
          f"received {len(rx)} bytes, expected {len(expected)}")
    first_bad = None
    for pos in range(max(len(rx), len(expected))):
        r = rx[pos] if pos < len(rx) else None
        e = expected[pos] if pos < len(expected) else None
        same = r == e
        if not same and first_bad is None:
            first_bad = pos
        xor = f"{r ^ e:08b}" if r is not None and e is not None else "--"
        print(f"{pos:3d}  rx={r if r is None else f'{r:02x}'}  "
              f"exp={e if e is None else f'{e:02x}'}  "
              f"{'  ' if same else '<<'} xor={xor}")
    print("identical" if first_bad is None else f"first difference at byte {first_bad}")


if __name__ == "__main__":
    main()
