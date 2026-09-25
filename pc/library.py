"""Local library preferences and acknowledged NFC presentation workflow."""
import json
import os
from pathlib import Path

from protocol import load_bin


def settings_path():
    base = Path(os.environ.get("APPDATA", Path.home() / ".config"))
    return base / "ESP32SwitchController" / "settings.json"


class Library:
    def __init__(self, path=None):
        self.path = Path(path) if path is not None else settings_path()
        self.folder = ""
        self.port = "COM3"
        self.names = {}
        self.warning = ""
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
            if not isinstance(data, dict):
                raise ValueError("Invalid settings")
            self.folder = data.get("folder", "") if isinstance(data.get("folder"), str) else ""
            self.port = data.get("port", "COM3") if isinstance(data.get("port"), str) else "COM3"
            names = data.get("names", {})
            if isinstance(names, dict):
                self.names = {k: v for k, v in names.items() if isinstance(v, str)}
        except FileNotFoundError:
            pass
        except (OSError, ValueError):
            self.warning = "No se pudieron leer las preferencias; se usarán los valores iniciales."

    def save(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temporary = self.path.with_suffix(".tmp")
        temporary.write_text(json.dumps({"folder": self.folder, "port": self.port,
                                         "names": self.names}, ensure_ascii=False, indent=2),
                             encoding="utf-8")
        temporary.replace(self.path)

    def name(self, path):
        return self.names.get(str(Path(path).resolve()), Path(path).stem)

    def relative_path(self, path):
        return Path(path).relative_to(Path(self.folder)).as_posix()

    def rename(self, path, name):
        key = str(Path(path).resolve())
        if name.strip():
            self.names[key] = name.strip()
        else:
            self.names.pop(key, None)
        self.save()

    def entries(self, search=""):
        if not self.folder:
            return []
        folder = Path(self.folder)
        if not folder.is_dir():
            raise FileNotFoundError("La carpeta guardada ya no está disponible. Selecciona otra.")
        needle = search.casefold().strip()
        paths = [p for p in folder.rglob("*") if p.suffix.lower() == ".bin" and p.is_file()]
        return sorted((p for p in paths
                       if needle in (self.name(p) + " " + self.relative_path(p)).casefold()),
                      key=lambda p: (self.name(p).casefold(), self.relative_path(p).casefold()))


class Presenter:
    """Only load after an acknowledged removal and a short, nonblocking gap.

    send(command, callback) must invoke callback exactly once for a reply/error.
    schedule(milliseconds, callback) is supplied by the GUI event loop.
    """
    def __init__(self, send, schedule, changed, error):
        self.send = send
        self.schedule = schedule
        self.changed = changed
        self.error = error
        self.busy = False
        self.phase = ""
        self.path = None
        self.last_path = None
        self.crc = None
        self.loaded = None
        self.generation = 0

    def reset(self):
        self.generation += 1
        self.busy = False
        self.phase = ""
        self.path = None
        self.crc = None
        self.loaded = None
        self.changed()

    def observe(self, reply):
        self.loaded = reply.get("loaded")
        if not self.loaded or reply.get("crc") != self.crc:
            self.path = None
            self.crc = None
        self.changed()

    def present(self, path):
        if self.busy:
            return False
        # Validate the entire candidate before removing the current tag.
        command = load_bin(path)
        candidate = Path(path).resolve()
        self._start("Retirando antes de presentar…")
        generation = self.generation

        def loaded(reply):
            if not self._accept(generation, reply):
                return
            if not reply.get("loaded") or reply.get("crc") != command["crc"]:
                self._fail("El ESP32 no confirmó el archivo esperado.")
                return
            self.path = self.last_path = candidate
            self.loaded = True
            self.crc = command["crc"]
            self._finish()

        def load():
            if generation != self.generation:
                return
            self.phase = "Presentando…"
            self.changed()
            self.send(command, loaded)

        def removed(reply):
            if not self._accept(generation, reply):
                return
            if reply.get("loaded") is not False:
                self._fail("El ESP32 no confirmó la retirada.")
                return
            self.loaded = False
            self.path = None
            self.crc = None
            self.phase = "Esperando para volver a presentar…"
            self.changed()
            self.schedule(500, load)

        self.send({"cmd": "unload"}, removed)
        return True

    def remove(self):
        if self.busy:
            return False
        self._start("Retirando…")
        generation = self.generation

        def removed(reply):
            if not self._accept(generation, reply):
                return
            if reply.get("loaded") is not False:
                self._fail("El ESP32 no confirmó la retirada.")
                return
            self.path = None
            self.crc = None
            self.loaded = False
            self._finish()

        self.send({"cmd": "unload"}, removed)
        return True

    def _start(self, phase):
        self.generation += 1
        self.busy = True
        self.phase = phase
        self.changed()

    def _accept(self, generation, reply):
        if generation != self.generation:
            return False
        if not reply.get("ok"):
            self._fail(reply.get("message", "No se pudo completar la operación."))
            return False
        return True

    def _fail(self, message):
        self._finish()
        self.error(message)

    def _finish(self):
        self.busy = False
        self.phase = ""
        self.changed()
