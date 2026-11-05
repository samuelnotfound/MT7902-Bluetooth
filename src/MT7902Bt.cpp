// Experimental MT7902 port: chip/firmware guard, no MT7925 register writes.
// USB transport adapted from WTLMtkBluetooth; original notices follow.
// <wtl> project: mtkbt | file: src/MT7902Bt.cpp | author: watchthelight </wtl>
// MT7902Bt: loads the Bluetooth RAM code into the MediaTek MT7925 (USB 0489:E111, interface 0 "BT_ACL_If") and then
// lets go of the interface, so bluetoothd (patched by BlueToolFixup) can drive it as a plain USB HCI controller.
//
// Procedure, ported from Linux drivers/bluetooth/btmtk.c v6.18 (Linux v6.18, ISC,
// Copyright (C) 2021 MediaTek Inc.): btmtk_usb_setup (0x7925 case), btmtk_setup_firmware_79xx,
// btmtk_usb_hci_wmt_sync, btmtk_usb_reg_read / uhw_reg_read / uhw_reg_write and btmtk_usb_subsys_reset.
//   * WMT command = HCI vendor command 0xFC6F sent as a class request on the default control endpoint.
//   * WMT event   = HCI event 0xE4 read back with a vendor IN request (bRequest 1, wValue 0x30), polled until data.
//   * per section: PATCH_DWNLD query with the 52-byte section spec (answer: download / already loaded / busy),
//     then the section in 250-byte blocks (flag 1 first, 2 middle, 3 last); 100 ms for activation.
//   * endpoint reset option 0x74011890 = 0x00010001, then WMT FUNC_CTRL 1 (Bluetooth on).
//
// Cross-checked against and partly adapted from MT7902BtFw.cpp of ga403wm-hackintosh
// (github.com/thanhnndev/ga403wm-hackintosh, kexts/MT7902BtFw/MT7902BtFw.cpp):
// the IOUSBHostInterface match on interface 0, the open / work / close pattern, the descriptor walk for the
// interrupt IN endpoint and the drain of that endpoint during setup. That code is under the licence below.
//
//   Copyright (c) 2026 thanhnndev and contributors
//   Portions Copyright (C) 2020, 2023 MediaTek Inc. (derived from the Linux mt76 driver)
//   All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without modification, are permitted (subject to
//   the limitations in the disclaimer below) provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright notice, this list of conditions and the
//       following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
//       following disclaimer in the documentation and/or other materials provided with the distribution.
//     * Neither the name of the copyright holder nor the names of its contributors may be used to endorse or
//       promote products derived from this software without specific prior written permission.
//   NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY THIS LICENSE. THIS SOFTWARE IS
//   PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
//   BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
//   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
//   INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
//   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
//   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//   ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Warm reboot (0.2). macOS never sends FUNC_CTRL 0 at shutdown, so after a restart the chip may still run the
// firmware of the previous boot. earlier boots on macOS 15 and 26 saw two forms of it: "section 0 (and 1): already loaded"
// followed by a query or a block that never gets its WMT event (section 2, block 55), with ~1000 interrupt packets
// of firmware chatter meanwhile. 0.2 handles it in three ways:
//   * a pass that fails after the chip already holds part of the firmware (a section reported loaded or sent in
//     this pass), or any failed block, counts as "half-loaded": the MT7925 subsystem reset of
//     btmtk_usb_subsys_reset runs and the whole download is repeated, at most kMaxResets times. Since 0.2.3 the
//     same goes for a download that "succeeds" by reusing sections from the previous boot but whose FUNC_CTRL then
//     gets no event (issue #1: sections 0 and 1 already loaded, section 2 sent, FUNC_CTRL silent);
//   * when the very first WMT query gets no event, an HCI Read Local Version is sent first; a Command Complete on
//     the interrupt endpoint means the firmware is up and the device is left as it is (as in 0.1);
//   * at system power off / restart (IOPMrootDomain priority interest) and in stop(), WMT FUNC_CTRL 0 is sent as
//     Linux btmtk_usb_shutdown does, so the next boot finds the function off and the WMT channel quiet.
// Linux follows the subsystem reset with a USB port reset (btusb_mtk_reset: usb_queue_reset_device); here the
// endpoint toggles of interface 0 are resynced with CLEAR_FEATURE(ENDPOINT_HALT) instead (IOUSBHostPipe::clearStall).
// That alone did not rule out a loop: on some boards the chip itself may drop off the bus during the reset, so 0.2.4
// also caps resets at kMaxResetsPerBoot across all instances.
//
// Every wait has a deadline, every failure ends in MT7902BtState = "error" with the interface closed. Nothing panics.
// Result: ioreg -rc MT7902Bt -l | grep MT7902Bt

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
// IOUSBHostInterface/IOUSBHostPipe are marked "Use USBDriverKit" in the SDK; they are still the KPI the 15.x kernel
// exports, and a dext cannot run before the kernel collection loads, so the deprecation is silenced here only.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <libkern/OSAtomic.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <stdarg.h>
#include <libkern/c++/OSBoolean.h>
#include <string.h>

extern "C" {
extern const unsigned int mtk_bt_fw_len;
extern const char mtk_bt_fw_sha256[];
extern const char mtk_bt_fw_name[];
extern const unsigned char mtk_bt_fw[];
}

namespace {
// btmtk.c: mt79xx rom patch layout
constexpr uint32_t kFwHdrSize = 32, kFwGdSize = 64, kFwSecMapSize = 64, kSecCommonSize = 12, kSecSendSize = 52;
constexpr uint32_t kMaxSections = 16;
constexpr uint32_t kBlock = 250;

// btmtk.h
enum : uint8_t { WMT_PATCH_DWNLD = 0x1, WMT_FUNC_CTRL = 0x6 };
constexpr uint8_t  kHciEvWmt = 0xE4;
constexpr uint32_t kRegChipId = 0x70010200;     // MTK_BT_READ_DEV_ID
constexpr uint32_t kRegFwVer = 0x80021004;
constexpr uint32_t kRegFwFlavor = 0x70010020;
constexpr uint32_t kBtMisc = 0x70002510;        // MTK_BT_MISC
constexpr uint32_t kBtRstDone = 0x00000100;     // MTK_BT_RST_DONE
constexpr uint32_t kBtSubsysRst = 0x70002610;
constexpr uint32_t kBtWdtStatus = 0x740003A0; //   // MTK_BT_RESET_REG_CONNV3
constexpr uint32_t kEpRstOpt = 0x74011890;      // MTK_EP_RST_OPT
constexpr uint32_t kEpRstInOut = 0x00010001;    // MTK_EP_RST_IN_OUT_OPT
constexpr uint32_t kUdmaIntStaBt = 0x74000024;  // MTK_UDMA_INT_STA_BT
constexpr uint32_t kUdmaIntStaBt1 = 0x74000308; // MTK_UDMA_INT_STA_BT1

// patch status, as btmtk_usb_hci_wmt_sync maps the PATCH_DWNLD event flag
enum { PATCH_UNDONE = 0, PATCH_PROGRESS = 1, PATCH_DONE = 2 };

// download() results. DL_NO_EVENT: the first query got no event at all. DL_HALF: the chip holds part of the
// firmware (a section already loaded or sent in this pass, or a block failed) and the pass still failed.
enum { DL_FAIL = 0, DL_OK = 1, DL_NO_EVENT = -2, DL_HALF = -3 };

constexpr uint32_t kCtrlTimeoutMs = 1000;       // per control transfer
constexpr uint32_t kWmtEventTimeoutMs = 2000;   // per WMT command (Linux waits up to HCI_INIT_TIMEOUT)
constexpr uint32_t kOpenTimeoutMs = 10000;
constexpr uint32_t kOffOpenTimeoutMs = 1000;    // FUNC_CTRL 0: bluetoothd has let go by then, or it is skipped
constexpr uint32_t kOffLockWaitMs = 5000;       // FUNC_CTRL 0: how long to wait for a worker that is still loading
constexpr uint32_t kMaxResets = 1;              // subsystem resets (each followed by a full download) per start
constexpr uint32_t kMaxResetsPerBoot = 1;       // the same, across every MT7902Bt instance since the kext loaded

// Shared by all instances for the life of the kext (one boot). If a subsystem reset makes the chip drop off the
// bus, it re-enumerates, a fresh MT7902Bt starts and would reset it again: a loop that kept the whole USB controller
// busy on issue #1 (keyboard and mouse frozen 3-5 s, working 1 s, over and over). The per-boot budget ends that.
static volatile SInt32 gResetsThisBoot = 0;
constexpr uint32_t kCmdBufSize = 512, kEvBufSize = 64, kIntrBufSize = 1024;

inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

uint64_t nowMs() {
    uint64_t t = 0, ns = 0;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000000ull;
}
} // namespace

class MT7902Bt : public IOService {
    OSDeclareDefaultStructors(MT7902Bt)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    bool willTerminate(IOService *provider, IOOptionBits options) override;
    void free() override;

private:
    IOUSBHostInterface *intf = nullptr;
    IOUSBHostPipe *intrPipe = nullptr;
    IOBufferMemoryDescriptor *intrBuf = nullptr;
    IOLock *lock = nullptr;            // one USB session at a time: the worker, or the FUNC_CTRL 0 at shutdown
    IONotifier *pmNotifier = nullptr;  // IOPMrootDomain priority interest (power off / restart)
    uint8_t *cmd = nullptr, *ev = nullptr;
    volatile bool stopping = false, intrStop = false, intrBusy = false, opened = false;
    volatile bool offPath = false;     // the FUNC_CTRL 0 session runs although stopping is set
    bool chipSeen = false, offDone = false, probeOnly = false;
    volatile uint32_t intrPackets = 0;
    // last HCI Command Complete seen on the interrupt endpoint (for the warm-reboot HCI probe)
    volatile uint16_t lastCcOpcode = 0;
    volatile uint8_t lastCcStatus = 0xFF;
    uint8_t intrAddr = 0, bulkIn = 0, bulkOut = 0;
    char log[1536] = {0};
    size_t loglen = 0;
    uint32_t bytesSent = 0, blocksSent = 0, sectionsSent = 0, sectionsSkipped = 0;

    static void worker(void *arg, wait_result_t);
    void run();
    bool setup();
    void vsay(bool error, const char *fmt, va_list ap);
    void say(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    void fail(const char *fmt, ...) __attribute__((format(printf, 2, 3)));   // say + MT7902BtLastError
    void state(const char *s) { setProperty("MT7902BtState", s); }
    void step(const char *s) { setProperty("MT7902BtStep", s); }
    bool aborted() const { return stopping && !offPath; }
    bool openInterface(uint32_t timeoutMs);
    bool beginSession(uint32_t timeoutMs);
    void endSession();
    void funcOff(const char *why);
    static IOReturn pmHandler(void *target, void *ref, UInt32 type, IOService *provider, void *arg, vm_size_t size);
    void findEndpoints();
    IOReturn ctrl(uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *buf, uint16_t len, uint32_t *done);
    bool regRead(uint32_t reg, uint32_t *val);
    bool uhwRead(uint32_t reg, uint32_t *val);
    bool uhwWrite(uint32_t reg, uint32_t val);
    int wmt(uint8_t op, uint8_t flag, const uint8_t *data, uint16_t dlen, uint8_t *evOut, uint32_t evOutLen);
    int download();
    bool hciAlive();
    bool subsysReset();
    int funcOn();
    void resyncEndpoints();
    void intrArm();
    static void intrDone(void *owner, void *param, IOReturn status, uint32_t bytes);
    void intrStopAndRelease();
};

OSDefineMetaClassAndStructors(MT7902Bt, IOService)

void MT7902Bt::say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsay(false, fmt, ap);
    va_end(ap);
}

// Errors also go to MT7902BtLastError. It is not cleared on a later success, so after a recovery it still says what
// set the recovery off.
void MT7902Bt::fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsay(true, fmt, ap);
    va_end(ap);
}

void MT7902Bt::vsay(bool error, const char *fmt, va_list ap) {
    char line[160];
    vsnprintf(line, sizeof(line), fmt, ap);
    IOLog("MT7902Bt: %s\n", line);
    if (error)
        setProperty("MT7902BtLastError", line);
    size_t n = strlen(line);
    if (loglen + n + 2 >= sizeof(log))   // keep the beginning, that is where things go wrong
        return;
    memcpy(log + loglen, line, n);
    loglen += n;
    log[loglen++] = ';';
    log[loglen] = 0;
    setProperty("MT7902BtLog", log);
}

IOReturn MT7902Bt::ctrl(uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *buf, uint16_t len, uint32_t *done) {
    if (aborted())
        return kIOReturnAborted;
    StandardUSB::DeviceRequest r;
    r.bmRequestType = type;
    r.bRequest = req;
    r.wValue = value;
    r.wIndex = index;
    r.wLength = len;
    uint32_t n = 0;
    IOReturn ret = intf->deviceRequest(r, len ? buf : nullptr, n, kCtrlTimeoutMs);
    if (done)
        *done = n;
    return ret;
}

// btmtk_usb_reg_read: vendor IN, bRequest 0x63
bool MT7902Bt::regRead(uint32_t reg, uint32_t *val) {
    uint32_t n = 0;
    IOReturn r = ctrl(0xC0, 0x63, (uint16_t)(reg >> 16), (uint16_t)(reg & 0xFFFF), ev, 4, &n);
    if (r != kIOReturnSuccess || n != 4) {
        fail("reg %08x read failed %08x n=%u", reg, r, n);
        return false;
    }
    *val = le32(ev);
    return true;
}

// btmtk_usb_uhw_reg_read: request type 0xDE, bRequest 0x01
bool MT7902Bt::uhwRead(uint32_t reg, uint32_t *val) {
    uint32_t n = 0;
    IOReturn r = ctrl(0xDE, 0x01, (uint16_t)(reg >> 16), (uint16_t)(reg & 0xFFFF), ev, 4, &n);
    if (r != kIOReturnSuccess || n != 4) {
        fail("uhw %08x read failed %08x n=%u", reg, r, n);
        return false;
    }
    *val = le32(ev);
    return true;
}

// btmtk_usb_uhw_reg_write: request type 0x5E, bRequest 0x02
bool MT7902Bt::uhwWrite(uint32_t reg, uint32_t val) {
    cmd[0] = (uint8_t)val;
    cmd[1] = (uint8_t)(val >> 8);
    cmd[2] = (uint8_t)(val >> 16);
    cmd[3] = (uint8_t)(val >> 24);
    IOReturn r = ctrl(0x5E, 0x02, (uint16_t)(reg >> 16), (uint16_t)(reg & 0xFFFF), cmd, 4, nullptr);
    if (r != kIOReturnSuccess) {
        fail("uhw %08x write failed %08x", reg, r);
        return false;
    }
    return true;
}

// btmtk_usb_hci_wmt_sync: one WMT command and its event. Returns the event length (>= 7) and copies the event to
// evOut, or -1 on a send failure, -2 when no event came before the deadline.
int MT7902Bt::wmt(uint8_t op, uint8_t flag, const uint8_t *data, uint16_t dlen, uint8_t *evOut, uint32_t evOutLen) {
    uint32_t hlen = 5u + dlen;   // struct btmtk_wmt_hdr + data
    if (hlen > 255)
        return -1;
    cmd[0] = 0x6F;               // HCI opcode 0xFC6F, little endian
    cmd[1] = 0xFC;
    cmd[2] = (uint8_t)hlen;
    cmd[3] = 1;                  // dir
    cmd[4] = op;
    cmd[5] = (uint8_t)(dlen + 1);
    cmd[6] = (uint8_t)((dlen + 1) >> 8);
    cmd[7] = flag;
    if (dlen)
        memcpy(cmd + 8, data, dlen);
    IOReturn r = ctrl(0x20, 0x00, 0, 0, cmd, (uint16_t)(3 + hlen), nullptr);   // class OUT to device, as btusb
    if (r != kIOReturnSuccess) {
        fail("wmt op %u send failed %08x", op, r);
        return -1;
    }
    uint64_t deadline = nowMs() + kWmtEventTimeoutMs;
    while (!aborted() && nowMs() < deadline) {
        uint32_t n = 0;
        r = ctrl(0xC0, 0x01, 0x30, 0, ev, kEvBufSize, &n);
        if (r == kIOReturnSuccess && n >= 7) {
            if (ev[0] != kHciEvWmt || ev[3] != op || ev[1] > n - 2 || le16(ev + 4) < 1 || le16(ev + 4) > n - 6) {   // struct btmtk_hci_wmt_evt: evt, plen, dir, op, dlen, flag
                say("wmt op %u: other event %02x op %u n=%u", op, ev[0], ev[3], n);
                continue;
            }
            if (evOut) {
                uint32_t c = n < evOutLen ? n : evOutLen;
                memcpy(evOut, ev, c);
            }
            return (int)n;
        }
        // Linux: "wait some time between two consecutive control URBs to relax the target device"
        IOSleep(1);
    }
    fail("wmt op %u flag %u: no event", op, flag);
    return -2;
}

// btmtk_setup_firmware_79xx. Returns DL_OK, DL_FAIL, DL_NO_EVENT when the first query got no event at all (the
// caller then checks for an already running firmware) or DL_HALF when the chip is left half-loaded (the caller
// then resets the subsystem and tries again). Linux has no such split: it fails setup and lets
// btusb_mtk_reset / the next hdev open sort it out.
int MT7902Bt::download() {
    const uint8_t *fw = mtk_bt_fw;
    const uint32_t fwlen = mtk_bt_fw_len;
    if (fwlen < kFwHdrSize + kFwGdSize) {
        fail("firmware blob too small");
        return DL_FAIL;
    }
    uint32_t nsec = le32(fw + kFwHdrSize + 12);   // btmtk_global_desc.section_num
    char date[17] = {0};
    memcpy(date, fw, 16);
    for (int i = 0; i < 16; i++)
        if (date[i] && (date[i] < 0x20 || date[i] > 0x7E))
            date[i] = '?';
    setProperty("MT7902BtFirmwareBuild", date);
    setProperty("MT7902BtFirmwareHwSw", ((uint32_t)le16(fw + 20) << 16) | le16(fw + 22), 32);
    setProperty("MT7902BtSections", nsec, 32);
    if (nsec == 0 || nsec > kMaxSections || kFwHdrSize + kFwGdSize + kFwSecMapSize * nsec > fwlen) {
        fail("bad section count %u", nsec);
        return DL_FAIL;
    }
    // Preflight every section before the first firmware write, not incrementally.
    const uint32_t tableEnd = kFwHdrSize + kFwGdSize + kFwSecMapSize * nsec;
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *m = fw + kFwHdrSize + kFwGdSize + kFwSecMapSize * i;
        uint32_t off = le32(m + 4), len = le32(m + 16);
        if (!len) continue;
        if (off < tableEnd || off > fwlen || len > fwlen - off) {
            fail("preflight: invalid firmware section %u", i); return DL_FAIL;
        }
        for (uint32_t j = 0; j < i; j++) {
            const uint8_t *prev = fw + kFwHdrSize + kFwGdSize + kFwSecMapSize * j;
            uint32_t po = le32(prev + 4), pl = le32(prev + 16);
            if (pl && off < po + pl && po < off + len) {
                fail("preflight: overlapping firmware sections"); return DL_FAIL;
            }
        }
    }
    bool firstQuery = true;
    // a failure from here on leaves a half-loaded chip when it already holds part of the firmware
    auto broken = [this]() { return (sectionsSkipped || sectionsSent) ? DL_HALF : DL_FAIL; };
    for (uint32_t i = 0; i < nsec && !stopping; i++) {
        const uint8_t *map = fw + kFwHdrSize + kFwGdSize + kFwSecMapSize * i;
        uint32_t off = le32(map + 4);            // secoffset
        uint32_t dl = le32(map + kSecCommonSize + 4);   // bin_info_spec.dlsize
        if (!dl)
            continue;
        if (off > fwlen || dl > fwlen - off) {
            fail("section %u out of range", i);
            return DL_FAIL;
        }
        uint8_t q[1 + kSecSendSize];
        q[0] = 0;                                // legacy download mode
        memcpy(q + 1, map + kSecCommonSize, kSecSendSize);
        int status = -1;
        for (int tries = 20; tries > 0 && !stopping; tries--) {
            uint8_t e[8] = {0};
            int n = wmt(WMT_PATCH_DWNLD, 0, q, sizeof(q), e, sizeof(e));
            if (n == -2 && firstQuery)
                return DL_NO_EVENT;
            firstQuery = false;
            if (n < 0)
                return stopping ? DL_FAIL : broken();
            if (e[6] > 2) { fail("unknown WMT patch status %u", e[6]); return broken(); }
            status = e[6];
            if (status != PATCH_PROGRESS)
                break;
            IOSleep(100);
        }
        if (stopping)
            return DL_FAIL;
        if (status == PATCH_PROGRESS) {
            fail("section %u: chip stays busy", i);
            return broken();
        }
        if (status == PATCH_DONE) {
            say("section %u: already loaded", i);
            sectionsSkipped++;
            continue;
        }
        if (status != PATCH_UNDONE)
            return broken();
        const uint8_t *p = fw + off;
        uint32_t left = dl, blocks = 0;
        bool first = true;
        while (left && !stopping) {
            uint16_t n = (uint16_t)(left > kBlock ? kBlock : left);
            uint8_t flag = first ? 1 : (left - n == 0 ? 3 : 2);
            first = false;
            if (wmt(WMT_PATCH_DWNLD, flag, p, n, nullptr, 0) < 0) {
                if (stopping)
                    return DL_FAIL;
                // the section is now partly in RAM whatever happened before: always half-loaded
                fail("section %u: block %u failed", i, blocks);
                return DL_HALF;
            }
            p += n;
            left -= n;
            blocks++;
            bytesSent += n;
            blocksSent++;
        }
        if (left)
            return DL_FAIL;
        sectionsSent++;
        say("section %u: %u bytes in %u blocks", i, dl, blocks);
        setProperty("MT7902BtBytesSent", bytesSent, 32);
    }
    if (stopping)
        return DL_FAIL;
    IOSleep(110);   // "Wait a few moments for firmware activation done" (usleep_range 100-120 ms)
    return DL_OK;
}

// Warm-reboot probe: HCI_Read_Local_Version_Information (0x1001). A running firmware answers with a Command
// Complete event on the interrupt endpoint, which intrDone records.
bool MT7902Bt::hciAlive() {
    if (!intrPipe)
        return false;
    lastCcOpcode = 0;
    lastCcStatus = 0xFF;
    cmd[0] = 0x01;
    cmd[1] = 0x10;
    cmd[2] = 0;
    if (ctrl(0x20, 0x00, 0, 0, cmd, 3, nullptr) != kIOReturnSuccess)
        return false;
    uint64_t deadline = nowMs() + 1000;
    while (!stopping && nowMs() < deadline) {
        if (lastCcOpcode == 0x1001)
            return lastCcStatus == 0;
        IOSleep(10);
    }
    return false;
}

// Linux btmtk_usb_subsys_reset(): default 79xx branch used for MT7902 in
// the pinned upstream tree (not the MT7925 CONNV3 path). Explicit opt-in only.
bool MT7902Bt::subsysReset() {
    if (getProperty("MT7902AllowReset") != kOSBooleanTrue || !chipSeen)
        return false;
    uint32_t v = 0;
    if (!uhwWrite(kEpRstOpt, kEpRstInOut) || !uhwRead(kBtWdtStatus, &v) ||
        !uhwWrite(kBtSubsysRst, 1) || !uhwWrite(kUdmaIntStaBt, 0xFF) ||
        !uhwRead(kUdmaIntStaBt, &v) || !uhwWrite(kUdmaIntStaBt1, 0xFF) ||
        !uhwRead(kUdmaIntStaBt1, &v))
        return false;
    IOSleep(20);
    if (!uhwWrite(kBtSubsysRst, 0) || !uhwRead(kBtSubsysRst, &v))
        return false;
    bool done = false;
    uint64_t deadline = nowMs() + 1000;
    while (!stopping && nowMs() < deadline) {
        if (uhwRead(kBtMisc, &v) && (v & kBtRstDone)) { done = true; break; }
        IOSleep(20);
    }
    uint32_t id = 0;
    if (!done || !regRead(kRegChipId, &id) || (id & 0xFFFF) != 0x7902) {
        fail("MT7902 reset did not complete or chip ID changed");
        return false;
    }
    return true;
}

// After the subsystem reset the chip's endpoints restart at DATA0 (EP reset option); bring the host side in step.
void MT7902Bt::resyncEndpoints() {
    uint8_t addrs[3] = {intrAddr, bulkIn, bulkOut};
    for (uint8_t a : addrs) {
        if (!a)
            continue;
        if (a == intrAddr && intrPipe) {
            intrPipe->abort(IOUSBHostIOSource::kAbortSynchronous, kIOReturnAborted, this);
            for (int i = 0; i < 100 && intrBusy; i++)
                IOSleep(10);
            IOReturn r = intrPipe->clearStall(true);
            say("ep %02x resync %08x", a, r);
            intrArm();
            continue;
        }
        IOUSBHostPipe *p = intf->copyPipe(a);
        if (p) {
            IOReturn r = p->clearStall(true);
            say("ep %02x resync %08x", a, r);
            p->release();
        }
    }
}

void MT7902Bt::intrDone(void *owner, void *, IOReturn status, uint32_t bytes) {
    MT7902Bt *me = static_cast<MT7902Bt *>(owner);
    if (status == kIOReturnSuccess && me->intrBuf) {
        me->intrPackets = me->intrPackets + 1;
        const uint8_t *b = static_cast<const uint8_t *>(me->intrBuf->getBytesNoCopy());
        // HCI Command Complete: 0x0E, plen, num_cmds, opcode (le16), status
        if (b && bytes >= 6 && b[0] == 0x0E) {
            me->lastCcStatus = b[5];
            me->lastCcOpcode = le16(b + 3);
        }
    }
    me->intrBusy = false;
    if (!me->intrStop && status != kIOReturnAborted && status != kIOReturnNotOpen && status != kIOReturnNoDevice)
        me->intrArm();
}

// HCI events the chip queues on the interrupt endpoint during setup are read and dropped (Linux has its interrupt
// URB running then too), so they cannot back up while the control endpoint is polled.
void MT7902Bt::intrArm() {
    if (!intrPipe || !intrBuf || intrStop || aborted())
        return;
    IOUSBHostCompletion c = {this, intrDone, nullptr};
    intrBusy = true;
    if (intrPipe->io(intrBuf, (uint32_t)intrBuf->getLength(), &c, 0) != kIOReturnSuccess)
        intrBusy = false;
}

void MT7902Bt::intrStopAndRelease() {
    intrStop = true;
    if (intrPipe) {
        intrPipe->abort(IOUSBHostIOSource::kAbortSynchronous, kIOReturnAborted, this);
        for (int i = 0; i < 100 && intrBusy; i++)
            IOSleep(10);
        intrPipe->release();
        intrPipe = nullptr;
    }
}

// Interface 0 endpoints from the configuration descriptor (bLength / bDescriptorType walk): interrupt IN for
// events, bulk IN/OUT for ACL. Only the interrupt pipe is used here; the bulk ones are only resynced after a reset.
void MT7902Bt::findEndpoints() {
    const uint8_t *cfg = reinterpret_cast<const uint8_t *>(intf->getConfigurationDescriptor());
    const uint8_t *idesc = reinterpret_cast<const uint8_t *>(intf->getInterfaceDescriptor());
    if (!cfg || !idesc)
        return;
    uint32_t total = (uint32_t)cfg[2] | (uint32_t)cfg[3] << 8, off = 0;
    bool mine = false;
    while (off + 2 <= total && cfg[off] >= 2 && off + cfg[off] <= total) {
        const uint8_t *d = cfg + off;
        if (d[1] == 4 && d[0] >= 9) {
            mine = d[2] == idesc[2] && d[3] == idesc[3];   // bInterfaceNumber, bAlternateSetting
        } else if (d[1] == 5 && d[0] >= 7 && mine) {
            uint8_t type = d[3] & 3;
            if (type == 3 && (d[2] & 0x80) && !intrAddr)
                intrAddr = d[2];
            else if (type == 2 && (d[2] & 0x80) && !bulkIn)
                bulkIn = d[2];
            else if (type == 2 && !(d[2] & 0x80) && !bulkOut)
                bulkOut = d[2];
        }
        off += d[0];
    }
    char eps[48];
    snprintf(eps, sizeof(eps), "intr-in %02x bulk-in %02x bulk-out %02x", intrAddr, bulkIn, bulkOut);
    setProperty("MT7902BtEndpoints", eps);
}

bool MT7902Bt::openInterface(uint32_t timeoutMs) {
    uint64_t deadline = nowMs() + timeoutMs;
    uint32_t tries = 0;
    while (!aborted()) {
        tries++;
        if (intf->open(this)) {
            opened = true;
            setProperty("MT7902BtOpenTries", tries, 32);
            return true;
        }
        if (nowMs() >= deadline)
            break;
        IOSleep(200);
    }
    setProperty("MT7902BtOpenTries", tries, 32);
    return false;
}

// Open interface 0 and start the interrupt drain. The caller holds lock.
bool MT7902Bt::beginSession(uint32_t timeoutMs) {
    if (!openInterface(timeoutMs))
        return false;
    if (!intrAddr)
        findEndpoints();
    if (intrAddr) {
        intrPipe = intf->copyPipe(intrAddr);
        if (!intrBuf)
            intrBuf = intf->createIOBuffer(kIODirectionIn, kIntrBufSize);
        intrStop = false;
        intrArm();
    }
    return true;
}

void MT7902Bt::endSession() {
    intrStopAndRelease();
    intf->close(this);
    opened = false;
}

// btmtk_usb_setup for the 0x7902, with the half-loaded recovery around btmtk_setup_firmware_79xx. Returns true
// when Bluetooth is on (or was already).
bool MT7902Bt::setup() {
    uint32_t devId = 0, fwVer = 0, flavor = 0;
    step("chip id");
    if (!regRead(kRegChipId, &devId))
        return false;
    if (!regRead(kRegFwVer, &fwVer)) return false;
    regRead(kRegFwFlavor, &flavor);
    setProperty("MT7902BtChipId", devId, 32);
    setProperty("MT7902BtFwVersion", fwVer, 32);
    setProperty("MT7902BtFwFlavor", (flavor & 0x80) >> 7, 32);
    say("chip %04x fw_ver %08x flavor %u", devId & 0xFFFF, fwVer, (flavor & 0x80) >> 7);
    if ((devId & 0xFFFF) != 0x7902) {
        fail("not an MT7902, refusing firmware writes");
        return false;
    }
    if (probeOnly) {
        say("probe only: chip identified; no firmware/register writes performed");
        return true;
    }
    // Linux btmtk_fw_get_filename(): MT7902_1_(low fwVersion + 1).
    if ((fwVer & 0xFF) != 0) {
        fail("chip requires MT7902 firmware revision %u; only revision 1 embedded", (fwVer & 0xFF) + 1);
        return false;
    }
    chipSeen = true;

    // pass 1 is the normal download; passes 2 .. kMaxResets + 1 each follow a subsystem reset
    for (uint32_t pass = 1;; pass++) {
        setProperty("MT7902BtTries", pass, 32);
        sectionsSent = sectionsSkipped = 0;
        step("download");
        int d = download();
        setProperty("MT7902BtSectionsSent", sectionsSent, 32);
        setProperty("MT7902BtSectionsSkipped", sectionsSkipped, 32);
        if (d == DL_OK) {
            int f = funcOn();
            if (f >= 0)
                return true;
            // FUNC_CTRL got no event after a pass that reused sections left over from the previous boot: the
            // chip holds a mix of old and new firmware and never starts it (issue #1, B850 desktop, 0.2.2).
            // Treat it like a half-loaded chip: reset the subsystem and download everything again.
            if (stopping || f != -2 || !sectionsSkipped)
                return false;
            say("func ctrl silent after reusing %u loaded sections", sectionsSkipped);
            d = DL_HALF;
        }
        if (stopping || (d != DL_NO_EVENT && d != DL_HALF))
            return false;
        if (d == DL_NO_EVENT && pass == 1) {
            step("hci probe");
            if (hciAlive()) {
                setProperty("MT7902BtWarmPath", "firmware already running (HCI answers)");
                say("no WMT event but HCI answers: firmware already running, left as is");
                return true;
            }
        }
        if (getProperty("MT7902AllowReset") != kOSBooleanTrue) {
            setProperty("MT7902BtWarmPath", "reset disabled; cold boot required");
            fail("firmware setup incomplete; automatic reset disabled (fully power off before retry)");
            return false;
        }
        if (pass > kMaxResets) {
            fail("download still failing after %u subsys resets, giving up", kMaxResets);
            return false;
        }
        if (OSIncrementAtomic(&gResetsThisBoot) >= (SInt32)kMaxResetsPerBoot) {
            setProperty("MT7902BtWarmPath", "reset budget for this boot used up, giving up");
            fail("%u subsys resets already done this boot, not resetting again (power off fully to recover)",
                 kMaxResetsPerBoot);
            return false;
        }
        const char *why = d == DL_HALF ? "half-loaded" : "no WMT event";
        say("pass %u: %s, subsys reset", pass, why);
        setProperty("MT7902BtWarmPath", d == DL_HALF ? "half-loaded, subsys reset" : "no WMT event, subsys reset");
        step("subsys reset");
        bool ok = subsysReset();
        setProperty("MT7902BtSubsysReset", ok ? "done" : "failed");
        say("subsys reset %s", ok ? "done" : "failed");
        if (!ok)
            return false;
        resyncEndpoints();
    }
}

// After download: endpoint reset option, WMT FUNC_CTRL 1 (Linux MT7902 path). Returns 0 when Bluetooth
// is on, -2 when FUNC_CTRL got no event, -1 on any other failure.
int MT7902Bt::funcOn() {
    step("ep reset option");
    if (!uhwWrite(kEpRstOpt, kEpRstInOut))
        return -1;

    step("func ctrl");
    uint8_t on = 1, e[16] = {0};
    int n = wmt(WMT_FUNC_CTRL, 0, &on, 1, e, sizeof(e));
    if (n < 0)
        return n == -2 ? -2 : -1;
    // struct btmtk_hci_wmt_evt_funcc: __be16 status after the 7-byte header; 0x404 = ON_DONE, 0x420 = ON_PROGRESS
    uint32_t st = n >= 9 ? ((uint32_t)e[7] << 8 | e[8]) : 0xFFFF;
    setProperty("MT7902BtFuncCtrlStatus", st, 32);
    say("bluetooth function on (flag %u status %04x)", e[6], st);
    return 0;
}

// btmtk_usb_shutdown: WMT FUNC_CTRL with param 0 ("Disable the device"), status ignored as in Linux. Sent once, at
// system power off / restart or in stop(), only when a 7925 was seen and nobody else (bluetoothd) has the
// interface open; by the time IOPMrootDomain calls its priority clients for a halt, launchd has already ended
// bluetoothd, so the interface is normally free. Best effort: every failure is logged and skipped.
void MT7902Bt::funcOff(const char *why) {
    if (!lock || !intf || !chipSeen || offDone)
        return;
    // stopping is already set, so a worker that is still loading gives up at its next transfer and lets go
    uint64_t deadline = nowMs() + kOffLockWaitMs;
    while (!IOLockTryLock(lock)) {
        if (nowMs() >= deadline) {
            fail("%s: worker still busy, func ctrl 0 skipped", why);
            return;
        }
        IOSleep(10);
    }
    offDone = true;
    offPath = true;
    if (intf->isInactive() || !beginSession(kOffOpenTimeoutMs)) {
        say("%s: interface not free, func ctrl 0 skipped", why);
    } else {
        uint8_t off = 0, e[16] = {0};
        int n = wmt(WMT_FUNC_CTRL, 0, &off, 1, e, sizeof(e));
        endSession();
        if (n < 0) {
            fail("%s: func ctrl 0 failed", why);
            setProperty("MT7902BtFuncOff", "failed");
        } else {
            uint32_t st = n >= 9 ? ((uint32_t)e[7] << 8 | e[8]) : 0xFFFF;
            say("%s: bluetooth function off (status %04x)", why, st);
            setProperty("MT7902BtFuncOff", why);
            state("off");
        }
    }
    offPath = false;
    IOLockUnlock(lock);
}

// IOPMrootDomain priority interest. For a halt or restart, handlePlatformHaltRestart delivers
// kIOMessageSystemWillPowerOff / kIOMessageSystemWillRestart to these clients (platformHaltRestartApplier) before
// the power-tree shutdown, so the USB stack still runs. Sleep messages are acknowledged at once (no sleep support).
IOReturn MT7902Bt::pmHandler(void *target, void *, UInt32 type, IOService *, void *, vm_size_t) {
    MT7902Bt *me = static_cast<MT7902Bt *>(target);
    if (me && (type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart)) {
        me->stopping = true;
        me->funcOff(type == kIOMessageSystemWillRestart ? "restart" : "power off");
    }
    return kIOReturnSuccess;
}

void MT7902Bt::run() {
    IOLockLock(lock);
    uint64_t t0 = nowMs();
    state("loading");
    step("open");
    if (stopping || !beginSession(kOpenTimeoutMs)) {
        if (!stopping)
            fail("interface busy (open failed)");
        state(stopping ? "stopped" : "error");
        IOLockUnlock(lock);
        return;
    }
    bool ok = setup();
    step("close");
    endSession();
    setProperty("MT7902BtIntrPackets", intrPackets, 32);
    setProperty("MT7902BtBlocksSent", blocksSent, 32);
    uint64_t ms = nowMs() - t0;
    setProperty("MT7902BtElapsedMs", ms, 32);
    say("%s in %llu ms, %u interrupt packets dropped", ok ? (probeOnly ? "probe-only" : "ready") : "failed", ms, intrPackets);
    step("done");
    state(stopping ? "stopped" : ok ? (probeOnly ? "probe-only" : "ready") : "error");
    IOLockUnlock(lock);
}

void MT7902Bt::worker(void *arg, wait_result_t) {
    MT7902Bt *me = static_cast<MT7902Bt *>(arg);
    me->run();
    me->release();
}

bool MT7902Bt::start(IOService *provider) {
    if (!IOService::start(provider))
        return false;
    intf = OSDynamicCast(IOUSBHostInterface, provider);
    if (!intf)
        return false;
    const StandardUSB::InterfaceDescriptor *id = intf->getInterfaceDescriptor();
    if (!id || id->bInterfaceNumber != 0)
        return false;
    cmd = static_cast<uint8_t *>(IOMallocZero(kCmdBufSize));
    ev = static_cast<uint8_t *>(IOMallocZero(kEvBufSize));
    lock = IOLockAlloc();
    if (!cmd || !ev || !lock)
        return false;
    probeOnly = getProperty("MT7902ProbeOnly") == kOSBooleanTrue;
    setProperty("MT7902BtVersion", "0.1.0");
    setProperty("MT7902BtProbeOnly", probeOnly ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("MT7902BtFirmware", mtk_bt_fw_name);
    setProperty("MT7902BtFirmwareSha256", mtk_bt_fw_sha256);
    setProperty("MT7902BtFirmwareBytes", mtk_bt_fw_len, 32);
    setProperty("MT7902BtTries", 0ull, 32);
    state("starting");
    pmNotifier = registerPrioritySleepWakeInterest(&MT7902Bt::pmHandler, this, nullptr);
    if (!pmNotifier)
        say("no power off notification, func ctrl 0 only in stop()");
    registerService();
    retain();   // released by the worker
    thread_t th = nullptr;
    if (kernel_thread_start(&MT7902Bt::worker, this, &th) != KERN_SUCCESS) {
        release();
        state("error");
        fail("worker thread failed");
        return true;   // stay attached and inert; the interface stays free for bluetoothd
    }
    thread_deallocate(th);
    return true;
}

bool MT7902Bt::willTerminate(IOService *provider, IOOptionBits options) {
    // the worker sees this between steps; every control transfer is aborted by close() in run()
    stopping = true;
    intrStop = true;
    return IOService::willTerminate(provider, options);
}

void MT7902Bt::stop(IOService *provider) {
    stopping = true;
    intrStop = true;
    if (pmNotifier) {
        pmNotifier->remove();   // waits for a handler that is running
        pmNotifier = nullptr;
    }
    // kextunload with the device still there: turn the function off like btmtk_usb_shutdown. When the provider is
    // going away (unplug, re-enumeration) it is inactive and funcOff skips.
    funcOff("stop");
    IOService::stop(provider);
}

void MT7902Bt::free() {
    OSSafeReleaseNULL(intrBuf);
    if (cmd)
        IOFree(cmd, kCmdBufSize);
    if (ev)
        IOFree(ev, kEvBufSize);
    if (lock)
        IOLockFree(lock);
    IOService::free();
}

#pragma clang diagnostic pop
