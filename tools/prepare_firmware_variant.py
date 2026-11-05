"""Create a firmware-mode bundle before signing; retain the probe output."""
from pathlib import Path
from datetime import datetime, timezone
import plistlib, shutil

def prepare(src, dst):
    if dst.exists():
        previous=dst.with_name('MT7902Bt.previous-'+datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%f')+'.kext')
        dst.rename(previous)
    shutil.copytree(src,dst)
    f=dst/'Contents/Info.plist';d=plistlib.loads(f.read_bytes())
    for e in d['IOKitPersonalities'].values():
        assert e['MT7902AllowReset'] is False
        e['MT7902ProbeOnly']=False
    f.write_bytes(plistlib.dumps(d,sort_keys=False))

if __name__=='__main__':
    p=Path(__file__).resolve().parents[1]
    prepare(p/'out/MT7902Bt.kext',p/'out/firmware/MT7902Bt.kext')
