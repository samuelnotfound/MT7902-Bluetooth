# Experimental MT7902 Bluetooth for macOS

A source-level port for this ASUS X1605VA's MediaTek Bluetooth device, USB **13D3:3579**, based on Linux btmtk's 79xx firmware path and WTLMtkBluetooth's macOS USB integration.

**Status: native macOS GitHub Actions build PASSED; hardware validation pending.** The x86_64 kext compiled, ad-hoc signing/verification and all 10 tests passed. This does not establish working Bluetooth. Nothing from this project has been enabled in the active EFI.

## What was implemented

- Exact USB vendor/product and interface-zero match, plus an internal **0x7902** chip-ID guard. Other chips are refused before firmware/register writes.
- Actual linux-firmware MT7902 image, pinned by full SHA-256: 509320 bytes, build 20250826211444. Two nonempty sections total **508928 download bytes**; the third section is empty.
- Linux 79xx section queries, 250-byte WMT blocks, firmware activation delay, endpoint option and FUNC_CTRL handoff. Existing transport/callback code is inherited from the credited loader.
- Firmware-table preflight in both generator and driver; section bounds/overlap checks and bounded event/timeouts.
- Removed MT7925 sleep-protection and CONNV3 reset operations. Linux's mainline default 79xx reset path is present behind **MT7902AllowReset=false**, with a one-reset-per-boot budget if explicitly enabled.
- Default **MT7902ProbeOnly=true**: read chip/firmware identity, close the interface, publish diagnostics. It does not download firmware or write device registers, and does not turn Bluetooth off at shutdown. It temporarily opens the interface and drains pending interrupt events while probing.

It is a firmware loader, not a replacement for Apple's full Bluetooth stack. Pairing, audio, BLE and sleep/wake are unverified. Do not assume the MT7925-specific WTLLegionBtFix plugin is applicable.

## First test: build and install a read-only probe

The project is also copied to OPENCORE/EFI/MT7902-Bluetooth. In macOS, with Command Line Tools installed:

```sh
bash "/Volumes/OPENCORE/EFI/MT7902-Bluetooth/tools/build-mac.sh"
sudo python3 "/Volumes/OPENCORE/EFI/MT7902-Bluetooth/tools/install.py" --efi "/Volumes/OPENCORE/EFI" --probe-only
```

The build compiles the kext, checks MH_KEXT_BUNDLE/x86_64, kmod_info, unexpected C++ runtime symbols, then ad-hoc signs/verifies the bundle. It needs Kernel.framework headers in the selected macOS SDK. If compilation fails, stop and preserve its output; the installer refuses a missing or malformed binary. The published binary was compiled by GitHub Actions on macos-15; no binary was compiled on Windows.

The installer changes only this driver's Kernel/Add entry and bundle. It backs up config.plist and an existing copy before replacing them. Its backup path is printed. It does not alter other kexts, audio, input, graphics or SIP/AMFI settings. OpenCore schema validation has not been run on the future installed config; preserve the printed backup for recovery.

Reboot using that EFI, then collect:

```sh
bash "/Volumes/OPENCORE/EFI/MT7902-Bluetooth/tools/collect-mac.sh"
```

Reports stay in the project's reports folder. Expected: **MT7902BtState=probe-only**, **MT7902BtChipId low 16 bits = 0x7902**, and zero firmware bytes sent. A successful probe does not enable Bluetooth. Return to Windows so the report and native build output can be reviewed.

## Firmware-loading test (after reviewing probe results)

The code path is implemented, but is a separate hardware test. Enable Lilu and BlueToolFixup from the official acidanthera/BrcmPatchRAM release first. No other Bluetooth firmware loader should claim the device. This package does not bundle those dependencies. Then:

```sh
sudo python3 "/Volumes/OPENCORE/EFI/MT7902-Bluetooth/tools/install.py" --efi "/Volumes/OPENCORE/EFI" --load-firmware
```

This switches probe mode off and sets the BlueToolFixup NVRAM keys (14 zero bytes for bluetoothInternalControllerInfo; one zero byte for bluetoothExternalDongleFailed) in Add/Delete. It refuses a firmware revision other than the embedded _1_1 image. It writes firmware to RAM; no flash-writing code is present. Automatic subsystem reset stays disabled. On a clean upload, expect two sections and 508928 bytes sent; warm-start skips are recorded separately. Firmware-ready is distinct from macOS adopting a controller or successful pairing.

USB mapping must expose the internal Bluetooth port. An absent device cannot be fixed by the loader. If the USB ID is present but the class is absent, inspect matching, build/loading and driver conflicts. If state=error, MT7902BtLog/Step/LastError record the stage.

## Rollback

From Windows or another boot, restore **config.plist from the installer backup** to EFI/OC/config.plist. This removes the driver's enabled entry; its leftover kext folder will not load. Keep the existing working EFI available. The installer reports the original bundle path if replacing an earlier build. NVRAM entries from a firmware-loading test are recorded in the backed-up config.

## Validation and provenance

Host regression tests cover: actual image/layout, truncated files, invalid section counts, metadata offsets, overflow-sized sections, overlaps, and exact USB/probe/reset defaults. Python scripts compile; build/collection scripts pass Bash syntax checks. The firmware generator verifies both the manifest and separately pinned full digest. **CI additionally establishes native C++ compilation and signing, but does not establish kernel loading, USB protocol success or Bluetooth function.**

```sh
python3 -I tests/test_firmware.py
```

Sources and retained licenses:
- https://github.com/watchthelight-labs/WTLMtkBluetooth (pinned revision in UPSTREAM.json)
- https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btmtk.c
- https://github.com/torvalds/linux/blob/master/drivers/bluetooth/btmtk.h
- https://gitlab.com/kernel-firmware/linux-firmware/-/tree/main/mediatek
- https://github.com/acidanthera/BrcmPatchRAM

See NOTICE.md and LICENSES. Historical upstream documents describe MT7925, not the support status of this port.

## BlueToolFixup and NVRAM

Firmware-loading mode requires **Lilu -> BlueToolFixup -> MT7902Bt**. BlueToolFixup is from the official acidanthera/BrcmPatchRAM release, not a Broadcom firmware uploader. Keep a single MediaTek firmware loader for this device. The installer refuses missing dependencies or a Lilu/BlueToolFixup ordering violation.

Under GUID `7C436110-AB2A-4BBB-A880-FE41995C9F82`:

| Key | Type | Exact value |
|---|---|---|
| bluetoothExternalDongleFailed | Data | `00` (1 byte) |
| bluetoothInternalControllerInfo | Data | `0000000000000000000000000000` (14 bytes) |

Both keys also go in NVRAM/Delete under the same GUID. Firmware-mode installation writes these automatically and preserves unrelated variables. Probe-only mode intentionally leaves them alone and does not require BlueToolFixup; this isolates identification from the Bluetooth-daemon handoff.

`config-firmware-example.plist` is a merge example, not a complete EFI config. Existing Lilu entries should not be duplicated. The installer is the preferred way to add the MT7902 entry. BlueToolFixup must already be installed/enabled before choosing --load-firmware.

The recommended keys come directly from [BlueToolFixup's documentation](https://github.com/acidanthera/BrcmPatchRAM#bluetoolfixupkext). Its legacy -btlfxnvramcheck alternative only applies through macOS 14; use the documented variables on Tahoe.

## GitHub build

This is a fork of watchthelight-labs/WTLMtkBluetooth. Native macOS CI builds the experimental port and uploads the ad-hoc-signed kext with licenses and its checksum. A successful CI build establishes compilation and packaging, not hardware operation. This ASUS adapter still needs a probe and firmware-loading test.

## Verified native build

[Successful macOS Actions run](https://github.com/samuelnotfound/MT7902-Bluetooth/actions/runs/42052282275) · [Download artifact](https://github.com/samuelnotfound/MT7902-Bluetooth/actions/runs/42052282275/artifacts/13426379236). Artifact downloads require a GitHub login and are retained for 7 days.

Compiled source: e5f7e7277105fe24bca7f5ecce8c400b5c7b54d1. Later documentation/provenance commits do not change those compiled inputs. `CI-BUILD.json` records the artifact and executable digests. The binary is 581712 bytes, x86_64 MH_KEXT_BUNDLE; its embedded MT7902 firmware was checked byte-for-byte after download.

For the first test, extract the artifact at the repository root so `out/MT7902Bt.kext` exists, then use the --probe-only installer above. The OPENCORE copy prepared for this laptop already includes that output, so rebuilding locally is optional. The probe will not enable Bluetooth.
