# MT7902 port: notices

This is an experimental derivative of watchthelight-labs/WTLMtkBluetooth, not an upstream-supported MT7902 release. Exact upstream revision and Linux reference hashes are in UPSTREAM.json. Original upstream notices remain in docs/NOTICE.upstream.md and src/MT7902Bt.cpp; ISC and Clear BSD license files are retained.

MT7902 changes follow Linux drivers/bluetooth/btmtk.c and btmtk.h (ISC, MediaTek copyright notices retained in docs/linux-reference). The default MT7902 reset route follows the pinned mainline reference, rather than a downstream patch that uses the MT7922 reset path. Automatic reset is disabled. MT7925 sleep-protection/CONNV3 writes were removed from the compiled source.

Firmware: BT_RAM_CODE_MT7902_1_1_hdr.bin, 509320 bytes, build 20250826211444, SHA-256 4f53b5e02fbd933172e18caf952bda410a877eaad00561781528cb4aff58dc38. Downloaded unchanged from https://gitlab.com/kernel-firmware/linux-firmware/-/raw/main/mediatek/BT_RAM_CODE_MT7902_1_1_hdr.bin. Redistributed solely under LICENSES/LICENCE.mediatek for use with MediaTek hardware. The MT7925 firmware is not included in this port.

Original MT7902 modifications and local tools are ISC, under the terms in LICENSE. The upstream copyright notice remains intact. This project is not affiliated with MediaTek, Apple or watchthelight-labs.
