# I–V curve viewer

A desktop viewer for accumulated tether sweeps. Every sweep contains 50
measured currents at positions 1, 3, …, 99 of the fixed 100-point voltage
sweep. Earlier sweeps are shown as pale curves; the selected sweep is blue,
its received samples are orange, and its final extrapolated segment is dashed.
Current is displayed in **µA**, with voltage in **V**.

To be used in conjunction with the ground station for comms software, to display
the payload data sent from the OBC.

## Setup and run

Requires Python 3.10 or later. From this directory:

```bash
python -m pip install -r requirements.txt
python app.py --file readings.txt
```

The viewer polls the input once per second. It watches the "readings.txt" file, and
automatically refreshes when the file identity, size, or modification time changes.
**Force update** rereads the file immediately.

## Raw input contract

The file contains only signed decimal integers, separated by whitespace and/or
commas:

```text
1350498366020
1306972041352
...
```

Each integer is a current in units of 10⁻¹⁷ A. Every consecutive group of 50
integers becomes one sweep; no headers or boundary markers are required. A
partial group is reported in the status bar until enough values arrive.

With no framing information, a missing or additional integer shifts the
boundaries of all later sweeps. This viewer assumes the transfer preserves all
values and their order.

## Simulate one ten-sweep transfer

Place the supplied CSV directory beside the Python files. These are already in the
repository, sourced from the Tether team's simulations.

```text
iv_viewer/
  voltage_sweep_points/
    voltage_sweep_point_00001_....csv
    voltage_sweep_point_00002_....csv
    ...
```

Then run:

```bash
python simulator.py --file readings.txt
```

The command runs once. It takes the first 10 CSV files in filename order,
reads `I_endpoint`, retains CSV rows 1, 3, …, 99, scales the values by 10¹⁷,
and appends 500 raw integers. Running it again appends another 10 sweeps.

For a fresh simulation file:

```bash
python simulator.py --file readings.txt --replace
```
`--replace` will start fresh from the first 10 files, and clean up the readings.txt file.

Use a different location or number of sweeps with `--source-dir` and `--count`.
The simulator validates all selected CSVs before opening the output, so a bad
CSV does not result in a partial batch.

## Viewer controls

- **Highlight** selects the sweep drawn strongly and shows its measured points.
  All other loaded sweeps remain visible in pale blue.
- **Clear view** removes curves from the window without changing the input.
  A later append reloads the file; Force update reloads it immediately.
- **Archive & clear file** moves a nonempty input into an `archive/` directory
  beside it and creates a fresh empty input. Use this while the ground-station
  writer is idle and closes the file between transfers.
- **Show detail near 0 V** opens the existing close-up graph.

## Code boundaries

- `reader.py`: raw parsing, change detection, fixed groups of 50, and archive
  rotation. Change `parse_raw_currents` if the source text format changes.
- `curves.py`: fixed voltages, integer scaling, interpolation, and final
  extrapolation.
- `app.py`: controls, notifications, sweep selection, and plotting.
- `simulator.py`: one-shot conversion of CSV curves into raw appended data.

Interpolation is for visualisation and cannot restore the 50 untransmitted
measurements exactly. The final 34 V value is extrapolated and is less reliable
than the interior estimates.
