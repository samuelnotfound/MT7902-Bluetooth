from pathlib import Path
import importlib.util, unittest, copy
p=Path(__file__).resolve().parents[1]/'tools/bluetooth_config.py'
s=importlib.util.spec_from_file_location('config',p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class BluetoothConfigurationTests(unittest.TestCase):
    def base(self):
        return {'Kernel':{'Add':[{'BundlePath':n,'Enabled':True} for n in ['Lilu.kext','BlueToolFixup.kext']]},'NVRAM':{'Add':{m.GUID:{'unrelated':b'keep','bluetoothInternalControllerInfo':bytes(16)}},'Delete':{m.GUID:['unrelated']}}}
    def test_exact_nvram_and_idempotence(self):
        c=self.base();m.configure_firmware_mode(c)
        a=c['NVRAM']['Add'][m.GUID];self.assertEqual(len(a['bluetoothInternalControllerInfo']),14);self.assertEqual(a['bluetoothExternalDongleFailed'],b'\0');self.assertEqual(a['unrelated'],b'keep')
        previous=copy.deepcopy(c);m.configure_firmware_mode(c);self.assertEqual(c,previous)
        self.assertTrue(set(m.VALUES).issubset(c['NVRAM']['Delete'][m.GUID]))
    def test_dependency_missing(self):
        c=self.base();c['Kernel']['Add'].pop()
        with self.assertRaises(ValueError):m.configure_firmware_mode(c)
    def test_order_invalid(self):
        c=self.base();c['Kernel']['Add'].reverse()
        with self.assertRaises(ValueError):m.configure_firmware_mode(c)
if __name__=='__main__':unittest.main()
