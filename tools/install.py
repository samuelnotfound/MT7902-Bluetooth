"""Install a locally built probe or firmware-loader bundle on an existing EFI."""
from pathlib import Path
import argparse, plistlib, struct, hashlib, datetime, shutil, os, sys
ap=argparse.ArgumentParser(__doc__)
ap.add_argument('--efi',type=Path,required=True)
mode=ap.add_mutually_exclusive_group(required=True)
mode.add_argument('--probe-only',action='store_true')
mode.add_argument('--load-firmware',action='store_true')
a=ap.parse_args()
if sys.platform!='darwin':sys.exit('Run on macOS after a successful native build.')
root=a.efi.resolve(); cfgpath=root/'OC/config.plist'; kexts=root/'OC/Kexts'
project=Path(__file__).resolve().parents[1]; source=project/'out/MT7902Bt.kext'
exe=source/'Contents/MacOS/MT7902Bt'
b=exe.read_bytes()
if len(b)<32 or struct.unpack_from('<III',b)[0:2]!=(0xfeedfacf,0x01000007) or struct.unpack_from('<I',b,12)[0]!=11:
    sys.exit('Expected an x86_64 MH_KEXT_BUNDLE; no EFI changes made.')
info=plistlib.loads((source/'Contents/Info.plist').read_bytes())
if info['CFBundleIdentifier']!='local.experimental.MT7902Bluetooth':sys.exit('Wrong bundle identifier')
sys.path.insert(0, str(project/'tools'))
from bluetooth_config import configure_firmware_mode
original=cfgpath.read_bytes(); cfg=plistlib.loads(original)
active=[e['BundlePath'] for e in cfg['Kernel']['Add'] if e['Enabled']]
if any(any(s in name.lower() for s in ('simplemtkbt','mtkbt.kext','intelbluetoothfirmware','brcmpatchram')) for name in active if name!='MT7902Bt.kext'):
    sys.exit('Another Bluetooth firmware loader is enabled. Disable it first; no changes made.')
if a.load_firmware and not all(x in active for x in ('BlueToolFixup.kext','Lilu.kext')):
    sys.exit('Firmware mode requires BlueToolFixup (official BrcmPatchRAM release) and Lilu enabled. No changes made.')
if a.load_firmware:
    configure_firmware_mode(cfg)
stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S-UTC')
backup=root/'Backups'/('MT7902-'+stamp); backup.mkdir(parents=True)
(backup/'config.plist').write_bytes(original)
target=kexts/'MT7902Bt.kext'; staged=backup/'staged/MT7902Bt.kext'
shutil.copytree(source,staged)
pi=staged/'Contents/Info.plist'; inf=plistlib.loads(pi.read_bytes())
for personality in inf['IOKitPersonalities'].values():personality['MT7902ProbeOnly']=a.probe_only
pi.write_bytes(plistlib.dumps(inf,sort_keys=False))
# Info.plist edits invalidate the ad-hoc signature; regenerate it before copying.
import subprocess
subprocess.run(['/usr/bin/codesign','--force','--sign','-',str(staged)],check=True)
subprocess.run(['/usr/bin/codesign','--verify','--strict',str(staged)],check=True)
entry={'Arch':'x86_64','BundlePath':'MT7902Bt.kext','Comment':'Experimental MT7902: '+('probe only' if a.probe_only else 'RAM firmware loader'), 'Enabled':True,'ExecutablePath':'Contents/MacOS/MT7902Bt','PlistPath':'Contents/Info.plist','MinKernel':'21.0.0','MaxKernel':''}
cfg['Kernel']['Add']=[e for e in cfg['Kernel']['Add'] if e['BundlePath']!='MT7902Bt.kext']; cfg['Kernel']['Add'].append(entry)
if a.load_firmware:
    configure_firmware_mode(cfg)
old=backup/'previous-MT7902Bt.kext'; moved=False; installed=False
try:
    if cfgpath.read_bytes()!=original:raise RuntimeError('Config changed during preparation')
    if target.exists():target.rename(old);moved=True
    staged.rename(target);installed=True
    temp=backup/'config.new.plist';temp.write_bytes(plistlib.dumps(cfg,sort_keys=False));os.replace(temp,cfgpath)
except Exception:
    if installed:target.rename(backup/'failed-install.kext')
    if moved:old.rename(target)
    cfgpath.write_bytes(original)
    raise
print('Installed',entry['Comment']); print('Backup:',backup)
print('Reboot, then: sudo bash "'+str(project/'tools/collect-mac.sh')+'"')
print('Rollback: restore',backup/'config.plist','to',cfgpath)
