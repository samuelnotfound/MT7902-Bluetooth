#!/bin/bash
set -u
cd "$(dirname "$0")/.."
[[ $(uname -s) == Darwin ]] || exit 1
OUT="reports/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
sudo dmesg > "$OUT/dmesg.txt" 2>&1
ioreg -rc MT7902Bt -l -w0 > "$OUT/driver.txt" 2>&1
ioreg -p IOUSB -l -w0 > "$OUT/usb.txt" 2>&1
kmutil showloaded > "$OUT/kexts.txt" 2>&1
system_profiler SPBluetoothDataType > "$OUT/bluetooth.txt" 2>&1
nvram -p | grep -i bluetooth > "$OUT/nvram-bluetooth.txt"
cat "$OUT/driver.txt"
echo "Saved $OUT"
