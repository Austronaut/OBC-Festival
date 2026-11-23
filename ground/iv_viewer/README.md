# I–V curve viewer

A desktop viewer for accumulated tether sweeps. Every sweep contains 50
measured currents at positions 1, 3, …, 99 of the fixed 100-point voltage
sweep. Earlier sweeps are shown as pale curves; the selected sweep is blue,
its received samples are orange, and its final extrapolated segment is dashed.
Current is displayed in **µA**, with voltage in **V**.

The viewer is intended to be used alongside the ground-station communications
software to display tether payload data received from the OBC.

## Setup and run

Requires Python 3.10 or later. From this directory:

```bash
python -m pip install -r requirements.txt
python app.py --file readings.txt
```

The viewer polls the input once per second. It automatically refreshes when
the file identity, size, or modification time changes. **Force update** rereads the file immediately.

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

## Simulate sweep arrivals

Place the supplied CSV directory beside the Python files:

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

The command runs once. On a fresh file it takes the first CSV in filename
order, reads `I_endpoint`, retains CSV rows 1, 3, …, 99, scales the values by
10¹⁷, and appends 50 raw integers. Running it again sees one complete sweep
already present and takes the second CSV; subsequent runs continue likewise.

To append a batch explicitly:

```bash
python simulator.py --file readings.txt --count 10
```

For a fresh simulation file:

```bash
python simulator.py --file readings.txt --replace
```

Use a different location or number of sweeps with `--source-dir` and `--count`.
Use `--start 31` to explicitly begin at the 31st CSV. The simulator validates
all selected CSVs before opening the output, so a bad CSV does not result in a
partial batch. It also refuses to append when the existing raw file ends with
an incomplete 50-value sweep.

## Viewer controls

- **Highlight** selects the sweep drawn strongly and shows its measured points.
  All other loaded sweeps remain visible in pale blue.
- **Follow newest** automatically highlights each newly completed sweep. Manually
  selecting an older sweep disables it until it is checked again.
- **Show average** displays a robust average of the measured sweeps. Whole-sweep
  outliers are detected against the median curve and remain visible as dashed
  coral lines, but are excluded from the average. At least four sweeps are
  required before automatic outlier rejection is enabled.
- **Reset view** restores the full −30 V to 34 V graph and resets the detail view
  after zooming or panning.
- **Clear view** removes curves from the window without changing the input.
  A later append reloads the file; Force update reloads it immediately.
- **Archive & clear file** moves a nonempty input into an `archive/` directory
  beside it and creates a fresh empty input. Use this while the ground-station
  writer is idle and closes the file between transfers.
- **Show detail near 0 V** opens the existing close-up graph.

## Code boundaries

- `reader.py`: raw parsing, change detection, fixed groups of 50, and archive
  rotation. Change `parse_raw_currents` if the source text format changes.
- `curves.py`: fixed voltages, integer scaling, interpolation, final
  extrapolation, robust outlier detection, and averaging.
- `app.py`: controls, notifications, sweep selection, and plotting.
- `simulator.py`: one-shot conversion of CSV curves into raw appended data.

Interpolation is for visualisation and cannot restore the 50 untransmitted
measurements exactly. The final 34 V value is extrapolated and is less reliable
than the interior estimates.
