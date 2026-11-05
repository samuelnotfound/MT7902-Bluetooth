from pathlib import Path
import tempfile, plistlib, unittest, importlib.util
p=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('variant',p/'tools/prepare_firmware_variant.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class SignedVariantTests(unittest.TestCase):
    def test_variant_preserves_probe_binary_and_disables_only_probe(self):
        with tempfile.TemporaryDirectory() as td:
            src=Path(td)/'probe.kext';dst=Path(td)/'firmware.kext'
            (src/'Contents/MacOS').mkdir(parents=True)
            (src/'Contents/Info.plist').write_bytes((p/'Info.plist').read_bytes())
            (src/'Contents/MacOS/MT7902Bt').write_bytes(b'binary-preserved')
            before=(src/'Contents/Info.plist').read_bytes();m.prepare(src,dst)
            self.assertEqual((src/'Contents/Info.plist').read_bytes(),before)
            self.assertEqual((dst/'Contents/MacOS/MT7902Bt').read_bytes(),b'binary-preserved')
            d=plistlib.loads((dst/'Contents/Info.plist').read_bytes())
            for e in d['IOKitPersonalities'].values():
                self.assertFalse(e['MT7902ProbeOnly']);self.assertFalse(e['MT7902AllowReset'])
if __name__=='__main__':unittest.main()
