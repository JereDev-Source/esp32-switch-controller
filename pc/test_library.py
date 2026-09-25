import json
import tempfile
import unittest
from pathlib import Path

from library import Library, Presenter
from serial_link import LineDecoder, parse_reply


def dump(path, discriminator=0):
    data = bytearray(540)
    data[0] = 4
    data[3] = 0x8c
    data[100] = discriminator
    path.write_bytes(data)
    return path


class LibraryTests(unittest.TestCase):
    def test_nested_folders_duplicate_names_and_path_search(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            (folder / "Zelda" / "Series").mkdir(parents=True)
            (folder / "Smash").mkdir()
            top = dump(folder / "Root.bin")
            nested = dump(folder / "Zelda" / "Series" / "Link.BIN")
            duplicate = dump(folder / "Smash" / "Link.bin", 1)
            (folder / "Zelda" / "notes.txt").write_text("Not a BIN")
            library = Library(folder / "settings.json")
            library.folder = temp
            self.assertEqual(set(library.entries()), {top, nested, duplicate})
            self.assertEqual(library.relative_path(nested), "Zelda/Series/Link.BIN")
            self.assertEqual(library.entries("ZELDA/SERIES"), [nested])
            self.assertEqual(len(library.entries("link")), 2)
            library.rename(nested, "Hero")
            self.assertEqual(library.entries("hero"), [nested])
            self.assertEqual(library.name(duplicate), "Link")

    def test_folder_port_names_and_search_survive_restart_without_changing_bins(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            first = dump(folder / "tag.bin")
            other = dump(folder / "other.BIN", 2)
            original = first.read_bytes()
            prefs = folder / "prefs" / "settings.json"
            library = Library(prefs)
            library.folder = temp
            library.port = "COM7"
            library.rename(first, "Kirby favorito")
            restored = Library(prefs)
            self.assertEqual(restored.port, "COM7")
            self.assertEqual(restored.folder, temp)
            self.assertEqual(restored.entries("FAVORITO"), [first])
            self.assertEqual(restored.entries("other.bin"), [other])
            self.assertEqual(first.read_bytes(), original)
            restored.rename(first, "")
            self.assertEqual(Library(prefs).name(first), "tag")

    def test_invalid_settings_and_missing_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "settings.json"
            for content in ("broken", "[]", "null"):
                path.write_text(content)
                model = Library(path)
                self.assertTrue(model.warning)
                self.assertEqual(model.entries(), [])
            path.write_text(json.dumps({"folder": str(Path(temp) / "missing")}))
            with self.assertRaises(FileNotFoundError):
                Library(path).entries()


class PresenterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.file = dump(Path(self.temp.name) / "first.bin")
        self.other = dump(Path(self.temp.name) / "second.bin", 1)
        self.commands = []
        self.scheduled = []
        self.errors = []
        self.model = Presenter(lambda cmd, cb: self.commands.append((cmd, cb)),
                               lambda delay, cb: self.scheduled.append((delay, cb)),
                               lambda: None, self.errors.append)

    def acknowledge(self, **overrides):
        command, callback = self.commands.pop(0)
        reply = dict(ok=True, connected=True, loaded=command["cmd"] == "load",
                     crc=command.get("crc", 0))
        reply.update(overrides)
        self.model.observe(reply)
        callback(reply)
        return command

    def complete(self, file):
        self.assertTrue(self.model.present(file))
        self.assertEqual(self.acknowledge()["cmd"], "unload")
        delay, callback = self.scheduled.pop(0)
        self.assertGreaterEqual(delay, 500)
        callback()
        self.assertEqual(self.acknowledge()["cmd"], "load")

    def test_switch_and_repeat_wait_for_ack_and_gap(self):
        self.complete(self.file)
        self.assertEqual(self.model.path, self.file.resolve())
        self.assertTrue(self.model.present(self.other))
        self.assertFalse(self.model.present(self.file))
        self.assertEqual(len(self.commands), 1)
        self.assertEqual(self.commands[0][0]["cmd"], "unload")
        self.assertEqual(self.scheduled, [])
        self.acknowledge()
        self.assertIsNone(self.model.path)
        self.assertFalse(self.model.loaded)
        self.assertEqual(self.commands, [])
        self.scheduled.pop()[1]()
        self.acknowledge()
        self.assertEqual(self.model.path, self.other.resolve())
        self.model.remove()
        self.acknowledge()
        self.assertEqual(self.model.last_path, self.other.resolve())
        self.complete(self.model.last_path)
        self.assertFalse(self.errors)

    def test_invalid_file_preserves_current_tag(self):
        self.complete(self.file)
        self.other.write_bytes(b"invalid")
        with self.assertRaises(ValueError):
            self.model.present(self.other)
        self.assertTrue(self.model.loaded)
        self.assertEqual(self.model.path, self.file.resolve())
        self.assertFalse(self.model.busy)
        self.assertEqual(self.commands, [])

    def test_rejected_removal_never_loads(self):
        self.model.present(self.file)
        self.acknowledge(ok=False, message="rejected")
        self.assertEqual(self.scheduled, [])
        self.assertEqual(self.errors, ["rejected"])
        self.assertFalse(self.model.busy)

    def test_disconnect_cancels_delayed_load_and_stale_reply(self):
        self.model.present(self.file)
        self.acknowledge()
        self.model.reset()
        self.scheduled.pop()[1]()
        self.assertEqual(self.commands, [])
        self.model.present(self.other)
        self.model.reset()
        self.commands.pop()[1](dict(ok=True, loaded=False))
        self.assertEqual(self.scheduled, [])

    def test_wrong_crc_does_not_claim_expected_file_present(self):
        self.model.present(self.file)
        self.acknowledge()
        self.scheduled.pop()[1]()
        self.acknowledge(crc=1)
        self.assertIsNone(self.model.path)
        self.assertTrue(self.errors)
        self.assertFalse(self.model.busy)

    def test_unknown_loaded_tag_and_device_reset_are_reported_truthfully(self):
        self.model.observe(dict(loaded=True, crc=13))
        self.assertIsNone(self.model.path)
        self.assertTrue(self.model.loaded)
        self.complete(self.file)
        self.model.observe(dict(loaded=False, crc=0))
        self.assertFalse(self.model.loaded)
        self.assertIsNone(self.model.path)


class DecoderTests(unittest.TestCase):
    def test_reply_with_log_prefix_and_suffix(self):
        line = 'I (123) LOG: @PC {"id":7,"ok":true,"connected":true,"loaded":false}extra log'
        self.assertEqual(parse_reply(line)["id"], 7)
        self.assertIsNone(parse_reply('I (123) NFC TX type=2A'))
        self.assertIsNone(parse_reply('@PC {"id":7,'))
        self.assertIsNone(parse_reply('@PC {"id":7,"ok":true}'))

    def test_partial_lines_unicode_and_multiple_responses(self):
        decoder = LineDecoder()
        packet = 'diagnóstico\r\n@PC {"id":1,"ok":true}\n@PC {"id":2}\n'.encode()
        lines = []
        for byte in packet:
            lines.extend(decoder.feed(bytes([byte])))
        self.assertEqual(lines, ['diagnóstico', '@PC {"id":1,"ok":true}', '@PC {"id":2}'])


if __name__ == "__main__":
    unittest.main()
