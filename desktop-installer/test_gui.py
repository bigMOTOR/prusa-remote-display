# SPDX-FileCopyrightText: 2026 Mykola Fiantsev
# SPDX-License-Identifier: GPL-3.0-only
"""Actual Qt controls with doubles, never real serial ports."""
import hashlib
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from PySide6.QtCore import QObject, Qt, Signal
from PySide6.QtWidgets import QApplication, QCheckBox, QFrame, QLayout, QMessageBox, QScrollArea, QVBoxLayout
from PySide6.QtTest import QTest
import app
import core


class GuiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.application = QApplication.instance() or QApplication([])
        cls.application.setStyle("Fusion")

    def setUp(self):
        self.window = app.Window(Path(__file__).resolve().parents[1], preview=True)
        self.window.show()
        self.application.processEvents()

    def tearDown(self):
        self.window.close()
        self.window.deleteLater()
        self.application.processEvents()

    def wait(self):
        for _ in range(150):
            self.application.processEvents()
            if not self.window.working:
                return
            QTest.qWait(10)
        self.fail("GUI task did not complete")

    def ready(self):
        data = b"test-fixture-not-firmware"
        self.window.session.image = core.VerifiedImage(
            core.Release("v0.1.0", "a" * 40, ()), data, hashlib.sha256(data).hexdigest())
        self.window.session.enabled = True
        self.window.port.addItem("Fake USB device", "test-port-not-a-device")
        self.window.port.setCurrentIndex(1)
        self.window.board.setChecked(True)
        self.window.erase.setChecked(True)
        self.window.refresh_controls()

    def test_preview_stays_disabled_even_with_checks(self):
        with patch("serial.tools.list_ports.comports") as scan, patch.object(core, "install") as write:
            self.window.board.setChecked(True)
            self.window.erase.setChecked(True)
            self.window.find_ports()
            self.window.confirm_install()
            self.assertFalse(self.window.install_button.isEnabled())
            self.assertFalse(self.window.refresh.isEnabled())
            scan.assert_not_called()
            write.assert_not_called()

    def test_cancel_does_not_write(self):
        self.ready()
        with patch.object(QMessageBox, "warning", return_value=QMessageBox.StandardButton.Cancel), \
                patch.object(core, "install") as write:
            self.window.confirm_install()
            write.assert_not_called()
        self.assertTrue(self.window.install_button.isEnabled())

    def test_failed_install_is_not_success_and_requires_reverification(self):
        self.ready()
        with patch.object(QMessageBox, "warning", return_value=QMessageBox.StandardButton.Ok), \
                patch.object(core, "install", side_effect=core.InstallError("test USB loss")):
            self.window.confirm_install()
            self.wait()
        self.assertIn("did not complete", self.window.status.text())
        self.assertNotIn("write verified", self.window.status.text())
        self.assertFalse(self.window.install_button.isEnabled())
        self.assertFalse(self.window.erase.isChecked())

    def test_system_exit_finishes_worker_and_unlocks_without_false_success(self):
        self.ready()
        with patch.object(QMessageBox, "warning", return_value=QMessageBox.StandardButton.Ok), \
                patch.object(core, "install", side_effect=SystemExit("Invalid esptool environment")):
            self.window.confirm_install()
            self.wait()
        self.assertIn("did not complete", self.window.status.text())
        self.assertFalse(self.window.install_button.isEnabled())
        self.assertTrue(self.window.check.isEnabled())
        self.assertFalse(self.window.erase.isChecked())
        self.assertIsNone(self.window.session.image)
        self.assertTrue(self.window.close())

    def test_small_screen_can_scroll_to_status_install_and_footer(self):
        self.window.resize(750, 600)
        self.application.processEvents()
        self.assertLessEqual(self.window.height(), 600)
        scroll = self.window.scroll
        self.assertGreater(scroll.verticalScrollBar().maximum(), 0)
        self.assertEqual(scroll.horizontalScrollBar().maximum(), 0, self.layout_diagnostics())
        for target in (self.window.status, self.window.install_button, self.window.footer):
            scroll.ensureWidgetVisible(target)
            self.application.processEvents()
            position = target.mapTo(scroll.viewport(), target.rect().center())
            self.assertTrue(scroll.viewport().rect().contains(position))

    def layout_diagnostics(self):
        scroll = self.window.scroll
        return {"platform": self.application.platformName(),
                "font": self.application.font().toString(),
                "viewport_width": scroll.viewport().width(),
                "root_minimum": scroll.widget().minimumSizeHint().width(),
                "board_minimum": self.window.board.minimumSizeHint().width(),
                "erase_minimum": self.window.erase.minimumSizeHint().width(),
                "port_minimum": self.window.port.minimumSizeHint().width()}

    def test_full_confirmations_wrap_when_actual_native_text_exceeds_width(self):
        # Do not depend on synthetic font stretch support/OS fallback metrics.
        # The unchanged whole-window test above still requires 750x600 to fit.
        for original in (self.window.board, self.window.erase):
            text = original.text()
            native_width = original.caption.fontMetrics().horizontalAdvance(text)
            available_width = int(native_width * .65)
            for control in (QCheckBox, app.WrappingCheckBox):
                with self.subTest(text=text, control=control.__name__):
                    scroll = QScrollArea()
                    scroll.setWidgetResizable(True)
                    scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
                    frame = QFrame()
                    frame.setObjectName("card")
                    layout = QVBoxLayout(frame)
                    layout.setContentsMargins(0, 0, 0, 0)
                    layout.setSizeConstraint(QLayout.SizeConstraint.SetMinimumSize)
                    checkbox = control(text)
                    layout.addWidget(checkbox)
                    scroll.setWidget(frame)
                    scroll.setStyleSheet(self.window.styleSheet())
                    scroll.resize(available_width, 180)
                    scroll.show()
                    try:
                        self.application.processEvents()
                        diagnostic = {"viewport": scroll.viewport().width(),
                                      "root_minimum": frame.minimumSizeHint().width(),
                                      "native_text_width": native_width,
                                      "font": checkbox.font().toString()}
                        if control is QCheckBox:
                            # Firing control: same text/font/width, old native one-line caption.
                            self.assertGreater(scroll.horizontalScrollBar().maximum(), 0, diagnostic)
                            continue
                        self.assertEqual(scroll.horizontalScrollBar().maximum(), 0, diagnostic)
                        caption = checkbox.caption
                        self.assertEqual(caption.text(), checkbox.text())
                        self.assertTrue(caption.wordWrap())
                        self.assertGreater(caption.height(), caption.fontMetrics().height())
                        self.assertGreaterEqual(caption.height(), caption.heightForWidth(caption.width()))
                        self.assertTrue(checkbox.rect().contains(caption.geometry()))
                        QTest.mouseClick(checkbox, Qt.MouseButton.LeftButton,
                                         pos=caption.geometry().bottomRight())
                        self.assertTrue(checkbox.isChecked())
                        checkbox.setFocus()
                        QTest.keyClick(checkbox, Qt.Key.Key_Space)
                        self.assertFalse(checkbox.isChecked())
                    finally:
                        scroll.close()
                        scroll.deleteLater()
                        self.application.processEvents()
        self.assertFalse(self.window.install_button.isEnabled())

    def test_confirmation_text_updates_native_and_visible_caption(self):
        checkbox = self.window.board
        text = checkbox.text() + " — updated confirmation"
        checkbox.setText(text)
        self.application.processEvents()
        self.assertEqual(checkbox.text(), text)
        self.assertEqual(checkbox.caption.text(), text)
        self.assertFalse(checkbox.isChecked())

    def test_long_usb_description_does_not_enlarge_window(self):
        self.window.resize(750, 600)
        description = "USB Serial Device — " + "long system device description " * 12
        self.window.port.addItem(description, "test-port-not-a-device")
        self.window.port.setCurrentIndex(1)
        self.application.processEvents()
        self.assertEqual(self.window.scroll.horizontalScrollBar().maximum(), 0,
                         self.layout_diagnostics())
        self.assertEqual(self.window.port.currentText(), description)
        self.assertEqual(self.window.port.toolTip(), description)

    def test_rescan_keeps_short_placeholder_and_full_device_description(self):
        self.ready()
        self.window.resize(750, 600)
        description = "long USB device description " * 12
        device = "test-port-not-a-device"
        for ports in ([], [SimpleNamespace(device=device, description=description)]):
            with patch("serial.tools.list_ports.comports", return_value=ports) as scan:
                self.window.find_ports()
                scan.assert_called_once_with()
            self.application.processEvents()
            self.assertEqual(self.window.port.currentIndex(), 0)
            self.assertEqual(self.window.port.itemText(0), "Select USB port")
            self.assertEqual(self.window.port.toolTip(), "Select USB port")
            self.assertFalse(self.window.install_button.isEnabled())
            if ports:
                self.window.port.setCurrentIndex(1)
                self.application.processEvents()
                self.assertEqual(self.window.port.currentData(), device)
                self.assertEqual(self.window.port.toolTip(), f"{device} — {description}")
            self.assertEqual(self.window.scroll.horizontalScrollBar().maximum(), 0,
                             self.layout_diagnostics())

    def test_primary_text_contrast_in_enabled_normal_and_hover_states(self):
        def luminance(color):
            channels = [int(color[i:i + 2], 16) / 255 for i in (1, 3, 5)]
            linear = [c / 12.92 if c <= .04045 else ((c + .055) / 1.055) ** 2.4 for c in channels]
            return sum(c * weight for c, weight in zip(linear, (.2126, .7152, .0722)))
        for color in (app.PRIMARY, app.PRIMARY_HOVER):
            self.assertGreaterEqual((1.05) / (luminance(color) + .05), 4.5)

    def test_focused_primary_has_visible_rendered_border_in_both_themes(self):
        self.ready()
        for mode, expected in (("light", "#202521"), ("dark", "#ed7d32")):
            with self.subTest(mode=mode):
                self.window.theme.setCurrentIndex(self.window.theme.findData(mode))
                self.window.install_button.setFocus()
                self.application.processEvents()
                button = self.window.install_button
                self.assertTrue(button.hasFocus())
                image = button.grab().toImage()
                # Sample the straight border, not rounded corners or label text.
                scale = image.devicePixelRatio()
                y = int(button.height() * scale / 2)
                border = image.pixelColor(int(scale), y).name()
                fill = image.pixelColor(int(6 * scale), y).name()
                self.assertEqual(border, expected)
                self.assertNotEqual(border, fill)

    def test_keyboard_focus_reaches_install_on_small_screen_and_svg_assets_load(self):
        from PySide6.QtGui import QIcon
        self.ready()
        self.window.resize(750, 600)
        self.window.theme.setFocus()
        visited = set()
        for _ in range(12):
            focused = self.application.focusWidget()
            visited.add(focused)
            if focused is self.window.install_button:
                position = focused.mapTo(self.window.scroll.viewport(), focused.rect().center())
                self.assertTrue(self.window.scroll.viewport().rect().contains(position))
                break
            QTest.keyClick(focused, Qt.Key.Key_Tab)
            self.application.processEvents()
        self.assertIn(self.window.install_button, visited)
        self.assertIn("QPushButton#primary:focus", self.window.styleSheet())
        for name in ("check.svg", "arrow-light.svg", "arrow-dark.svg"):
            icon = QIcon(str(self.window.resources / "desktop-installer/assets" / name))
            self.assertFalse(icon.pixmap(18, 18).isNull())

    def test_unavailable_release_and_recheck_clear_old_image(self):
        self.ready()
        with patch.object(core, "discover", side_effect=core.InstallError("No complete release")):
            self.window.check_release()
            self.wait()
        self.assertIsNone(self.window.session.image)
        self.assertIn("No complete release", self.window.status.text())
        self.assertFalse(self.window.install_button.isEnabled())

    def test_selected_release_never_enables_preview_or_scans(self):
        release = core.Release("v0.1.0", "a" * 40, ())
        with patch.object(core, "discover", return_value=release), \
                patch("serial.tools.list_ports.comports") as scan:
            self.window.check_release()
            self.wait()
            self.assertIn("v0.1.0", self.window.version.text())
            self.assertFalse(self.window.install_button.isEnabled())
            scan.assert_not_called()

    def test_theme_selection_preserves_installation_guards(self):
        self.window.board.setChecked(True)
        self.window.erase.setChecked(True)
        for name in ("dark", "light", "system"):
            self.window.theme.setCurrentIndex(self.window.theme.findData(name))
            self.application.processEvents()
            self.assertIn(self.window.resolved_theme, ("dark", "light"))
            if name != "system":
                self.assertEqual(self.window.resolved_theme, name)
            self.assertFalse(self.window.install_button.isEnabled())
            self.assertFalse(self.window.refresh.isEnabled())

    def test_system_theme_follows_color_scheme_but_override_remains_explicit(self):
        # Offscreen Qt cannot change the OS scheme; drive the same Qt signal seam.
        class Hints(QObject):
            colorSchemeChanged = Signal(object)
            scheme = Qt.ColorScheme.Light

            def colorScheme(self):
                return self.scheme

            def change(self, scheme):
                self.scheme = scheme
                self.colorSchemeChanged.emit(scheme)

        hints = Hints()
        with patch.object(QApplication, "styleHints", return_value=hints):
            window = app.Window(Path(__file__).resolve().parents[1], preview=True)
        try:
            with patch.object(QApplication, "styleHints", return_value=hints):
                hints.change(Qt.ColorScheme.Dark)
                self.assertEqual(window.resolved_theme, "dark")
                window.theme.setCurrentIndex(window.theme.findData("light"))
                hints.change(Qt.ColorScheme.Light)
                hints.change(Qt.ColorScheme.Dark)
                self.assertEqual(window.resolved_theme, "light")
        finally:
            window.close()
            window.deleteLater()


if __name__ == "__main__":
    unittest.main()
