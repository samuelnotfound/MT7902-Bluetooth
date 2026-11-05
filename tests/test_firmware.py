"""Host tests: actual firmware plus corrupted headers/tables/sections."""
from pathlib import Path
import sys, struct, hashlib, importlib.util, unittest, plistlib
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('layout', ROOT/'tools/firmware_layout.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
FW = (ROOT/'firmware/BT_RAM_CODE_MT7902_1_1_hdr.bin').read_bytes()
SHA = '4f53b5e02fbd933172e18caf952bda410a877eaad00561781528cb4aff58dc38'
class FirmwareTests(unittest.TestCase):
    def corrupt(self, where, value):
        b = bytearray(FW); struct.pack_into('<I', b, where, value); return b
    def test_real_image(self):
        self.assertEqual(hashlib.sha256(FW).hexdigest(), SHA)
        s = m.validate_layout(FW)
        self.assertEqual([(x['offset'],x['size']) for x in s], [(288,87104),(87392,421824),(509216,0)])
        self.assertEqual(sum(x['size'] for x in s),508928)
    def test_truncation(self):
        for n in (0,31,95,96,287,1000,len(FW)-200):
            with self.subTest(n=n), self.assertRaises(ValueError): m.validate_layout(FW[:n])
    def test_invalid_counts(self):
        for n in (0,17,0xffffffff):
            with self.subTest(n=n), self.assertRaises(ValueError): m.validate_layout(self.corrupt(44,n))
    def test_offset_inside_metadata(self):
        with self.assertRaises(ValueError):m.validate_layout(self.corrupt(100,95))
    def test_integer_overflow_range(self):
        with self.assertRaises(ValueError):m.validate_layout(self.corrupt(112,0xffffffff))
    def test_overlap(self):
        with self.assertRaises(ValueError):m.validate_layout(self.corrupt(164,300))
    def test_usb_guard(self):
        d=plistlib.loads((ROOT/'Info.plist').read_bytes())
        self.assertEqual(len(d['IOKitPersonalities']),1)
        p=next(iter(d['IOKitPersonalities'].values()))
        self.assertEqual((p['idVendor'],p['idProduct'],p['bInterfaceNumber']),(0x13d3,0x3579,0))
        self.assertTrue(p['MT7902ProbeOnly']); self.assertFalse(p['MT7902AllowReset'])
if __name__=='__main__':unittest.main(verbosity=2)
