"""Raw append-only input boundary.

Change ``parse_raw_currents`` if the ground-station text format changes. The
rest of the application only deals with complete 50-reading ``Snapshot``
objects.
"""

from dataclasses import dataclass
from datetime import datetime
import os
from pathlib import Path
import re

SAMPLE_COUNT = 50
SCALE = 10**17


@dataclass(frozen=True)
class Snapshot:
    raw_currents: tuple[int, ...]


@dataclass(frozen=True)
class ReadBatch:
    sweeps: tuple[Snapshot, ...]
    pending_values: int
    source_size: int


def parse_raw_currents(text: str) -> tuple[int, ...]:
    """Parse signed decimal integers separated by whitespace and/or commas."""
    tokens = [token for token in re.split(r"[\s,]+", text.strip()) if token]
    try:
        currents = tuple(int(token, 10) for token in tokens)
    except ValueError as exc:
        raise ValueError("Input must contain only signed decimal integers") from exc
    if any(not -(2**63) <= value < 2**63 for value in currents):
        raise ValueError("A current exceeds the signed 64-bit wire range")
    return currents


class SweepFileReader:
    """Poll raw data and group it into fixed-size sweeps.

    Rereading this small file after a change makes truncation, archive rotation,
    app restarts, and partially appended sweeps straightforward.
    """

    def __init__(self, path: Path):
        self.path = path
        self._signature: tuple[int, int, int] | None = None

    def poll(self, force: bool = False) -> ReadBatch | None:
        before = self.path.stat()
        signature = (before.st_ino, before.st_size, before.st_mtime_ns)
        if not force and signature == self._signature:
            return None

        text = self.path.read_text(encoding="utf-8")
        after = self.path.stat()
        after_signature = (after.st_ino, after.st_size, after.st_mtime_ns)
        if signature != after_signature:
            raise ValueError("File changed during read; retrying")

        values = parse_raw_currents(text)
        complete_count = len(values) // SAMPLE_COUNT
        sweeps = tuple(
            Snapshot(values[index:index + SAMPLE_COUNT])
            for index in range(0, complete_count * SAMPLE_COUNT, SAMPLE_COUNT)
        )
        self._signature = signature
        return ReadBatch(sweeps, len(values) % SAMPLE_COUNT, after.st_size)

    def reset_tracking(self) -> None:
        self._signature = None

    def archive_and_clear(self) -> Path | None:
        """Move the current input into ./archive and create an empty source."""
        self.path.parent.mkdir(parents=True, exist_ok=True)
        archived = None
        if self.path.exists() and self.path.stat().st_size:
            archive_dir = self.path.parent / "archive"
            archive_dir.mkdir(parents=True, exist_ok=True)
            stamp = datetime.now().astimezone().strftime("%Y%m%d-%H%M%S-%f")
            archived = archive_dir / f"{self.path.stem}-{stamp}{self.path.suffix}"
            os.replace(self.path, archived)
        self.path.touch(exist_ok=True)
        self.reset_tracking()
        return archived
