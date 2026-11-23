"""Presentation layer: Qt controls and pyqtgraph rendering."""

import argparse
from pathlib import Path

import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from curves import AverageCurveResult, Curve, VOLTAGES, build_average_curve, build_curve
from reader import SweepFileReader

INK = "#293747"
TEAL = "#50bad1"
PALE_TEAL = (80, 186, 209, 78)
ORANGE = "#f2a93b"
PURPLE = "#7a5af8"
PALE_OUTLIER = (214, 105, 88, 105)
BACKGROUND = "#fcfbf9"


def make_plot(title: str, legend: bool = True) -> pg.PlotWidget:
    plot = pg.PlotWidget(background=BACKGROUND)
    plot.setTitle(title, color=INK, size="13pt")
    plot.setLabel("bottom", "Voltage input", units="V", color=INK)
    plot.setLabel("left", "Current", units="µA", color=INK)
    plot.showGrid(x=True, y=True, alpha=0.18)
    plot.getPlotItem().getViewBox().setMouseEnabled(x=True, y=True)
    plot.setMinimumHeight(250)
    if legend:
        plot.addLegend(offset=(15, 12))
    return plot


class MainWindow(QMainWindow):
    def __init__(self, path: Path):
        super().__init__()
        self.setWindowTitle("I–V Curve Viewer")
        self.resize(1080, 760)
        self.reader = SweepFileReader(path)
        self.curves: tuple[Curve, ...] = ()
        self.average_result = AverageCurveResult(None, (), (), ())
        self.last_source_count = 0
        self.main_items: list[object] = []
        self.detail_items: list[object] = []

        root = QWidget()
        self.setCentralWidget(root)
        layout = QVBoxLayout(root)
        layout.setContentsMargins(24, 20, 24, 16)
        layout.setSpacing(12)

        heading = QLabel("I–V curve monitor")
        heading.setObjectName("heading")
        layout.addWidget(heading)
        subtitle = QLabel("Raw sweep history · measured samples and linear reconstruction")
        subtitle.setObjectName("subtitle")
        layout.addWidget(subtitle)

        file_row = QHBoxLayout()
        self.path_label = QLabel(str(path.resolve()))
        self.path_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        file_row.addWidget(self.path_label, 1)
        choose = QPushButton("Choose file…")
        choose.clicked.connect(self.choose_file)
        file_row.addWidget(choose)
        layout.addLayout(file_row)

        actions = QHBoxLayout()
        force = QPushButton("Force update")
        force.clicked.connect(lambda: self.poll(force=True))
        actions.addWidget(force)
        clear = QPushButton("Clear view")
        clear.clicked.connect(self.clear_view)
        actions.addWidget(clear)
        archive = QPushButton("Archive && clear file")
        archive.clicked.connect(self.archive_and_clear)
        actions.addWidget(archive)
        actions.addSpacing(14)
        actions.addWidget(QLabel("Highlight:"))
        self.sweep_picker = QComboBox()
        self.sweep_picker.setMinimumWidth(120)
        self.sweep_picker.currentIndexChanged.connect(self.selection_changed)
        actions.addWidget(self.sweep_picker)
        self.follow_latest = QCheckBox("Follow newest")
        self.follow_latest.setChecked(True)
        self.follow_latest.toggled.connect(self.follow_latest_changed)
        actions.addWidget(self.follow_latest)
        self.average_toggle = QCheckBox("Show average")
        self.average_toggle.setChecked(True)
        self.average_toggle.toggled.connect(self.render_all)
        actions.addWidget(self.average_toggle)
        actions.addStretch(1)
        reset_view = QPushButton("Reset view")
        reset_view.clicked.connect(self.reset_view)
        actions.addWidget(reset_view)
        self.detail_toggle = QCheckBox("Show detail near 0 V")
        self.detail_toggle.toggled.connect(self.set_detail_visible)
        actions.addWidget(self.detail_toggle)
        layout.addLayout(actions)

        self.notice = QLabel("Waiting for raw sweep data")
        self.notice.setObjectName("notice")
        layout.addWidget(self.notice)

        self.main_plot = make_plot("Voltage input vs current")
        layout.addWidget(self.main_plot, 1)

        self.detail_plot = make_plot("Detail near the zero crossing", legend=False)
        self.detail_plot.setXRange(-2, 5, padding=0)
        self.detail_plot.setVisible(False)
        layout.addWidget(self.detail_plot, 1)

        self.status = QLabel("Polling once per second…")
        self.status.setObjectName("status")
        layout.addWidget(self.status)
        self.setStyleSheet("""
            QMainWindow, QWidget { background: #fcfbf9; color: #293747; }
            QLabel#heading { font-size: 24px; font-weight: 600; }
            QLabel#subtitle, QLabel#status { color: #68798a; }
            QLabel#notice { color: #277a62; background: #e9f7f1;
                            border-radius: 5px; padding: 7px 10px; }
            QPushButton, QComboBox { padding: 7px 12px; border: 1px solid #cbd5db;
                                    border-radius: 5px; background: white; }
            QPushButton:hover, QComboBox:hover { background: #eef8fa; }
        """)

        self.timer = QTimer(self)
        self.timer.timeout.connect(self.poll)
        self.timer.start(1000)
        self.poll(force=True)

    def choose_file(self) -> None:
        selected, _ = QFileDialog.getOpenFileName(
            self,
            "Select readings file",
            str(self.reader.path.parent),
            "Text files (*.txt);;All files (*)",
        )
        if selected:
            self.reader = SweepFileReader(Path(selected))
            self.path_label.setText(str(Path(selected).resolve()))
            self.curves = ()
            self.average_result = AverageCurveResult(None, (), (), ())
            self.last_source_count = 0
            self.poll(force=True)

    def set_detail_visible(self, visible: bool) -> None:
        self.detail_plot.setVisible(visible)

    def selection_changed(self, *_args) -> None:
        if self.sweep_picker.currentIndex() >= 0:
            self.follow_latest.blockSignals(True)
            self.follow_latest.setChecked(False)
            self.follow_latest.blockSignals(False)
        self.render_all()

    def follow_latest_changed(self, enabled: bool) -> None:
        if enabled and self.curves:
            self._select_latest()
            self.render_all()

    def clear_view(self) -> None:
        self.curves = ()
        self.average_result = AverageCurveResult(None, (), (), ())
        self._refresh_picker()
        self.render_all()
        self.notice.setText("View cleared · input file left unchanged")
        self.status.setText("Use Force update to reload the existing file")

    def archive_and_clear(self) -> None:
        answer = QMessageBox.question(
            self,
            "Archive and clear input",
            "Archive the current readings file and start a new empty file?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel,
            QMessageBox.StandardButton.Cancel,
        )
        if answer != QMessageBox.StandardButton.Yes:
            return
        try:
            archived = self.reader.archive_and_clear()
            self.curves = ()
            self.average_result = AverageCurveResult(None, (), (), ())
            self.last_source_count = 0
            self._refresh_picker()
            self.render_all()
            if archived:
                self.notice.setText(f"Archived as {archived.name}")
            else:
                self.notice.setText("Input file was already empty")
            self.status.setText("0 complete sweeps · waiting for data")
        except OSError as exc:
            self.notice.setText("Could not archive the input file")
            self.status.setText(str(exc))

    def poll(self, force: bool = False) -> None:
        previous_count = self.last_source_count
        try:
            batch = self.reader.poll(force=force)
            if batch is None:
                return
            self.curves = tuple(build_curve(snapshot) for snapshot in batch.sweeps)
            self.last_source_count = len(self.curves)
            self.average_result = build_average_curve(self.curves)
            count = len(self.curves)
            new_data_arrived = count > previous_count
            self._refresh_picker(
                select_latest=new_data_arrived and self.follow_latest.isChecked()
            )
            self.render_all()

            if count > previous_count:
                received = count - previous_count
                if received == 1:
                    self.notice.setText(f"Data received · Sweep {count}")
                else:
                    self.notice.setText(f"Data received · {received} new sweeps")
            elif force:
                self.notice.setText("Forced update complete")
            elif count < previous_count:
                self.notice.setText("Input file was cleared or replaced")

            pending = ""
            if batch.pending_values:
                needed = 50 - batch.pending_values
                pending = f" · {batch.pending_values}/50 values received for next sweep ({needed} needed)"
            average = ""
            if count:
                excluded = len(self.average_result.excluded_indices)
                average = f" · average uses {count - excluded}/{count} sweeps"
                if excluded:
                    average += f" ({excluded} outlier{'s' if excluded != 1 else ''} excluded)"
            self.status.setText(
                f"{count} complete sweep{'s' if count != 1 else ''} · "
                f"{batch.source_size:,} bytes{average}{pending}"
            )
        except FileNotFoundError:
            self.status.setText("Input file does not exist yet · waiting for data")
        except (OSError, UnicodeError, ValueError) as exc:
            self.notice.setText("New data could not be loaded · showing last valid curves")
            self.status.setText(str(exc))

    def _refresh_picker(self, select_latest: bool = False) -> None:
        old_selection = self.sweep_picker.currentData()
        excluded = set(self.average_result.excluded_indices)
        self.sweep_picker.blockSignals(True)
        self.sweep_picker.clear()
        for index in range(len(self.curves)):
            suffix = " · excluded from average" if index in excluded else ""
            self.sweep_picker.addItem(f"Sweep {index + 1}{suffix}", index)
        if self.curves:
            if select_latest:
                desired = len(self.curves) - 1
            else:
                desired = old_selection if isinstance(old_selection, int) else len(self.curves) - 1
            desired = min(desired, len(self.curves) - 1)
            self.sweep_picker.setCurrentIndex(desired)
        self.sweep_picker.setEnabled(bool(self.curves))
        self.sweep_picker.blockSignals(False)

    def _select_latest(self) -> None:
        if not self.curves:
            return
        self.sweep_picker.blockSignals(True)
        self.sweep_picker.setCurrentIndex(len(self.curves) - 1)
        self.sweep_picker.blockSignals(False)

    def _clear_plot_items(self) -> None:
        for item in self.main_items:
            self.main_plot.removeItem(item)
        for item in self.detail_items:
            self.detail_plot.removeItem(item)
        self.main_items.clear()
        self.detail_items.clear()
        legend = self.main_plot.getPlotItem().legend
        if legend is not None:
            legend.clear()

    def render_all(self, *_args) -> None:
        self._clear_plot_items()
        if not self.curves:
            return
        selected = self.sweep_picker.currentData()
        if not isinstance(selected, int):
            selected = len(self.curves) - 1

        excluded = set(self.average_result.excluded_indices)
        other_label_used = False
        outlier_label_used = False
        for index, curve in enumerate(self.curves):
            if index == selected:
                continue
            if index in excluded:
                name = "Excluded outlier" if not outlier_label_used else None
                outlier_label_used = True
                main_pen = pg.mkPen(
                    PALE_OUTLIER, width=1.5, style=Qt.PenStyle.DashLine
                )
                detail_pen = pg.mkPen(
                    PALE_OUTLIER, width=1.3, style=Qt.PenStyle.DashLine
                )
            else:
                name = "Other sweeps" if not other_label_used else None
                other_label_used = True
                main_pen = pg.mkPen(PALE_TEAL, width=1.4)
                detail_pen = pg.mkPen(PALE_TEAL, width=1.2)
            main = self.main_plot.plot(
                curve.voltage, curve.current_ua,
                pen=main_pen,
                name=name,
            )
            detail = self.detail_plot.plot(
                curve.voltage, curve.current_ua,
                pen=detail_pen,
            )
            self.main_items.append(main)
            self.detail_items.append(detail)

        average_curve = self.average_result.curve
        if self.average_toggle.isChecked() and average_curve is not None:
            included = len(self.average_result.included_indices)
            average_line = self.main_plot.plot(
                average_curve.voltage[:-1], average_curve.current_ua[:-1],
                pen=pg.mkPen(PURPLE, width=3.0),
                name=f"Average ({included}/{len(self.curves)})",
            )
            average_tip = self.main_plot.plot(
                average_curve.voltage[-2:], average_curve.current_ua[-2:],
                pen=pg.mkPen(PURPLE, width=3.0, style=Qt.PenStyle.DashLine),
            )
            average_detail = self.detail_plot.plot(
                average_curve.voltage, average_curve.current_ua,
                pen=pg.mkPen(PURPLE, width=3.0),
            )
            self.main_items.extend((average_line, average_tip))
            self.detail_items.append(average_detail)

        curve = self.curves[selected]
        line = self.main_plot.plot(
            curve.voltage[:-1], curve.current_ua[:-1],
            pen=pg.mkPen(TEAL, width=2.6),
            name=f"Sweep {selected + 1}",
        )
        tip = self.main_plot.plot(
            curve.voltage[-2:], curve.current_ua[-2:],
            pen=pg.mkPen(TEAL, width=2.6, style=Qt.PenStyle.DashLine),
            name="Extrapolated end",
        )
        points = self.main_plot.plot(
            curve.measured_voltage, curve.measured_current_ua,
            pen=None,
            symbol="o",
            symbolSize=7,
            symbolBrush=ORANGE,
            symbolPen=pg.mkPen("#ffffff", width=1),
            name="Measured samples",
        )
        self.main_items.extend((line, tip, points))

        detail_line = self.detail_plot.plot(
            curve.voltage, curve.current_ua, pen=pg.mkPen(TEAL, width=2.6)
        )
        detail_points = self.detail_plot.plot(
            curve.measured_voltage, curve.measured_current_ua,
            pen=None, symbol="o", symbolSize=7,
            symbolBrush=ORANGE, symbolPen=pg.mkPen("#ffffff", width=1),
        )
        self.detail_items.extend((detail_line, detail_points))
        local = [y for x, y in zip(curve.voltage, curve.current_ua) if -2 <= x <= 5]
        if local:
            low, high = min(local), max(local)
            margin = max((high - low) * 0.1, 1.0)
            self.detail_plot.setYRange(low - margin, high + margin, padding=0)

    def reset_view(self) -> None:
        """Restore the full sweep and detail view after zooming or panning."""
        main_values = [
            current
            for curve in self.curves
            for current in curve.current_ua
        ]
        average_curve = self.average_result.curve
        if self.average_toggle.isChecked() and average_curve is not None:
            main_values.extend(average_curve.current_ua)
        self._set_plot_range(
            self.main_plot, VOLTAGES[0], VOLTAGES[-1], main_values, x_padding=0.02
        )

        detail_values = [
            current
            for curve in self.curves
            for voltage, current in zip(curve.voltage, curve.current_ua)
            if -2 <= voltage <= 5
        ]
        if self.average_toggle.isChecked() and average_curve is not None:
            detail_values.extend(
                current
                for voltage, current in zip(
                    average_curve.voltage, average_curve.current_ua
                )
                if -2 <= voltage <= 5
            )
        self._set_plot_range(
            self.detail_plot, -2, 5, detail_values, x_padding=0
        )

    @staticmethod
    def _set_plot_range(
        plot: pg.PlotWidget,
        x_min: float,
        x_max: float,
        y_values: list[float],
        x_padding: float,
    ) -> None:
        plot.setXRange(x_min, x_max, padding=x_padding)
        if not y_values:
            plot.setYRange(-1, 1, padding=0)
            return
        low, high = min(y_values), max(y_values)
        span = high - low
        margin = max(span * 0.08, abs(low) * 0.02, abs(high) * 0.02, 0.05)
        plot.setYRange(low - margin, high + margin, padding=0)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Watch raw current data and display accumulated I–V sweeps"
    )
    parser.add_argument("--file", type=Path, default=Path("readings.txt"))
    args = parser.parse_args()
    app = QApplication([])
    window = MainWindow(args.file)
    window.show()
    raise SystemExit(app.exec())


if __name__ == "__main__":
    main()
