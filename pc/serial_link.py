"""Serial I/O off the Tk thread, preserving partial lines across reads."""
import json
import queue
import threading


def parse_reply(line):
    """Recover a reply sharing a UART line with a firmware log prefix/suffix."""
    start = line.find("@PC ")
    if start < 0:
        return None
    try:
        reply, _ = json.JSONDecoder().raw_decode(line[start + 4:].lstrip())
    except ValueError:
        return None
    if not isinstance(reply, dict) or type(reply.get("id")) is not int:
        return None
    if any(type(reply.get(key)) is not bool for key in ("ok", "connected", "loaded")):
        return None
    return reply


class LineDecoder:
    def __init__(self):
        self.buffer = bytearray()

    def feed(self, chunk):
        self.buffer.extend(chunk)
        lines = []
        while b"\n" in self.buffer:
            raw, _, self.buffer = self.buffer.partition(b"\n")
            lines.append(raw.decode("utf-8", "replace").rstrip("\r"))
        if len(self.buffer) > 16384:
            self.buffer.clear()
        return lines


class SerialLink:
    def __init__(self, port, events):
        self.port = port
        self.events = events
        self.outgoing = queue.Queue(maxsize=16)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def send(self, command):
        self.outgoing.put_nowait((json.dumps(command, separators=(",", ":")) + "\n").encode())

    def run(self):
        decoder = LineDecoder()
        try:
            while not self.stop.is_set():
                try:
                    packet = self.outgoing.get_nowait()
                except queue.Empty:
                    packet = None
                if packet is not None and self.port.write(packet) != len(packet):
                    raise OSError("Escritura serie incompleta")
                for line in decoder.feed(self.port.read(2048)):
                    # Drop verbose firmware logs under load, never command replies.
                    if "@PC " in line or self.events.qsize() < 1000:
                        self.events.put((self, line))
        except Exception as exc:
            if not self.stop.is_set():
                self.events.put((self, exc))
        finally:
            try:
                self.port.write(b'{"id":0,"cmd":"release"}\n')
            except Exception:
                pass
            self.port.close()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=0.5)
        if self.thread.is_alive():
            self.port.close()
