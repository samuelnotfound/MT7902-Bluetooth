#!/bin/bash
# Build only. Native macOS Command Line Tools are required.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $(uname -s) == Darwin ]] || { echo "Build on macOS with the Command Line Tools." >&2; exit 1; }
python3 -I -m unittest discover -s tests -v
VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' Info.plist)
SDK=$(xcrun --sdk macosx --show-sdk-path)
K="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[[ -d "$K/IOKit" ]] || { echo "Kernel.framework headers missing from SDK: $SDK" >&2; exit 1; }
mkdir -p out/obj out/gen
OUT=out/MT7902Bt.kext/Contents
mkdir -p "$OUT/MacOS"
python3 -I tools/gen_firmware.py out/gen/MT7902Firmware.c
FLAGS=(-arch x86_64 -mmacosx-version-min=10.15 -mkernel -nostdinc -fno-builtin -fno-common -fno-stack-protector
 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -isystem "$(xcrun clang -print-resource-dir)/include" -I"$K" -O2 -Wall -Wno-unused-parameter)
xcrun clang++ "${FLAGS[@]}" -fapple-kext -std=gnu++17 -nostdinc++ -fno-exceptions -fno-rtti -fno-use-cxa-atexit -Wno-inconsistent-missing-override -c src/MT7902Bt.cpp -o out/obj/MT7902Bt.o
xcrun clang "${FLAGS[@]}" -std=gnu11 -c out/gen/MT7902Firmware.c -o out/obj/MT7902Firmware.o
xcrun clang "${FLAGS[@]}" -std=gnu11 -DKEXT_ID=local.experimental.MT7902Bluetooth "-DKEXT_VERSION=\"$VERSION\"" -c src/kmod_info.c -o out/obj/kmod_info.o
xcrun clang++ -arch x86_64 -mmacosx-version-min=10.15 -nostdlib -Xlinker -kext -Xlinker -export_dynamic out/obj/MT7902Bt.o out/obj/MT7902Firmware.o out/obj/kmod_info.o -L"$SDK/usr/lib" -lkmodc++ -lkmod -lcc_kext -o "$OUT/MacOS/MT7902Bt"
cp Info.plist "$OUT/Info.plist"
xcrun otool -hv "$OUT/MacOS/MT7902Bt" | grep -q KEXTBUNDLE
xcrun nm "$OUT/MacOS/MT7902Bt" > out/symbols.txt
grep -q ' _kmod_info' out/symbols.txt
if xcrun nm -u "$OUT/MacOS/MT7902Bt" | grep -Eq '[[:space:]]_{2,3}(cxa_|Unwind|gxx_)'; then echo "Unexpected C++ runtime dependency" >&2; exit 1; fi
codesign --force --sign - out/MT7902Bt.kext
codesign --verify --strict out/MT7902Bt.kext
shasum -a 256 "$OUT/MacOS/MT7902Bt" > out/SHA256SUMS
printf 'Built %s. Default mode is PROBE ONLY; no EFI files were modified.\n' "out/MT7902Bt.kext"
