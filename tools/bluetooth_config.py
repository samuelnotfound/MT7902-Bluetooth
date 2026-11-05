"""BlueToolFixup third-party-controller NVRAM, separate from hardware probing."""
GUID = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
VALUES = {'bluetoothExternalDongleFailed': bytes(1), 'bluetoothInternalControllerInfo': bytes(14)}

def configure_firmware_mode(config):
    enabled = [e['BundlePath'] for e in config['Kernel']['Add'] if e['Enabled']]
    required = ('Lilu.kext', 'BlueToolFixup.kext')
    if not all(x in enabled for x in required):
        raise ValueError('Enable Lilu and BlueToolFixup before firmware mode')
    if enabled.index(required[0]) > enabled.index(required[1]):
        raise ValueError('Lilu must precede BlueToolFixup')
    nvram = config.setdefault('NVRAM', {})
    add = nvram.setdefault('Add', {}).setdefault(GUID, {})
    add.update(VALUES)
    delete = nvram.setdefault('Delete', {}).setdefault(GUID, [])
    for key in VALUES:
        if key not in delete: delete.append(key)
