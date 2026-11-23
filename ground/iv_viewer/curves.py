"""Pure computation layer; no file access or GUI dependencies."""

from dataclasses import dataclass
from math import sqrt
from statistics import median
from reader import SCALE, SAMPLE_COUNT, Snapshot

OUTLIER_MIN_SWEEPS = 4
OUTLIER_Z_THRESHOLD = 3.5
OUTLIER_MIN_RELATIVE_DISTANCE = 0.05

# Voltage positions supplied for this sweep. Keep them here until metadata
# from the sender replaces the fixed sweep definition.
VOLTAGES = tuple(float(v) for v in """
-30.0 -29.353535353535400 -28.707070707070700 -28.060606060606100
-27.414141414141400 -26.767676767676800 -26.12121212121210 -25.474747474747500
-24.828282828282800 -24.18181818181820 -23.535353535353500 -22.88888888888890
-22.242424242424200 -21.595959595959600 -20.949494949494900 -20.303030303030300
-19.656565656565700 -19.01010101010100 -18.363636363636400 -17.717171717171700
-17.07070707070710 -16.424242424242400 -15.777777777777800 -15.13131313131310
-14.484848484848500 -13.838383838383800 -13.19191919191920 -12.545454545454500
-11.898989898989900 -11.252525252525300 -10.606060606060600 -9.959595959595960
-9.313131313131310 -8.666666666666660 -8.020202020202020 -7.37373737373737
-6.727272727272730 -6.0808080808080800 -5.4343434343434300 -4.787878787878790
-4.141414141414140 -3.4949494949494900 -2.8484848484848500 -2.2020202020202000
-1.5555555555555500 -0.9090909090909070 -0.26262626262625900 0.38383838383838800
1.0303030303030300 1.6767676767676800 2.3232323232323300 2.969696969696980
3.6161616161616200 4.262626262626260 4.909090909090910 5.555555555555560
6.202020202020210 6.848484848484850 7.494949494949500 8.141414141414150
8.787878787878790 9.43434343434344 10.080808080808100 10.727272727272700
11.373737373737400 12.02020202020200 12.666666666666700 13.313131313131300
13.959595959596000 14.606060606060600 15.25252525252530 15.898989898989900
16.545454545454500 17.191919191919200 17.83838383838380 18.48484848484850
19.131313131313100 19.77777777777780 20.42424242424240 21.070707070707100
21.717171717171700 22.363636363636400 23.010101010101000 23.65656565656570
24.303030303030300 24.949494949495000 25.5959595959596 26.24242424242430
26.888888888888900 27.535353535353500 28.181818181818200 28.82828282828280
29.47474747474750 30.121212121212100 30.767676767676800 31.41414141414140
32.06060606060610 32.70707070707070 33.35353535353540 34.0
""".split())
assert len(VOLTAGES) == 100


@dataclass(frozen=True)
class Curve:
    measured_voltage: tuple[float, ...]
    measured_current_ua: tuple[float, ...]
    voltage: tuple[float, ...]
    current_ua: tuple[float, ...]


@dataclass(frozen=True)
class AverageCurveResult:
    curve: Curve | None
    included_indices: tuple[int, ...]
    excluded_indices: tuple[int, ...]
    outlier_scores: tuple[float, ...]


def reconstruct_curve(measured_current_ua: tuple[float, ...]) -> Curve:
    """Fill the 100 voltage positions from 50 measured current values."""
    if len(measured_current_ua) != SAMPLE_COUNT:
        raise ValueError(f"Expected {SAMPLE_COUNT} measured currents")
    reconstructed = [0.0] * len(VOLTAGES)
    for k, current in enumerate(measured_current_ua):
        reconstructed[2 * k] = current
    for k in range(SAMPLE_COUNT - 1):
        left, right = 2 * k, 2 * k + 2
        ratio = (VOLTAGES[left + 1] - VOLTAGES[left]) / (VOLTAGES[right] - VOLTAGES[left])
        reconstructed[left + 1] = (
            measured_current_ua[k]
            + ratio * (measured_current_ua[k + 1] - measured_current_ua[k])
        )
    # Last position lies outside the samples: linear extrapolation, not interpolation.
    ratio = (VOLTAGES[99] - VOLTAGES[98]) / (VOLTAGES[98] - VOLTAGES[96])
    reconstructed[99] = measured_current_ua[-1] + ratio * (
        measured_current_ua[-1] - measured_current_ua[-2]
    )
    return Curve(VOLTAGES[::2], measured_current_ua, VOLTAGES, tuple(reconstructed))


def build_curve(snapshot: Snapshot) -> Curve:
    if len(snapshot.raw_currents) != SAMPLE_COUNT:
        raise ValueError(f"Expected {SAMPLE_COUNT} measured currents")
    # Positions 1, 3, ... 99 in one-based numbering, i.e. indices 0, 2, ... 98.
    measured = tuple(value / SCALE * 1_000_000 for value in snapshot.raw_currents)
    return reconstruct_curve(measured)


def build_average_curve(curves: tuple[Curve, ...]) -> AverageCurveResult:
    """Build a pointwise average after rejecting whole-curve outliers.

    A pointwise median curve provides the robust reference. Each sweep receives
    a normalised RMS distance from that reference, and distances are classified
    using a median/MAD threshold. A 5% floor prevents small numerical differences
    from being labelled as outliers. Fewer than four sweeps are all retained.
    """
    if not curves:
        return AverageCurveResult(None, (), (), ())

    measured = tuple(curve.measured_current_ua for curve in curves)
    reference = tuple(
        median(values[index] for values in measured)
        for index in range(SAMPLE_COUNT)
    )
    reference_range = max(reference) - min(reference)
    reference_magnitude = max(abs(value) for value in reference)
    normaliser = max(reference_range, reference_magnitude, 1e-12)
    scores = tuple(
        sqrt(
            sum((value - reference[index]) ** 2 for index, value in enumerate(values))
            / SAMPLE_COUNT
        ) / normaliser
        for values in measured
    )

    excluded: tuple[int, ...] = ()
    if len(curves) >= OUTLIER_MIN_SWEEPS:
        centre = median(scores)
        mad = median(abs(score - centre) for score in scores)
        robust_sigma = 1.4826 * mad
        threshold = max(
            centre + OUTLIER_Z_THRESHOLD * robust_sigma,
            OUTLIER_MIN_RELATIVE_DISTANCE,
        )
        excluded = tuple(index for index, score in enumerate(scores) if score > threshold)

    excluded_set = set(excluded)
    included = tuple(index for index in range(len(curves)) if index not in excluded_set)
    # The median-based threshold should always retain at least half the sweeps,
    # but retain everything if future threshold changes ever violate that rule.
    if not included:
        included = tuple(range(len(curves)))
        excluded = ()

    averaged = tuple(
        sum(measured[curve_index][point_index] for curve_index in included) / len(included)
        for point_index in range(SAMPLE_COUNT)
    )
    return AverageCurveResult(reconstruct_curve(averaged), included, excluded, scores)
