import tempfile
import unittest
import zlib
from pathlib import Path
from protocol import state_for, load_bin

class ProtocolTests(unittest.TestCase):
    def test_grip_and_release(self):
        axes=dict(lx=2048,ly=2048,rx=2048,ry=2048)
        state=state_for({"L","R"},axes)
        self.assertEqual((state["l"],state["r"]), (64,64))
        self.assertEqual(state_for(set(),axes)["l"],0)
    def test_bin_validation(self):
        data=bytearray(540);data[0]=4;data[3]=0x8c
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/"test.bin";p.write_bytes(data)
            result=load_bin(p)
            self.assertEqual(result["crc"],zlib.crc32(data))
            self.assertEqual(bytes.fromhex(result["hex"]),data)
            data[8]=1;p.write_bytes(data)
            with self.assertRaises(ValueError): load_bin(p)
            p.write_bytes(bytes(539))
            with self.assertRaises(ValueError): load_bin(p)
            p.write_bytes(bytes(541))
            with self.assertRaises(ValueError): load_bin(p)
    def test_unknown_button_rejected(self):
        with self.assertRaises(KeyError): state_for({"bogus"},{})
if __name__=="__main__": unittest.main()
