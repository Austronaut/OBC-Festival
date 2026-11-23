"""Append CSV-derived sweeps to the raw ground-station text file once."""

import argparse
import csv
from decimal import Decimal, InvalidOperation, ROUND_HALF_EVEN
from pathlib import Path

from reader import SAMPLE_COUNT, SCALE, parse_raw_currents


def extract_currents(csv_path: Path) -> tuple[int, ...]:
    """Read I_endpoint and retain CSV rows 1, 3, ..., 99."""
    with csv_path.open(newline="", encoding="utf-8-sig") as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames or "I_endpoint" not in reader.fieldnames:
            raise ValueError(f"{csv_path.name}: missing I_endpoint column")
        rows = list(reader)
    if len(rows) < SAMPLE_COUNT * 2 - 1:
        raise ValueError(f"{csv_path.name}: expected at least 99 data rows, found {len(rows)}")

    currents = []
    for sample_number, row in enumerate(rows[0:SAMPLE_COUNT * 2:2], start=1):
        try:
            amperes = Decimal(row["I_endpoint"])
        except (InvalidOperation, TypeError) as exc:
            raise ValueError(
                f"{csv_path.name}: invalid I_endpoint near sample {sample_number}"
            ) from exc
        scaled = int((amperes * SCALE).to_integral_value(rounding=ROUND_HALF_EVEN))
        if not -(2**63) <= scaled < 2**63:
            raise ValueError(f"{csv_path.name}: scaled current exceeds signed 64-bit range")
        currents.append(scaled)
    return tuple(currents)


def append_sweeps(output: Path, csv_files: list[Path], replace: bool) -> None:
    sweeps = [extract_currents(path) for path in csv_files]
    output.parent.mkdir(parents=True, exist_ok=True)
    needs_separator = not replace and output.exists() and output.stat().st_size > 0
    with output.open("w" if replace else "a", encoding="utf-8", newline="\n") as destination:
        if needs_separator:
            destination.write("\n")
        for sweep in sweeps:
            destination.write("\n".join(map(str, sweep)))
            destination.write("\n")


def find_default_source() -> Path:
    underscored = Path("voltage_sweep_points")
    spaced = Path("voltage sweep points")
    return underscored if underscored.exists() or not spaced.exists() else spaced


def existing_sweep_count(output: Path) -> int:
    """Count complete sweeps and refuse to append after an incomplete one."""
    if not output.exists() or output.stat().st_size == 0:
        return 0
    values = parse_raw_currents(output.read_text(encoding="utf-8"))
    remainder = len(values) % SAMPLE_COUNT
    if remainder:
        raise ValueError(
            f"{output} ends with an incomplete sweep ({remainder}/{SAMPLE_COUNT} values); "
            "complete or clear it before simulating another transfer"
        )
    return len(values) // SAMPLE_COUNT


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Append CSV-derived I–V sweeps to a raw readings file once"
    )
    parser.add_argument("--file", type=Path, default=Path("readings.txt"))
    parser.add_argument("--source-dir", type=Path, default=find_default_source())
    parser.add_argument(
        "--count", type=int, default=1,
        help="number of sequential CSV sweeps to append (default: 1)",
    )
    parser.add_argument(
        "--start", type=int,
        help="1-based CSV position to start from; default continues after existing sweeps",
    )
    parser.add_argument(
        "--replace", action="store_true",
        help="replace the readings file instead of appending to it",
    )
    args = parser.parse_args()
    if args.count < 1:
        parser.error("--count must be at least 1")
    if args.start is not None and args.start < 1:
        parser.error("--start must be at least 1")

    try:
        existing_count = 0 if args.replace else existing_sweep_count(args.file)
    except (OSError, UnicodeError, ValueError) as exc:
        parser.error(str(exc))
    offset = args.start - 1 if args.start is not None else existing_count

    all_csv_files = sorted(args.source_dir.glob("*.csv"))
    csv_files = all_csv_files[offset:offset + args.count]
    if len(csv_files) < args.count:
        parser.error(
            f"need CSV positions {offset + 1}–{offset + args.count}, but "
            f"{args.source_dir} contains {len(all_csv_files)} files"
        )
    append_sweeps(args.file, csv_files, args.replace)
    action = "Wrote" if args.replace else "Appended"
    print(
        f"{action} {len(csv_files)} sweeps "
        f"({len(csv_files) * SAMPLE_COUNT} currents) to {args.file}"
    )
    for path in csv_files:
        print(f"  {path.name}")


if __name__ == "__main__":
    main()
