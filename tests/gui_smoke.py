"""Exercise the real Tk window and serial worker without a physical ESP32."""
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import types
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "pc"))
import tkinter as tk
from controller import App
from library import Library


class FakePort:
    def __init__(self, **kwargs):
        self.is_open = False
        self.port = kwargs.get("port")
        self.loaded = False
        self.crc = 0
        self.data = bytearray()
        self.commands = []
        self.drop_replies = {}
        self.logs_only = False
        self.lock = threading.Lock()

    def open(self):
        self.is_open = True

    def close(self):
        self.is_open = False

    def write(self, packet):
        if not self.is_open:
            raise OSError("Closed")
        command = json.loads(packet)
        with self.lock:
            self.commands.append((time.monotonic(), command))
            if command["cmd"] == "load":
                self.loaded = True
                self.crc = command["crc"]
            elif command["cmd"] == "unload":
                self.loaded = False
                self.crc = 0
            reply = dict(id=command["id"], ok=True, connected=True,
                         loaded=self.loaded, crc=self.crc)
            if self.logs_only:
                self.data.extend(b'I (5000) BTSTACK_HID: NFC TX type=2A\n')
                return len(packet)
            if self.drop_replies.get(command["cmd"], 0):
                self.drop_replies[command["cmd"]] -= 1
                return len(packet)
            self.data.extend(("@PC " + json.dumps(reply) + "\n").encode())
        return len(packet)

    def read(self, count):
        time.sleep(0.002)
        if not self.is_open:
            raise OSError("Unplugged")
        with self.lock:
            # Deliberately split every response across multiple reads.
            result = bytes(self.data[:17])
            del self.data[:17]
            return result


class GuiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        for i, name in enumerate(("Kirby.bin", "Link.BIN")):
            data = bytearray(540)
            data[0] = 4
            data[3] = 0x8c
            data[100] = i
            (self.folder / name).write_bytes(data)
        self.library = Library(self.folder / "settings.json")
        self.library.folder = str(self.folder)
        self.root = tk.Tk()
        self.root.withdraw()
        with patch.object(App, "refresh_ports"):
            self.app = App(self.root, self.library)
        self.addCleanup(self.app.close)
        self.app.port.set("COM_FAKE")
        with patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=FakePort)}):
            self.app.connect()
        self.port = self.app.link.port
        self.pump_until(lambda: self.app.ready)

    def pump_until(self, predicate, timeout=3):
        deadline = time.monotonic() + timeout
        while not predicate() and time.monotonic() < deadline:
            self.root.update()
            time.sleep(0.005)
        self.assertTrue(predicate(), self.app.operation.get())

    def select(self, name):
        row = next(row for row, path in self.app.rows.items() if path.name == name)
        self.app.tree.selection_set(row)
        self.root.update()

    def test_full_library_workflow_keeps_controller_running(self):
        self.assertEqual(Library(self.library.path).port, "COM_FAKE")
        self.assertEqual(len(self.app.rows), 2)
        self.select("Kirby.bin")
        self.assertFalse(self.port.loaded)  # Selection alone never presents.
        self.app.present_selected()
        self.pump_until(lambda: not self.app.presenter.busy)
        self.assertTrue(self.port.loaded)
        self.assertIn("Kirby", self.app.file_status.get())
        with patch("controller.simpledialog.askstring", return_value="Mi Kirby"):
            self.app.rename()
        self.assertIn("Mi Kirby", self.app.file_status.get())
        self.app.search.set("MI KIRBY")
        self.assertEqual(len(self.app.rows), 1)
        self.assertEqual(self.app.selected().name, "Kirby.bin")
        self.app.keydown(types.SimpleNamespace(widget=self.app.search_box, keysym="x"))
        self.assertNotIn("A", self.app.pressed)
        self.app.search.set("")
        self.select("Link.BIN")
        self.app.pressed.add("A")
        started = time.monotonic()
        self.app.present_selected()
        self.pump_until(lambda: not self.app.presenter.busy)
        commands = [(t, c) for t, c in self.port.commands if t >= started]
        removal = next(t for t, c in commands if c["cmd"] == "unload")
        load = next(t for t, c in commands if c["cmd"] == "load")
        self.assertGreaterEqual(load - removal, 0.5)
        self.assertTrue(any(c["cmd"] == "state" and c["r"] == 8 for t, c in commands
                            if removal < t < load))
        self.assertIn("Link", self.app.file_status.get())
        self.app.remove()
        self.pump_until(lambda: not self.app.presenter.busy)
        self.assertFalse(self.port.loaded)
        self.app.repeat()
        self.pump_until(lambda: not self.app.presenter.busy)
        self.assertTrue(self.port.loaded)
        self.assertIn("Link", self.app.file_status.get())
        self.app.disconnect()
        self.assertFalse(self.app.ready)
        self.assertEqual(self.app.axes, dict.fromkeys(("lx", "ly", "rx", "ry"), 2048))

    def test_invalid_file_and_disconnect_during_gap(self):
        self.select("Kirby.bin")
        self.app.present_selected()
        self.pump_until(lambda: not self.app.presenter.busy)
        (self.folder / "Link.BIN").write_bytes(b"invalid")
        self.select("Link.BIN")
        before = len([c for _, c in self.port.commands if c["cmd"] == "unload"])
        self.app.present_selected()
        self.assertTrue(self.app.operation.get().startswith("Error:"))
        self.assertTrue(self.port.loaded)
        self.assertEqual(before, len([c for _, c in self.port.commands if c["cmd"] == "unload"]))
        self.app.repeat()
        self.pump_until(lambda: bool(self.app.presenter.phase.startswith("Esperando")))
        self.app.disconnect()
        until = time.monotonic() + 0.7
        while time.monotonic() < until:
            self.root.update()
            time.sleep(0.01)
        self.assertFalse(self.port.loaded)
        self.assertFalse(self.app.presenter.busy)

    def test_unplug_clears_connection_and_input(self):
        self.app.pressed.add("A")
        self.app.sliders["lx"].set(10)
        self.port.close()
        self.pump_until(lambda: self.app.link is None)
        self.assertFalse(self.app.ready)
        self.assertFalse(self.app.connected)
        self.assertFalse(self.app.pressed)
        self.assertEqual(self.app.axes["lx"], 2048)
        self.assertIn("desconectado", self.app.pc_status.get())
        self.assertIn("desconocido", self.app.file_status.get())

    def test_lost_state_reply_does_not_stop_controller_updates(self):
        self.port.drop_replies["state"] = 1
        before = len(self.port.commands)
        self.app.pressed.add("A")
        self.pump_until(lambda: sum(c["cmd"] == "state" for _, c in
                                   self.port.commands[before:]) >= 5, timeout=1.5)
        self.assertTrue(self.app.ready)

    def test_lost_release_reply_does_not_disconnect_a_responsive_device(self):
        self.port.drop_replies["release"] = 1
        self.app.release_all()
        until = time.monotonic() + 3.3
        while time.monotonic() < until:
            self.root.update()
            time.sleep(0.005)
        self.assertIsNotNone(self.app.link)
        self.assertTrue(self.app.ready)

    def test_lost_load_reply_does_not_repeat_nfc_or_disconnect(self):
        self.port.drop_replies["load"] = 1
        self.select("Kirby.bin")
        self.app.present_selected()
        self.pump_until(lambda: not self.app.presenter.busy, timeout=4.5)
        self.assertIsNotNone(self.app.link)
        self.assertTrue(self.port.loaded)
        self.assertIsNone(self.app.presenter.path)
        self.assertEqual(sum(c["cmd"] == "load" for _, c in self.port.commands), 1)
        self.assertIn("Sin confirmación de load", self.app.operation.get())

    def test_logs_without_control_replies_still_time_out(self):
        self.port.logs_only = True
        self.pump_until(lambda: self.app.link is None, timeout=4)
        self.assertFalse(self.app.ready)
        self.assertIn("Sin respuestas de control", self.app.operation.get())


if __name__ == "__main__":
    unittest.main()
