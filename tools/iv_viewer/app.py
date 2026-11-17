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

from curves import Curve, build_curve
from reader import SweepFileReader

INK = "#293747"
TEAL = "#50bad1"
PALE_TEAL = (80, 186, 209, 78)
ORANGE = "#f2a93b"
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
        self.sweep_picker.currentIndexChanged.connect(self.render_all)
        actions.addWidget(self.sweep_picker)
        actions.addStretch(1)
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
            self.last_source_count = 0
            self.poll(force=True)

    def set_detail_visible(self, visible: bool) -> None:
        self.detail_plot.setVisible(visible)

    def clear_view(self) -> None:
        self.curves = ()
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
            self._refresh_picker()
            self.render_all()

            count = len(self.curves)
            if count > previous_count:
                received = count - previous_count
                self.notice.setText(
                    f"Data received · {received} new sweep{'s' if received != 1 else ''}"
                )
            elif force:
                self.notice.setText("Forced update complete")
            elif count < previous_count:
                self.notice.setText("Input file was cleared or replaced")

            pending = ""
            if batch.pending_values:
                needed = 50 - batch.pending_values
                pending = f" · {batch.pending_values}/50 values received for next sweep ({needed} needed)"
            self.status.setText(
                f"{count} complete sweep{'s' if count != 1 else ''} · "
                f"{batch.source_size:,} bytes{pending}"
            )
        except FileNotFoundError:
            self.status.setText("Input file does not exist yet · waiting for data")
        except (OSError, UnicodeError, ValueError) as exc:
            self.notice.setText("New data could not be loaded · showing last valid curves")
            self.status.setText(str(exc))

    def _refresh_picker(self) -> None:
        old_selection = self.sweep_picker.currentData()
        self.sweep_picker.blockSignals(True)
        self.sweep_picker.clear()
        for index in range(len(self.curves)):
            self.sweep_picker.addItem(f"Sweep {index + 1}", index)
        if self.curves:
            desired = old_selection if isinstance(old_selection, int) else len(self.curves) - 1
            desired = min(desired, len(self.curves) - 1)
            self.sweep_picker.setCurrentIndex(desired)
        self.sweep_picker.setEnabled(bool(self.curves))
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

        other_label_used = False
        for index, curve in enumerate(self.curves):
            if index == selected:
                continue
            name = "Other sweeps" if not other_label_used else None
            other_label_used = True
            main = self.main_plot.plot(
                curve.voltage, curve.current_ua,
                pen=pg.mkPen(PALE_TEAL, width=1.4),
                name=name,
            )
            detail = self.detail_plot.plot(
                curve.voltage, curve.current_ua,
                pen=pg.mkPen(PALE_TEAL, width=1.2),
            )
            self.main_items.append(main)
            self.detail_items.append(detail)

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
