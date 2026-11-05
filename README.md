# MT7902 Bluetooth for macOS

A Bluetooth RAM firmware loader for MediaTek MT7902, based on Linux btmtk and [WTLMtkBluetooth](https://github.com/watchthelight-labs/WTLMtkBluetooth). After loading firmware, it releases the USB interface to Apple's Bluetooth stack through BlueToolFixup.

## Compatibility

| Component | Verified configuration |
|---|---|
| Adapter | MediaTek MT7902, USB `13D3:3579` |
| Laptop | ASUS Vivobook X1605VA |
| macOS | Tahoe 26.7.2, build 25G332 |
| Dependencies | OpenCore, Lilu and BlueToolFixup 2.7.2 |

Bluetooth operation was confirmed on this configuration with version 0.1.0. The loader reported two firmware sections and 508,928 bytes transferred in approximately 3.2 seconds. Version 0.1.1 removes unused reset/retry code; it requires a separate hardware regression test. Other adapters, sleep/wake and individual Bluetooth profiles are not validated.

## Installation

Download the latest successful [Actions artifact](https://github.com/samuelnotfound/MT7902-Bluetooth/actions/workflows/build.yml?query=branch%3Amt7902-port) and extract it at the repository root. Install and enable Lilu and BlueToolFixup first. The adapter's internal USB port must be present in your USB map.

From the repository directory:

```sh
sudo python3 tools/install.py --efi /Volumes/OPENCORE/EFI --load-firmware
```

The installer selects the signed firmware-loading bundle, adds its OpenCore entry after the existing dependencies, configures NVRAM and saves a rollback backup. Reboot to load the driver. Use one firmware loader for this device.

For identification without uploading firmware, substitute `--probe-only`. This mode does not require BlueToolFixup or modify Bluetooth NVRAM.

### Load order and NVRAM

`Lilu → BlueToolFixup → MT7902Bt`

Under GUID `7C436110-AB2A-4BBB-A880-FE41995C9F82`, use these Data values in `NVRAM → Add` and include both key names in `NVRAM → Delete`:

| Key | Value |
|---|---|
| `bluetoothExternalDongleFailed` | `00` — 1 byte |
| `bluetoothInternalControllerInfo` | `0000000000000000000000000000` — 14 bytes |

The firmware-mode installer applies these automatically. `config-firmware-example.plist` is a merge reference, not a complete EFI configuration.

## Diagnostics and recovery

```sh
sudo bash tools/collect-mac.sh
```

Reports are saved under `reports/`. Check `MT7902BtState`, `MT7902BtStep`, `MT7902BtLastError` and the firmware-transfer counters. `ready` confirms loader completion; pairing and audio still depend on the macOS Bluetooth stack.

The driver does not flash firmware or perform subsystem resets. On failure it releases the interface and records an error. To roll back, restore the installer's backed-up `config.plist` to `EFI/OC/config.plist`.

## Building

Requires macOS with Xcode Command Line Tools and Python 3:

```sh
bash tools/build-mac.sh
```

Outputs:

- `out/firmware/MT7902Bt.kext` — firmware loading
- `out/MT7902Bt.kext` — probe only

Both bundles are ad-hoc signed and verified. GitHub Actions runs the tests and native build. Compilation does not replace hardware testing.

## License

ISC, with third-party USB code and MediaTek firmware under their respective licenses. See [NOTICE.md](NOTICE.md), [LICENSE](LICENSE) and [LICENSES](LICENSES). Keep these notices with binary distributions.
