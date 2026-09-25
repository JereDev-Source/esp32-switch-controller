import json
import queue
import tempfile
import unittest
from pathlib import Path
from session import SerialSession, parse_command

class FakeSerial:
    def __init__(self):
        self.rx=queue.Queue()
        self.commands=[]
        self.closed=False
    def read(self,n):
        try: return self.rx.get(timeout=.01)
        except queue.Empty: return b""
    def write(self,data):
        cmd=json.loads(data);self.commands.append(cmd)
        self.rx.put(b"HID RX rejected id=0x11 actual=47 expected=48\n")
        response=("@PC "+json.dumps(dict(id=cmd["id"],ok=True))+"\n").encode()
        self.rx.put(response[:8]);self.rx.put(response[8:])
        return len(data)
    def close(self): self.closed=True

class SessionTests(unittest.TestCase):
    def test_windows_path(self):
        self.assertEqual(parse_command('load "D:\\mis datos\\Kirby.bin"'),
                         ("load","D:\\mis datos\\Kirby.bin"))
    def test_press_validation(self):
        cmd,(state,duration)=parse_command("press L R --seconds 0.5")
        self.assertEqual((state["r"],state["l"],duration),(64,64,.5))
        for line in ("press L --seconds nan","press","press L --seconds 61"):
            with self.assertRaises(ValueError): parse_command(line)
    def test_capture_during_commands(self):
        with tempfile.TemporaryDirectory() as d:
            port=FakeSerial(); path=Path(d)/"capture.log"
            session=SerialSession(port,path)
            session.send({"cmd":"status"})
            session.press(dict(r=0,s=0,l=64,lx=2048,ly=2048,rx=2048,ry=2048),.01)
            session.close()
            self.assertTrue(port.closed)
            self.assertEqual(port.commands[-1]["cmd"],"release")
            self.assertIn("HID RX rejected",path.read_text())
            self.assertIn("@PC",path.read_text())
if __name__=="__main__": unittest.main()
