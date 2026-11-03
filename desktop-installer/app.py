# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Native desktop GUI. Preview mode has no serial enumeration or access."""
from __future__ import annotations

import argparse
import contextlib
import io
import sys
from pathlib import Path

from PySide6.QtCore import QObject, QRunnable, QSize, Qt, QThreadPool, QTimer, Signal
from PySide6.QtGui import QIcon
from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QFrame, QHBoxLayout,
                               QLabel, QMainWindow, QMessageBox, QProgressBar,
                               QPushButton, QScrollArea, QLayout, QSizePolicy, QStyle,
                               QStyleOptionButton, QStylePainter, QVBoxLayout, QWidget)
import core


class Signals(QObject):
    done = Signal(object, str)


class Task(QRunnable):
    def __init__(self, function):
        super().__init__()
        self.function = function
        self.signals = Signals()

    def run(self):
        result, message = None, ""
        try:
            result = self.function()
        except BaseException as error:
            # A CLI library may raise SystemExit. It must not strand the GUI busy
            # or masquerade as success; this boundary belongs only to the worker.
            message = str(error)[:600] or type(error).__name__
        finally:
            self.signals.done.emit(result, message)


PRIMARY = "#b84a15"
PRIMARY_HOVER = "#a33f10"
PORT_PLACEHOLDER = "Select USB port"
STYLE = """
QWidget { background: #f6f5f1; color: #202521; font-size: 14px; }
QFrame#card { background: #ffffff; border: 1px solid #dedfd9; border-radius: 14px; }
QFrame#card QLabel, QFrame#card QCheckBox { background: transparent; }
QLabel#eyebrow { color: #666d65; font-size: 11px; font-weight: 600; }
QLabel#title { font-size: 31px; font-weight: 650; }
QLabel#step { font-size: 19px; font-weight: 600; }
QLabel#muted { color: #636b64; }
QLabel#status { color: #5b625a; padding: 12px; background: #edece6; border-radius: 8px; }
QLabel#error { color: #913519; padding: 12px; background: #fff0e8; border-radius: 8px; }
QPushButton { background: #ffffff; border: 1px solid #d2d5cc; border-radius: 8px;
              padding: 10px 16px; font-weight: 550; }
QPushButton:hover { background: #eeeFE8; }
QPushButton#primary { background: PRIMARY_COLOR; color: #ffffff; border: 1px solid PRIMARY_COLOR; }
QPushButton#primary:hover { background: PRIMARY_HOVER_COLOR; }
QPushButton#primary:disabled { background: #e7e8e2; color: #868b83; border-color: #e7e8e2; }
QPushButton:disabled { background: #e7e8e2; color: #868b83; border-color: #e7e8e2; }
QComboBox { background: #ffffff; padding: 9px; border: 1px solid #d2d5cc; border-radius: 6px; }
QComboBox { padding-right: 30px; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right;
                      width: 25px; border: none; background: transparent; }
QComboBox::down-arrow { width: 12px; height: 8px; }
QPushButton:focus, QPushButton#primary:focus, QComboBox:focus { border: 2px solid #ed7d32; }
QCheckBox:focus { outline: none; border: 2px solid #ed7d32; border-radius: 4px; }
QCheckBox { border: 2px solid transparent; }
QScrollArea { border: none; }
QCheckBox { spacing: 10px; padding: 5px 0px; }
QCheckBox::indicator { width: 18px; height: 18px; border: 1px solid #8a9187;
                      background: #ffffff; border-radius: 3px; }
QCheckBox::indicator:checked { background: #e86c26; border-color: #a34613; }
QProgressBar { border: none; background: #e9ebe4; border-radius: 4px; height: 8px; }
QProgressBar::chunk { background: #e86c26; border-radius: 4px; }
""".replace("PRIMARY_HOVER_COLOR", PRIMARY_HOVER).replace("PRIMARY_COLOR", PRIMARY)

PALETTES = {
    "light": {"background": "#edece7", "card": "#f8f7f3", "text": "#202521",
              "muted": "#596157", "border": "#cdd0c5", "status": "#e0e2d7",
              "disabled": "#d9ddd2", "disabled_text": "#64705e", "error": "#f9e5d9",
              "error_text": "#873214", "hover": "#e2e5d9", "focus": PRIMARY,
              "primary_focus": "#202521"},
    "dark": {"background": "#20251f", "card": "#2b3229", "text": "#edf0e6",
             "muted": "#b2bdac", "border": "#465141", "status": "#343d30",
             "disabled": "#384231", "disabled_text": "#a3ae9b", "error": "#463026",
             "error_text": "#ffbb95", "hover": "#394332", "focus": "#ed7d32",
             "primary_focus": "#ed7d32"},
}


def theme_css(mode, assets):
    p = PALETTES[mode]
    check = (assets / "check.svg").as_posix()
    arrow = (assets / f"arrow-{mode}.svg").as_posix()
    # Use semantic overrides so state colors do not accidentally follow background substitutions.
    return STYLE + f"""
    QWidget {{ background: {p['background']}; color: {p['text']}; }}
    QFrame#card {{ background: {p['card']}; border-color: {p['border']}; }}
    QLabel#eyebrow, QLabel#muted {{ color: {p['muted']}; }}
    QLabel#status {{ color: {p['text']}; background: {p['status']}; }}
    QLabel#error {{ color: {p['error_text']}; background: {p['error']}; }}
    QPushButton, QComboBox {{ background: {p['card']}; border-color: {p['border']}; }}
    QPushButton:hover {{ background: {p['hover']}; }}
    QPushButton:disabled, QPushButton#primary:disabled {{ background: {p['disabled']};
        color: {p['disabled_text']}; border-color: {p['disabled']}; }}
    QCheckBox::indicator {{ background: {p['card']}; border-color: {p['muted']}; }}
    QCheckBox::indicator:checked {{ background: #ed7d32; border-color: #ed7d32; }}
    QCheckBox::indicator:checked {{ image: url("{check}"); }}
    QComboBox::down-arrow {{ image: url("{arrow}"); }}
    QComboBox QAbstractItemView {{ background: {p['card']}; color: {p['text']};
        selection-background-color: {p['status']}; selection-color: {p['text']}; }}
    QPushButton:focus, QPushButton#primary:focus, QComboBox:focus,
        QCheckBox:focus {{ border-color: {p['focus']}; }}
    QPushButton#primary:focus {{ border-color: {p['primary_focus']}; }}
    QScrollBar:vertical {{ background: {p['background']}; width: 12px; border: none; }}
    QScrollBar::handle:vertical {{ background: {p['border']}; min-height: 32px;
        border: 3px solid {p['background']}; border-radius: 5px; }}
    QScrollBar::handle:vertical:hover {{ background: {p['muted']}; }}
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0px; }}
    QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {{ background: transparent; }}
    QProgressBar {{ background: {p['disabled']}; }}
    """


class WrappingCheckBox(QCheckBox):
    """Keep native checkbox behavior while allowing its full confirmation to wrap."""
    def __init__(self, text):
        super().__init__(text)
        self.caption = QLabel(text, self)
        self.caption.setWordWrap(True)
        self.caption.setTextFormat(Qt.TextFormat.PlainText)
        # The entire caption remains the checkbox's click target, not a second control.
        self.caption.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        policy = QSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        policy.setHeightForWidth(True)
        self.setSizePolicy(policy)

    def option(self):
        option = QStyleOptionButton()
        self.initStyleOption(option)
        return option

    def content_size(self, size):
        return self.style().sizeFromContents(QStyle.ContentsType.CT_CheckBox,
                                             self.option(), size, self)

    def setText(self, text):
        super().setText(text)
        if hasattr(self, "caption"):
            self.caption.setText(text)
            self.updateGeometry()

    def sizeHint(self):
        return self.content_size(self.caption.sizeHint())

    def minimumSizeHint(self):
        return self.content_size(self.caption.minimumSizeHint())

    def heightForWidth(self, width):
        overhead = self.content_size(QSize(0, 0))
        return self.content_size(QSize(0, self.caption.heightForWidth(
            max(1, width - overhead.width())))).height()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self.caption.setGeometry(self.style().subElementRect(
            QStyle.SubElement.SE_CheckBoxContents, self.option(), self))

    def paintEvent(self, event):
        option = self.option()
        option.text = ""  # The child label paints the same text, with native font metrics.
        painter = QStylePainter(self)
        painter.drawControl(QStyle.ControlElement.CE_CheckBox, option)

    def hitButton(self, position):
        return self.rect().contains(position)


class Window(QMainWindow):
    def __init__(self, resource_root: Path, preview=False):
        super().__init__()
        self.resources = resource_root
        key = resource_root / "desktop-installer/publisher.pem"
        self.public_key = key.read_bytes() if key.is_file() and not preview else b""
        self.session = core.Session(enabled=bool(self.public_key))
        self.release = None
        self.working = False
        self.tasks = []
        self.setWindowTitle("Display Installer")
        self.setWindowIcon(QIcon(str(resource_root / "desktop-installer/assets/app-icon.png")))
        self.setMinimumSize(730, 560)
        available = QApplication.primaryScreen().availableGeometry()
        self.resize(750, min(900, max(560, available.height() - 60)))
        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setCentralWidget(self.scroll)
        root = QWidget()
        self.scroll.setWidget(root)
        layout = QVBoxLayout(root)
        layout.setSizeConstraint(QLayout.SizeConstraint.SetMinimumSize)
        layout.setContentsMargins(32, 26, 32, 20)
        layout.setSpacing(15)
        heading = QHBoxLayout()
        heading.addWidget(self.label("PRUSA REMOTE DISPLAY · DESKTOP INSTALLER", "eyebrow"), 1)
        self.theme = QComboBox()
        self.theme.setAccessibleName("Application appearance")
        self.theme.addItem("System", "system")
        self.theme.addItem("Dark", "dark")
        self.theme.addItem("Light", "light")
        self.theme.currentIndexChanged.connect(self.apply_theme)
        heading.addWidget(self.theme)
        layout.addLayout(heading)
        layout.addWidget(self.label("Install your display", "title"))
        layout.addWidget(self.label("Connect by USB. Install the official firmware.\n"
                                    "No browser or command line needed.", "muted"))
        firmware = self.card(layout)
        line = QHBoxLayout()
        line.addWidget(self.label("1   Official firmware", "step"), 1)
        self.check = QPushButton("Check release")
        self.check.setAccessibleName("Check official firmware release")
        self.check.clicked.connect(self.check_release)
        line.addWidget(self.check)
        firmware.addLayout(line)
        self.version = self.label("Not checked yet", "muted")
        firmware.addWidget(self.version)
        firmware.addWidget(self.label("Downloaded from the project's GitHub Releases.\n"
                                     "The exact version and publisher are checked before writing.", "muted"))
        device = self.card(layout)
        device.addWidget(self.label("2   Connect the supported display", "step"))
        device.addWidget(self.label("Use a USB cable that carries data, not only power.", "muted"))
        self.board = WrappingCheckBox("I have the Waveshare ESP32-S3-LCD-2.8 — non-touch model")
        self.board.setMinimumHeight(28)
        self.board.setAccessibleName("Confirm supported non-touch Waveshare board")
        self.board.toggled.connect(self.refresh_controls)
        device.addWidget(self.board)
        ports = QHBoxLayout()
        self.port = QComboBox()
        # A device's long OS description must not enlarge the whole window.
        self.port.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        self.port.setMinimumContentsLength(12)
        self.port.setFixedHeight(42)
        self.port.currentTextChanged.connect(self.port.setToolTip)
        self.port.addItem(PORT_PLACEHOLDER, "")
        self.port.setAccessibleName("Display USB port")
        self.port.currentIndexChanged.connect(self.refresh_controls)
        ports.addWidget(self.port, 1)
        self.refresh = QPushButton("Find USB ports")
        self.refresh.setFixedHeight(42)
        self.refresh.clicked.connect(self.find_ports)
        ports.addWidget(self.refresh)
        device.addLayout(ports)
        device.addSpacing(4)
        device.addWidget(self.label("The ESP32-S3 chip alone cannot identify the exact display model.", "muted"))
        warning = self.card(layout)
        warning.addWidget(self.label("3   Install and set up", "step"))
        self.erase = WrappingCheckBox("I understand installation erases the display's saved settings")
        self.erase.setMinimumHeight(28)
        self.erase.toggled.connect(self.refresh_controls)
        warning.addWidget(self.erase)
        warning.addWidget(self.label("After installation, configure Wi-Fi and your printer on the\n"
                                    "display's local setup page. They are never entered in this app.", "muted"))
        self.status = self.label("Publisher trust is not configured in this local preview.\n"
                                 "USB access and installation are disabled.", "status")
        layout.addWidget(self.status)
        self.progress = QProgressBar()
        self.progress.setTextVisible(False)
        self.progress.hide()
        layout.addWidget(self.progress)
        self.install_button = QPushButton("Install firmware")
        self.install_button.setObjectName("primary")
        self.install_button.clicked.connect(self.confirm_install)
        layout.addWidget(self.install_button)
        self.footer = self.label("Independent project · Not affiliated with Prusa Research\n"
                            f"Installer {core.APP_VERSION} · Initial USB installation replaces settings", "muted")
        self.footer.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.footer)
        QApplication.styleHints().colorSchemeChanged.connect(self.apply_theme)
        self.apply_theme()
        if self.session.enabled:
            self.message("Check the official firmware release to begin.")
        self.refresh_controls()

    def apply_theme(self, *_):
        mode = self.theme.currentData()
        if mode == "system":
            mode = "dark" if QApplication.styleHints().colorScheme() == Qt.ColorScheme.Dark else "light"
        self.resolved_theme = mode
        self.setStyleSheet(theme_css(mode, self.resources / "desktop-installer/assets"))

    @staticmethod
    def label(text, name):
        result = QLabel(text)
        result.setObjectName(name)
        result.setWordWrap(True)
        result.setTextFormat(Qt.TextFormat.PlainText)
        return result

    @staticmethod
    def card(parent):
        frame = QFrame()
        frame.setObjectName("card")
        box = QVBoxLayout(frame)
        box.setContentsMargins(19, 16, 19, 16)
        box.setSpacing(9)
        parent.addWidget(frame)
        return box

    def message(self, text, error=False):
        self.status.setText(text)
        self.status.setObjectName("error" if error else "status")
        self.status.style().unpolish(self.status)
        self.status.style().polish(self.status)

    def refresh_controls(self, *_):
        self.session.port = self.port.currentData() or ""
        self.session.board_confirmed = self.board.isChecked()
        self.session.erase_confirmed = self.erase.isChecked()
        self.install_button.setEnabled(self.session.can_install and not self.working)
        self.check.setEnabled(not self.working)
        self.refresh.setEnabled(self.session.enabled and self.session.board_confirmed and not self.working)
        for control in (self.board, self.erase, self.port):
            control.setEnabled(not self.working)

    def submit(self, function, completed):
        self.working = True
        self.progress.setRange(0, 0)
        self.progress.show()
        self.refresh_controls()
        task = Task(function)
        self.tasks.append(task)

        def finish(result, error):
            self.working = False
            self.progress.hide()
            self.tasks.remove(task)
            completed(result, error)
            self.refresh_controls()
        task.signals.done.connect(finish)
        QThreadPool.globalInstance().start(task)

    def check_release(self):
        self.session.image = None
        self.release = None
        self.message("Checking official firmware…")

        def complete(release, error):
            if error:
                self.version.setText("No installation-ready release found")
                self.message(error, True)
                return
            self.release = release
            self.version.setText(f"Firmware {release.tag} · commit {release.commit[:12]}")
            if not self.public_key:
                self.message("Release found, but publisher trust is not configured in this preview.\n"
                             "USB access and installation remain disabled.")
                return
            self.message("Downloading and verifying the exact firmware package…")
            self.submit(lambda: core.prepare(release, self.public_key, self.resources), prepared)

        def prepared(image, error):
            if error:
                self.message(error, True)
            else:
                self.session.image = image
                self.message(f"Firmware {image.release.tag} verified. Confirm the board and choose its USB port.")
        self.submit(core.discover, complete)

    def find_ports(self):
        if not self.session.enabled or not self.board.isChecked() or self.working:
            return
        from serial.tools import list_ports
        self.port.clear()
        self.port.addItem(PORT_PLACEHOLDER, "")
        try:
            for item in list_ports.comports():
                self.port.addItem(f"{item.device} — {item.description}", item.device)
            if self.port.count() == 1:
                self.message("No USB port found. Check the data cable, then try Find USB ports again.")
        except Exception:
            self.message("Unable to list USB ports. Check permissions and reconnect the display.", True)

    def confirm_install(self):
        if not self.session.can_install or self.working:
            return
        port = self.session.port
        choice = QMessageBox.warning(self, "Erase and install firmware?",
                                     f"Install {self.session.image.release.tag} on {port}?\n\n"
                                     "This erases the display's saved settings. Keep USB connected.\n"
                                     "Use only the confirmed non-touch Waveshare model.",
                                     QMessageBox.StandardButton.Cancel | QMessageBox.StandardButton.Ok,
                                     QMessageBox.StandardButton.Cancel)
        if choice != QMessageBox.StandardButton.Ok:
            return
        self.message("Writing and verifying firmware. Keep USB connected; do not close the app…")

        def write():
            # esptool performs its own connection/security checks and write verification.
            # Do not expose a shell or force/secure-boot/eFuse bypass.
            output = io.StringIO()
            with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
                core.install(self.session, port)
            return True

        def complete(result, error):
            self.session.image = None
            self.erase.setChecked(False)
            if error:
                self.message("Installation did not complete. " + error +
                             "\nKeep the display connected. Recheck firmware before retrying.", True)
            else:
                self.message("Firmware write verified. The display has been asked to restart.\n"
                             "Check its screen, then use its local Wi-Fi setup page.")
        self.submit(write, complete)

    def closeEvent(self, event):
        if self.working:
            self.message("Wait for the current operation to finish before closing the app.")
            event.ignore()
        else:
            event.accept()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", action="store_true", help="disable all USB access")
    parser.add_argument("--screenshot", type=Path, help="render no-hardware preview and exit")
    parser.add_argument("--theme", choices=("system", "dark", "light"), default="system")
    args = parser.parse_args()
    app = QApplication(sys.argv[:1])
    app.setStyle("Fusion")
    resource_root = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parents[1]))
    window = Window(resource_root, preview=args.preview or bool(args.screenshot))
    window.theme.setCurrentIndex(window.theme.findData(args.theme))
    window.show()
    if args.screenshot:
        def capture():
            if not window.grab().save(str(args.screenshot)):
                raise RuntimeError("Screenshot could not be saved")
            app.quit()
        QTimer.singleShot(300, capture)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
