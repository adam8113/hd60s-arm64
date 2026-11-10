// HD60 S capture on Apple Silicon: open the card, run the cold init, then pull
// the isochronous stream off endpoint 0x83.
//
// Each phase is its own subcommand, so a first run against real hardware can be
// kept read-only.
//
// Two things to know before changing any of this:
//
//   wIndex is not always zero. It is zero for the I2C targets, but for the FX3
//   direct port (wValue 0x004C) it carries the register number.
//
//   The isochronous request size has to come from GetPipePropertiesV2, which
//   reports bytesPerInterval: 32768 at SuperSpeed alt 2. GetPipeProperties
//   reports wMaxPacketSize instead, which is 1024, and reading that many bytes
//   per microframe loses most of the stream.
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include "init_regs.h"
#include "vendor_regs.h"
#include <pthread.h>
#include <csignal>

#include "hd60s_ids.h"

#define T_MCU   0x5066      // Nuvoton MCU, I2C 0x66
#define T_VIDEO 0x509c      // video front end, I2C 0x9c
#define T_PORT  0x004c      // FX3 direct port; register goes in wIndex

static IOUSBDeviceInterface500**    gDev = nullptr;
static IOUSBInterfaceInterface500** gIf  = nullptr;
static int  gVerbose = 0;
static bool gPollWanted = false;
static bool gRmw = false;
static bool gVfix = false;
static bool gNoMatrix = false;
static bool gNoEdid = false, gNoTx = false, gNoCtl = false;
static int gLimit = -1;
static bool gNoP80 = false;
// Neither is needed once the card has been set up by `replay`, and both
// black out the HDMI passthrough, so they are opt-in.
static bool gArm = false, gPost = false;

// ---------------------------------------------------------------- transport

static const char* ioerr(IOReturn r) {
    switch (r) {
        case kIOReturnSuccess:          return "ok";
        case kIOUSBPipeStalled:         return "STALL (device rejected it)";
        case kIOUSBTransactionTimeout:  return "timeout";
        case kIOReturnNotResponding:    return "not responding";
        case kIOReturnNoDevice:         return "no device";
        case kIOReturnNotOpen:          return "not open";
        default:                        return "?";
    }
}

static IOReturn ctl(uint8_t bReq, uint16_t wValue, uint16_t wIndex,
                    void* buf, uint16_t len, bool in, UInt32 ms = 250) {
    IOUSBDevRequestTO q; memset(&q, 0, sizeof q);
    q.bmRequestType = USBmakebmRequestType(in ? kUSBIn : kUSBOut, kUSBVendor, kUSBDevice);
    q.bRequest = bReq; q.wValue = wValue; q.wIndex = wIndex;
    q.wLength = len; q.pData = buf;
    q.noDataTimeout = ms; q.completionTimeout = ms;
    IOReturn r = (*gDev)->DeviceRequestTO(gDev, &q);
    // A stalled or timed-out setup leaves endpoint 0 halted; every later
    // request then comes back "not responding" and the log becomes noise.
    if (r != kIOReturnSuccess) (*gDev)->USBDeviceAbortPipeZero(gDev);
    if (gVerbose) {
        printf("      %s bReq=0x%02x wV=0x%04x wI=0x%04x len=%u ->", in?"IN ":"OUT",
               bReq, wValue, wIndex, len);
        if (r != kIOReturnSuccess) printf(" 0x%08x %s\n", r, ioerr(r));
        else { const uint8_t* p=(const uint8_t*)buf;
               for (uint16_t i=0;i<len && i<8;i++) printf(" %02x", p[i]);
               printf("%s\n", len?"":" ok"); }
    }
    return r;
}

// MCU's own registers: 0x9a writes, 0x9b reads.
static int mcuRead(uint8_t reg) {
    uint8_t k[3] = { 0x9b, 0x01, reg };
    if (ctl(0xC0, T_MCU, 0, k, 3, false) != kIOReturnSuccess) return -1;
    uint8_t v = 0;
    if (ctl(0xC0, T_MCU, 0, &v, 1, true) != kIOReturnSuccess) return -1;
    return v;
}
__attribute__((unused))
static IOReturn mcuWrite(uint8_t reg, uint8_t val) {
    uint8_t k[3] = { 0x9a, reg, val };
    return ctl(0xC0, T_MCU, 0, k, 3, false);
}

// Video front end. Register 0x00 on the chip is the page select. The MCU acts
// as the I2C master: 0x9d proxies a read of the video chip's current page.
static IOReturn vidPage(uint8_t page) {
    uint8_t p[2] = { 0x00, page };
    return ctl(0xC0, T_VIDEO, 0, p, 2, false);
}
static IOReturn vidWrite(uint8_t page, uint8_t reg, uint8_t val) {
    if (vidPage(page) != kIOReturnSuccess) return kIOReturnError;
    uint8_t p[2] = { reg, val };
    return ctl(0xC0, T_VIDEO, 0, p, 2, false);
}
static int vidRead(uint8_t page, uint8_t reg) {
    if (vidPage(page) != kIOReturnSuccess) return -1;
    uint8_t k[3] = { 0x9d, 0x01, reg };
    if (ctl(0xC0, T_MCU, 0, k, 3, false) != kIOReturnSuccess) return -1;
    uint8_t v = 0;
    if (ctl(0xC0, T_MCU, 0, &v, 1, true) != kIOReturnSuccess) return -1;
    return v;
}

// FX3 direct port. This is the one place wIndex is not zero.
static int portRead(uint8_t reg) {
    uint8_t v = 0;
    if (ctl(0xC0, T_PORT, reg, &v, 1, true) != kIOReturnSuccess) return -1;
    return v;
}
static IOReturn portWrite(uint8_t reg, uint8_t val) {
    uint8_t v = val;
    return ctl(0xC0, T_PORT, reg, &v, 1, false);
}
static int gpioRead(uint16_t sel) {
    uint8_t v = 0;
    if (ctl(0xC1, sel, 0, &v, 1, true) != kIOReturnSuccess) return -1;
    return v;
}

// ---------------------------------------------------------------- discovery

static bool openDevice() {
    io_iterator_t it; io_service_t s;
    IOServiceGetMatchingServices(kIOMainPortDefault,
                                 IOServiceMatching(kIOUSBDeviceClassName), &it);
    while ((s = IOIteratorNext(it))) {
        IOCFPlugInInterface** pl = nullptr; SInt32 sc;
        if (IOCreatePlugInInterfaceForService(s, kIOUSBDeviceUserClientTypeID,
                kIOCFPlugInInterfaceID, &pl, &sc) == KERN_SUCCESS && pl) {
            IOUSBDeviceInterface500** d = nullptr;
            (*pl)->QueryInterface(pl, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500),
                                  (LPVOID*)&d);
            (*pl)->Release(pl);
            if (d) { UInt16 v, p;
                (*d)->GetDeviceVendor(d, &v); (*d)->GetDeviceProduct(d, &p);
                if (isHD60S(v, p)) { gDev = d; IOObjectRelease(s); break; }
                (*d)->Release(d); }
        }
        IOObjectRelease(s);
    }
    IOObjectRelease(it);
    if (!gDev) { printf("  HD60 S not found. Plug it in and try again.\n"); return false; }
    (*gDev)->USBDeviceOpen(gDev);
    UInt8 c = 0; (*gDev)->GetConfiguration(gDev, &c);
    if (c != 1) {
        IOReturn sc = (*gDev)->SetConfiguration(gDev, 1);
        printf("  SetConfiguration(1) -> 0x%08x%s\n", sc,
               sc == kIOReturnSuccess ? "" : "  FAILED");
        (*gDev)->GetConfiguration(gDev, &c);
    }
    UInt8 speed = 0; (*gDev)->GetDeviceSpeed(gDev, &speed);
    UInt8 nc = 0; (*gDev)->GetNumberOfConfigurations(gDev, &nc);
    // IOKit: 0 low, 1 full, 2 high, 3 super, 4 super+
    static const char* sn[] = { "low", "full", "high (USB 2.0 link)", "SuperSpeed", "SuperSpeed+" };
    printf("  device open, config %u (of %u), speed %s\n", c, nc,
           speed < 5 ? sn[speed] : "?");
    if (speed < 3)
        printf("  WARNING: not a SuperSpeed link. Alt 2 does not exist at USB 2.0.\n");
    return true;
}

static bool claimInterface0() {
    IOUSBFindInterfaceRequest rq = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                     kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    io_iterator_t ii; (*gDev)->CreateInterfaceIterator(gDev, &rq, &ii);
    io_service_t is;
    while ((is = IOIteratorNext(ii))) {
        IOCFPlugInInterface** pl = nullptr; SInt32 sc;
        IOUSBInterfaceInterface500** f = nullptr;
        if (IOCreatePlugInInterfaceForService(is, kIOUSBInterfaceUserClientTypeID,
                kIOCFPlugInInterfaceID, &pl, &sc) == KERN_SUCCESS && pl) {
            (*pl)->QueryInterface(pl, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500),
                                  (LPVOID*)&f);
            (*pl)->Release(pl);
        }
        IOObjectRelease(is);
        if (!f) continue;
        UInt8 n = 0; (*f)->GetInterfaceNumber(f, &n);
        if (n != 0) { (*f)->Release(f); continue; }
        gIf = f; break;
    }
    IOObjectRelease(ii);
    if (!gIf) { printf("  interface 0 not found\n"); return false; }
    IOReturn r = (*gIf)->USBInterfaceOpen(gIf);
    if (r != kIOReturnSuccess) { printf("  USBInterfaceOpen failed 0x%08x\n", r); return false; }
    UInt8 curAlt = 0xff; (*gIf)->GetAlternateSetting(gIf, &curAlt);
    UInt8 nEp = 0; (*gIf)->GetNumEndpoints(gIf, &nEp);
    printf("  interface 0 open, current alt %u, %u endpoint(s)\n", curAlt, nEp);
    return true;
}

// ------------------------------------------------------------------- phases

// Read-only. Touches no write path at all.
static int phaseProbe() {
    // Work up from the request that cannot fail to the one we need, so a
    // timeout says which layer is dead rather than just "no answer".
    printf("\n-- control endpoint --\n");
    { uint8_t b[18]; IOUSBDevRequestTO q; memset(&q, 0, sizeof q);
      q.bmRequestType = USBmakebmRequestType(kUSBIn, kUSBStandard, kUSBDevice);
      q.bRequest = kUSBRqGetDescriptor; q.wValue = 0x0100; q.wLength = 18; q.pData = b;
      q.noDataTimeout = 250; q.completionTimeout = 250;
      IOReturn r = (*gDev)->DeviceRequestTO(gDev, &q);
      printf("  standard GET_DESCRIPTOR : %-28s %u bytes\n", ioerr(r), (unsigned)q.wLenDone); }

    int g39 = gpioRead(0x0039);
    printf("  vendor 0xC1 gpio 0x39   : %s%02x\n", g39 < 0 ? "TIMEOUT (" : "0x", g39 < 0 ? 0 : g39);

    { uint8_t k[3] = { 0x9b, 0x01, 0x0e };
      IOReturn r = ctl(0xC0, T_MCU, 0, k, 3, false);
      printf("  vendor 0xC0 MCU write   : %s\n", ioerr(r)); }

    // Some FX3 firmware only dispatches vendor requests once an interface is
    // in use. Costs nothing to rule out.
    if (claimInterface0()) {
        int g2 = gpioRead(0x0039);
        printf("  same, interface 0 claimed: %s%02x\n",
               g2 < 0 ? "TIMEOUT (" : "0x", g2 < 0 ? 0 : g2);
    }

    printf("\n-- MCU registers (0x9b) --\n");
    static const uint8_t mregs[] = { 0x01, 0x09, 0x0b, 0x0c, 0x0e };
    for (uint8_t r : mregs) {
        int v = mcuRead(r);
        printf("  MCU 0x%02x = %s%02x\n", r, v < 0 ? "no answer (" : "0x", v < 0 ? 0 : v);
        if (v < 0) { printf("  (MCU silent, skipping the rest of the register reads)\n");
                     break; }
    }
    printf("\n-- video front end (0x9d, per page) --\n");
    printf("  page 0: 0xab=0x%02x 0xb3=0x%02x 0xb0=0x%02x\n",
           vidRead(0,0xab), vidRead(0,0xb3), vidRead(0,0xb0));
    printf("  page 1: 0x24=0x%02x 0x25=0x%02x 0x26=0x%02x\n",
           vidRead(1,0x24), vidRead(1,0x25), vidRead(1,0x26));
    printf("  page 2: 0x0e=0x%02x 0x27=0x%02x 0x20=0x%02x 0x11=0x%02x 0x12=0x%02x\n",
           vidRead(2,0x0e), vidRead(2,0x27), vidRead(2,0x20),
           vidRead(2,0x11), vidRead(2,0x12));
    printf("  page 0: 0x55=0x%02x   (capture, locked: 0x03)\n", vidRead(0,0x55));
    printf("\n  readiness (page 2 reg 0x0e): 0x%02x  %s\n", vidRead(2,0x0e),
           vidRead(2,0x0e) == 0 ? "ALREADY READY" : "not zero - needs arming");
    printf("\n-- GPIO 0x39 --\n  0x%02x\n", gpioRead(0x0039));

    if (!gIf && !claimInterface0()) return 3;
    printf("\n-- interface 0 alternate settings --\n");
    for (UInt8 alt = 0; alt <= 4; alt++) {
        IOReturn ar = (*gIf)->SetAlternateInterface(gIf, alt);
        if (ar != kIOReturnSuccess) {
            printf("  alt %u: SetAlternateInterface -> 0x%08x\n", alt, ar); continue; }
        UInt8 np = 0; (*gIf)->GetNumEndpoints(gIf, &np);
        printf("  alt %u: %u endpoint(s)", alt, np);
        for (UInt8 p = 1; p <= np; p++) {
            UInt8 dir, num, tt, ival, burst, mult; UInt16 mps, bpi;
            if ((*gIf)->GetPipePropertiesV2(gIf, p, &dir, &num, &tt, &mps, &ival,
                                            &burst, &mult, &bpi) != kIOReturnSuccess) continue;
            printf("   ep 0x%02x %s mps=%u burst=%u mult=%u bytesPerInterval=%u",
                   (unsigned)(num | (dir==kUSBIn?0x80:0)),
                   tt==kUSBIsoc?"isoc":(tt==kUSBBulk?"bulk":"intr"), mps, burst, mult, bpi);
        }
        printf("\n");
    }
    (*gIf)->SetAlternateInterface(gIf, 0);
    printf("\n  probe complete. Nothing was written.\n");
    return 0;
}

// The first six operations of the vendor driver's cold init, and nothing more.
// Four zero-length OUTs then a read, so each step is checked rather than fired
// into the dark. 0xC2 resets part of the card and will drop HDMI passthrough.
static int phaseWake() {
    printf("\n-- wake: first six ops of the vendor cold init --\n");
    struct { uint8_t bReq; uint16_t wValue; const char* what; } ops[] = {
        { 0xC2, 0x0000, "reset/prepare" },
        { 0xC7, 0x0064, "prepare" },
        { 0xC1, 0xC039, "gpio 0x39 set" },
        { 0xC1, 0x4134, "gpio 0x34 set" },
    };
    for (auto& o : ops) {
        IOReturn r = ctl(o.bReq, o.wValue, 0, nullptr, 0, false);
            printf("  0x%02x wValue=0x%04x  %-16s -> %s\n",
               o.bReq, o.wValue, o.what, ioerr(r));
        if (r != kIOReturnSuccess) { printf("  stopping here.\n"); return -1; }
        usleep(2000);
    }
    int g = gpioRead(0x0039);
    printf("  read back gpio 0x39                -> %s%02x   (capture shows 0x01)\n",
           g < 0 ? "TIMEOUT (" : "0x", g < 0 ? 0 : g);
    if (g < 0) { printf("  the FX3 still will not answer a read. Wake did not take.\n"); return -1; }

    int m = mcuRead(0x0e);
    printf("  MCU register 0x0e                  -> %s%02x\n",
           m < 0 ? "no answer (" : "0x", m < 0 ? 0 : m);
    if (m < 0) { printf("  FX3 is awake but the MCU is not answering yet.\n"); return -1; }
    printf("  MCU is awake.\n");
    return 0;
}

// The FX3 / GPIO prologue the application issues when capture begins.
// Fixed values, but this is the block that needs wIndex, so it is opt-in.
static void phasePrologue() {
    printf("\n-- capture-start prologue --\n");
    vidWrite(0, 0xb0, 0x21);
    vidWrite(0, 0xab, 0x15);
    ctl(0xC2, 0x0000, 0, nullptr, 0, false);
    ctl(0xC7, 0x0064, 0, nullptr, 0, false);
    ctl(0xC1, 0xC039, 0, nullptr, 0, false);
    ctl(0xC1, 0x4134, 0, nullptr, 0, false);
    printf("  gpio 0x39 -> 0x%02x\n", gpioRead(0x0039));
    printf("  port 0x3b -> 0x%02x\n", portRead(0x3b));
    portWrite(0x3b, 0x02);
    portWrite(0x20, 0x05);
    portWrite(0x00, 0x00);
    portWrite(0x10, 0xfe);
    printf("  port 0x01 -> 0x%02x\n", portRead(0x01));
    for (int i = 0; i < 2; i++) { portWrite(0x01, 0x03); portWrite(0x11, 0xfc); portRead(0x01); }
    portWrite(0x01, 0x02); portWrite(0x11, 0xfc);
    portRead(0x20); portWrite(0x20, 0x15);
    portRead(0x20); portWrite(0x20, 0x05);
    portRead(0x20); portWrite(0x20, 0x05);
    printf("  prologue done\n");
}

// The block that makes page 2 register 0x0E start counting down.
// 0xAB bit 7 is pulsed; 0xB3 goes 0xFF then 0x00; page 1 0x24/0x25/0x26 are set.
// Both 0xAB and 0x27 are written back to the value just read, not a constant.
// The vendor's idle loop rewrites these three every cycle, so they are the
// steady state the front end is meant to sit in. A card that has never had the
// cold init has them at 0x00, which makes the arming block write 0x00 back.
static void phaseMaintain() {
    printf("\n-- restoring the steady-state registers the idle loop holds --\n");
    printf("  before: p0 0xb3=0x%02x  p2 0x27=0x%02x  p2 0x20=0x%02x\n",
           vidRead(0,0xb3), vidRead(2,0x27), vidRead(2,0x20));
    vidWrite(0, 0xb3, 0xff);
    vidWrite(2, 0x27, 0xff);
    vidWrite(2, 0x20, 0x00);
    printf("  after : p0 0xb3=0x%02x  p2 0x27=0x%02x  p2 0x20=0x%02x\n",
           vidRead(0,0xb3), vidRead(2,0x27), vidRead(2,0x20));
}

static void phaseArm() {
    printf("\n-- arming the video front end --\n");
    int ab  = vidRead(0, 0xab);
    int r27 = vidRead(2, 0x27);
    printf("  read page0 0xab=0x%02x  page2 0x27=0x%02x\n", ab, r27);
    if (ab < 0 || r27 < 0) { printf("  reads failed, not arming\n"); return; }

    vidWrite(0, 0xab, (uint8_t)(ab | 0x80));
    vidWrite(2, 0x27, (uint8_t)r27);
    vidWrite(0, 0xb3, 0xff);
    vidWrite(1, 0x24, 0x41);
    vidWrite(1, 0x25, 0x00);
    vidWrite(1, 0x26, 0x00);
    usleep(5000);
    vidWrite(0, 0xab, (uint8_t)ab);
    vidWrite(2, 0x27, (uint8_t)r27);
    vidWrite(0, 0xb3, 0x00);
    printf("  armed (0xab pulsed 0x%02x -> 0x%02x, 0xb3 0xff -> 0x00)\n",
           ab | 0x80, ab);
}

// Poll page 2 register 0x0E, mirror each value back, stop at zero.
// The vendor driver took 1.6 s and 227 iterations on a real lock, so the
// default budget is generous.
static int phaseWaitReady(int maxIter) {
    printf("\n-- waiting for readiness (page 2 reg 0x0e -> 0x00) --\n");
    int last = -1;
    for (int i = 0; i < maxIter; i++) {
        int v = vidRead(2, 0x0e);
        if (v < 0) { printf("  read failed at iteration %d\n", i); return -1; }
        vidWrite(2, 0x0e, (uint8_t)v);
        if (v != last) { printf("  iter %4d: 0x0e = 0x%02x%s\n", i, v, v ? "" : "   READY"); last = v; }
        if (v == 0) return i;
        usleep(7000);
    }
    printf("  still not zero after %d iterations (%.1f s)\n", maxIter, maxIter * 0.007);
    printf("  0x0e last read 0x%02x. Non-zero means no locked HDMI source, or HDCP.\n", last);
    return -1;
}

// Read-only. Poll the registers the vendor driver polls and report movement.
// Locked-signal values seen in the capture, for comparison:
//   MCU 0x09=0xb2  0x0e=0x6c  0x11=0xf4  0x12=0xa2  0x55=0x03
static int phaseWatch(int secs) {
    printf("\n-- watching signal registers for %d s (read-only) --\n", secs);
    printf("  capture, locked : MCU 09=b2 0e=6c | video p0 55=03  p2 11=f4 12=a2\n\n");
    int p09=-2,p0e=-2,p11=-2,p12=-2,p55=-2,pv=-2;
    int changes = 0;
    for (int i = 0; i * 50 < secs * 1000; i++) {
        // 0x09 and 0x0e are MCU registers; 0x55 is video page 0; 0x11, 0x12
        // and 0x0e are video page 2. Reading the video ones with 0x9b returns
        // noise, which is how this was caught.
        int a=mcuRead(0x09), b=mcuRead(0x0e), e=vidRead(0, 0x55),
            c=vidRead(2, 0x11), d=vidRead(2, 0x12), v=vidRead(2, 0x0e);
        if (a!=p09||b!=p0e||c!=p11||d!=p12||e!=p55||v!=pv) {
            printf("  t=%5.2fs  MCU 09=%02x 0e=%02x | video p0 55=%02x  p2 11=%02x 12=%02x 0e=%02x\n",
                   i*0.05, a,b,e,c,d,v);
            p09=a;p0e=b;p11=c;p12=d;p55=e;pv=v; changes++;
        }
        usleep(50000);
    }
    printf("\n  %d distinct states in %d s.\n", changes, secs);
    if (changes <= 1)
        printf("  Nothing moved. Either the input is stable or there is no signal at all.\n");
    return 0;
}

// Read-only. Show every video register the cold init touches, current value
// against the value the vendor writes, so the gap is visible before any write.
static int phaseDump() {
    printf("\n-- cold init registers: current vs what the vendor writes --\n");
    int seen[3][256]; memset(seen, 0xff, sizeof seen);
    int nDiff = 0, nSame = 0, nSkip = 0;
    uint8_t lastPage = 0xff;
    for (int i = 0; i < kColdInitLen; i++) {
        const InitReg& o = kColdInit[i];
        if (o.kind != 0) continue;
        if (o.aux > 2) { nSkip++; continue; }          // page 128 is a separate space
        if (seen[o.aux][o.data[0]] != -1) continue;    // first write wins for the report
        int cur = vidRead(o.aux, o.data[0]);
        seen[o.aux][o.data[0]] = cur;
        if (o.aux != lastPage) { printf("\n  page %u\n   ", o.aux); lastPage = o.aux; }
        bool diff = (cur != o.data[1]);
        printf(" %02x:%02x%s%02x", o.data[0], cur, diff ? "!=" : "==", o.data[1]);
        if (diff) nDiff++; else nSame++;
    }
    printf("\n\n  %d already match, %d differ, %d skipped (page 128)\n", nSame, nDiff, nSkip);
    return 0;
}

// Apply the cold init. This is a replay, but a state-matched one: the sequence
// opens with the same 0xC2 reset the vendor driver issues, so the card is put
// into the state the trace was captured from rather than having values written
// into some other state. Recovery if it goes wrong is ./start wake, not a
// physical replug.
static int phaseInit(bool verify) {
    printf("\n-- applying the cold init (%d ops) --\n", kColdInitLen);
    printf("  p0 F0-FF before:");
    for (int r = 0xf0; r <= 0xff; r++) printf(" %02x", vidRead(0, (uint8_t)r));
    printf("\n");
    int fail = 0, mismatch = 0, nv = 0, nm = 0, nh = 0, nc = 0, nSkipped = 0;
    int limit = (gLimit >= 0 && gLimit < kColdInitLen) ? gLimit : kColdInitLen;
    for (int i = 0; i < limit; i++) {
        const InitReg& o = kColdInit[i];
        // The cold init is what kills the HDMI passthrough and clears the
        // input-status register p1 0x34, so its parts are individually skippable
        // while the minimum useful set is being found.
        if (o.kind == 1 && o.len && (o.data[0] == 0xd4 || o.data[0] == 0xd5)) {
            if (gNoEdid) { nSkipped++; continue; }
        }
        if (o.kind == 2 && gNoTx)  { nSkipped++; continue; }
        // Page 0x80 may not be a valid page on this part. If the page select
        // does not take, these writes land somewhere unintended, and page 0
        // registers 0xF0-0xFF end up cleared when nothing writes them.
        if (o.kind == 0 && o.aux == 0x80 && gNoP80) { nSkipped++; continue; }
        if (o.kind == 3 && gNoCtl) { nSkipped++; continue; }
        IOReturn r;
        switch (o.kind) {
            case 0: r = vidWrite(o.aux, o.data[0], o.data[1]); nv++; break;
            case 1: r = ctl(0xC0, T_MCU, 0, (void*)o.data, o.len, false); nm++; break;
            case 2: { uint8_t p[2] = { o.data[0], o.data[1] };
                      r = ctl(0xC0, 0x5098, 0, p, 2, false); nh++; } break;
            case 4: { uint8_t p[8]; memcpy(p, o.data, o.len);
                      r = ctl(0xC0, T_PORT, o.aux, p, o.len, false); nh++; } break;
            default: { uint16_t wv = (uint16_t)(o.data[0] | (o.data[1] << 8));
                       // 0xC6 uses wIndex 0x0100, which the old trace had lost
                       r = ctl(o.aux, wv, o.windex, nullptr, 0, false); nc++;
                       usleep(2000); } break;
        }
        if (r != kIOReturnSuccess) {
            printf("  op %3d kind %u -> %s\n", i, o.kind, ioerr(r));
            if (++fail > 8) { printf("  too many failures, stopping.\n"); return -1; }
        } else if (verify && o.kind == 0 && o.aux <= 2) {
            int back = vidRead(o.aux, o.data[0]);
            if (back != o.data[1] && mismatch < 12) {
                printf("  op %3d page %u reg 0x%02x: wrote 0x%02x, reads 0x%02x\n",
                       i, o.aux, o.data[0], o.data[1], back);
                mismatch++;
            }
        }
    }
    printf("  applied: %d video, %d MCU, %d HDMI-tx, %d control, %d skipped\n",
           nv, nm, nh, nc, nSkipped);
    printf("  p0 F0-FF after :");
    for (int r = 0xf0; r <= 0xff; r++) printf(" %02x", vidRead(0, (uint8_t)r));
    printf("\n  (vendor had F3=c0 F5=3f F6=08 F7=29 F9=60 FA=04)\n");
    printf("  %d failed writes, %d read-back mismatches%s\n", fail, mismatch,
           mismatch ? " (some registers are write-only or hardware-updated)" : "");
    return fail ? -1 : 0;
}

// The vendor driver runs this cycle about eight times a second for as long as
// the card is open, before and during capture. We only ever ran it once. If the
// front end needs the host to keep feeding it, a one-shot will not do.
static volatile bool gPoll = false;
static UInt32 gPollN = 0;
static void* pollThread(void*) {
    while (gPoll) {
        mcuRead(0x09);
        mcuWrite(0x0f, 0x00); mcuWrite(0xc1, 0x01); mcuWrite(0xc6, 0x03);
        vidRead(0, 0x55);
        vidWrite(0, 0xb3, 0xff);
        vidWrite(2, 0x27, 0xff);
        vidRead(2, 0x11); vidRead(2, 0x12);
        vidWrite(2, 0x20, 0x00);
        int v = vidRead(2, 0x0e);
        if (v >= 0) vidWrite(2, 0x0e, (uint8_t)v);
        gPollN++;
        usleep(120000);
    }
    return nullptr;
}

// The video timing configuration, recovered from a known-good capture.
//
// The driver measures the incoming signal, then writes this block. The values
// are computed, so replaying them is only valid if the card is measuring the
// same signal. The reference set below is what the front end read at the moment
// the vendor driver wrote these values; ./start measure checks ours against it
// and ./start format refuses to write unless they agree.
//
// The measurement decodes as 1920x1080: p0 6a/6b = 0x0898 = 2200 pixels H total,
// p0 5b/5c = 0x0465 = 1125 lines V total.
// `id` marks the registers that identify the video format. The rest are sync
// positions that jitter by a pixel or two between reads, and status bits that
// depend on where the front end is in its own state machine. Only the format
// identity has to match before the timing block is safe to write.
struct RefReg { uint8_t page, reg, val, id; };
static const RefReg kMeasureRef[] = {
    { 0, 0x6a, 0x08, 1 }, { 0, 0x6b, 0x98, 1 },   // H total 0x0898 = 2200
    { 0, 0x5b, 0x04, 1 }, { 0, 0x5c, 0x65, 1 },   // V total 0x0465 = 1125
    { 2, 0x11, 0x7a, 1 }, { 2, 0x12, 0x11, 1 },
    { 0, 0x57, 0x09, 1 }, { 0, 0x59, 0x08, 1 }, { 0, 0x5f, 0x00, 1 },
    { 1, 0x01, 0x8d, 1 }, { 2, 0x29, 0x07, 1 }, { 2, 0x28, 0x80, 1 },
    { 2, 0x49, 0xa8, 1 }, { 2, 0x4b, 0x10, 1 }, { 2, 0x47, 0x00, 1 },
    { 0, 0x55, 0x7f, 0 }, { 0, 0x58, 0x47, 0 }, { 0, 0x5a, 0x27, 0 },
    { 1, 0x34, 0x90, 0 }, { 2, 0x4a, 0x50, 0 }, { 2, 0x48, 0x82, 0 },
    { 2, 0x0c, 0x00, 0 },
};
static const int kMeasureRefLen = sizeof kMeasureRef / sizeof kMeasureRef[0];

// Written in this order, between an 0xAB bit-7 hold and release.
static const RefReg kTiming[] = {
    { 0, 0x90, 0x15 }, { 0, 0x91, 0x15 }, { 0, 0x92, 0x40 },
    { 0, 0xab, 0x95 }, { 0, 0xac, 0x95 }, { 0, 0xad, 0x05 },
    { 0, 0x1e, 0x11 }, { 0, 0x1f, 0x01 },
    { 0, 0x9c, 0xfc }, { 0, 0x9b, 0x09 }, { 0, 0x96, 0xf7 }, { 0, 0x95, 0x02 },
    { 0, 0xa2, 0x02 }, { 0, 0xa1, 0x01 }, { 0, 0x9a, 0xa0 }, { 0, 0x99, 0x79 },
    { 0, 0x94, 0x06 }, { 0, 0x93, 0x07 },
    { 0, 0xa0, 0x5b }, { 0, 0x9f, 0x7f }, { 0, 0x9e, 0x96 }, { 0, 0x9d, 0x7a },
    { 0, 0x98, 0x65 }, { 0, 0x97, 0x7e },
    { 0, 0xa4, 0x06 }, { 0, 0xa3, 0x07 },
    { 0, 0xa6, 0x00 }, { 0, 0xa5, 0x20 },
    { 0, 0xa8, 0x00 }, { 0, 0xa7, 0x00 },
    { 0, 0xaa, 0x00 }, { 0, 0xa9, 0x20 },
    { 0, 0xb0, 0x21 },
};
static const int kTimingLen = sizeof kTiming / sizeof kTiming[0];

static int phaseMeasure() {
    printf("\n-- incoming signal measurement vs the known-good capture --\n");
    int match = 0, idMatch = 0, idTotal = 0;
    for (int i = 0; i < kMeasureRefLen; i++) {
        const RefReg& r = kMeasureRef[i];
        int v = vidRead(r.page, r.reg);
        bool ok = (v == r.val);
        if (ok) match++;
        if (r.id) { idTotal++; if (ok) idMatch++; }
        printf("  p%u %02x = %02x   reference %02x  %-7s %s\n",
               r.page, r.reg, v < 0 ? 0 : v, r.val, ok ? "match" : "DIFFERS",
               r.id ? "format identity" : "(jitter/status)");
    }
    int ht = (vidRead(0,0x6a) << 8) | vidRead(0,0x6b);
    int vt = (vidRead(0,0x5b) << 8) | vidRead(0,0x5c);
    printf("\n  H total %d, V total %d", ht, vt);
    if (ht == 2200 && vt == 1125) printf("  -> 1920x1080\n");
    else printf("  -> not the 1080p the reference was taken from\n");
    printf("  %d of %d registers match; format identity %d of %d\n",
           match, kMeasureRefLen, idMatch, idTotal);
    return idMatch == idTotal ? 0 : 1;
}

// Apply the timing block. Guarded: the values are computed for one measured
// signal, so writing them against a different one is exactly the mistake that
// wedged this card before.
static int phaseFormat(bool force) {
    printf("\n-- video timing configuration --\n");
    // A single read can land while the front end is mid-update, so a first
    // miss is retried rather than treated as a different format.
    int match = 0, total = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        match = 0; total = 0;
        for (int i = 0; i < kMeasureRefLen; i++) {
            if (!kMeasureRef[i].id) continue;
            total++;
            if (vidRead(kMeasureRef[i].page, kMeasureRef[i].reg) == kMeasureRef[i].val) match++;
        }
        if (match == total) break;
        usleep(30000);
    }
    printf("  format identity matches on %d of %d registers\n", match, total);
    if (match < total && !force) {
        printf("  refusing to write: these values are computed for the reference\n"
               "  signal. Run ./start measure to see which differ, or pass --force.\n");
        return -1;
    }
    int ab = vidRead(0, 0xab);
    if (ab < 0) { printf("  cannot read 0xab\n"); return -1; }
    vidWrite(0, 0xab, (uint8_t)(ab | 0x80));            // hold
    for (int i = 0; i < kTimingLen; i++) {
        // 0x93..0xAA pair up into twelve 16-bit values, which is the shape of a
        // 3x3 colour matrix plus three offsets. The reference was captured from
        // a YCbCr source; a computer output is usually RGB and wants a different
        // matrix, so --nomatrix leaves the cold init's values in place.
        uint8_t r = kTiming[i].reg;
        if (gNoMatrix && r >= 0x93 && r <= 0xaa) continue;
        vidWrite(kTiming[i].page, r, kTiming[i].val);
    }
    if (gNoMatrix) printf("  (colour matrix 0x93..0xAA left at the cold init values)\n");
    vidWrite(0, 0xab, (uint8_t)(ab & 0x7f));            // release
    printf("  %d timing registers written, 0xab pulsed 0x%02x -> 0x%02x\n",
           kTimingLen, ab | 0x80, ab & 0x7f);
    int bad = 0;
    for (int i = 0; i < kTimingLen && !gNoMatrix; i++) {
        int back = vidRead(kTiming[i].page, kTiming[i].reg);
        if (back != kTiming[i].val) {
            if (bad < 12)
                printf("    p%u %02x: wrote %02x, reads %02x\n",
                       kTiming[i].page, kTiming[i].reg, kTiming[i].val, back);
            bad++;
        }
    }
    printf("  read-back: %d of %d timing registers did not hold\n", bad, kTimingLen);
    return 0;
}

// Colour space conversion in the video front end, set from the source.
//
// The HDMI transmitter behind the passthrough port (IT66121, I2C 0x9A) is set
// up by the cold init to take YCbCr 4:2:2 from the front end and convert it to
// RGB for the display. The capture path takes the same YCbCr. The cold init
// leaves the front end matrix at identity, which is right for a YCbCr source and
// wrong for an RGB one: R, G and B go out as Cr, Y and Cb, so grey turns green
// and white turns pink, on the TV and in OBS alike. A Switch always sends RGB,
// and so does a PS5 at 1080p SDR.
//
// Page 2 0x48..0x4b hold the received AVI InfoFrame, PB1..PB4. 0x48 bit 7 is set
// whenever a frame has been received (0x80, 0x82, 0xd2 seen; 0x00 with no
// signal), and bits 6:5 are the colour space: 0 RGB, 1 YCbCr 4:2:2, 2 4:4:4.
//
// For RGB the vendor driver writes p0 0x92 = 0x40 and a BT.709 RGB to YCbCr
// limited matrix into 0x93..0xAA. Each coefficient is a 16-bit pair, high byte
// first, 1.0 = 0x1000, negative values in two's complement over 15 bits. The
// nine are laid out by input R, G, B, each giving Cr, Y, Cb.
// The cold init's own values are restored for a YCbCr source.
static const uint8_t kCscRgb[] = {
    0x07,0x06, 0x02,0xf7, 0x7e,0x65,   // R -> Cr  0.439, Y  0.185, Cb -0.100
    0x79,0xa0, 0x09,0xfc, 0x7a,0x96,   // G -> Cr -0.398, Y  0.624, Cb -0.338
    0x7f,0x5b, 0x01,0x02, 0x07,0x06,   // B -> Cr -0.040, Y  0.063, Cb  0.439
    0x20,0x00, 0x00,0x00, 0x20,0x00,   // offsets
};
static const uint8_t kCscYcbcr[] = {
    0x0d,0xc0, 0x00,0x00, 0x00,0x00,
    0x00,0x00, 0x0d,0xf6, 0x00,0x00,
    0x00,0x00, 0x00,0x00, 0x0d,0xc0,
    0x20,0x00, 0x00,0x00, 0x20,0x00,
};
static const uint8_t kCscModeRgb = 0x40, kCscModeYcbcr = 0x62;

static int sourceColourSpace() {
    // The InfoFrame can be read mid-update; take two matching reads.
    for (int attempt = 0; attempt < 10; attempt++) {
        int a = vidRead(2, 0x48);
        usleep(20000);
        int b = vidRead(2, 0x48);
        if (a >= 0 && a == b && (a & 0x80)) return (a >> 5) & 3;
        usleep(100000);
    }
    return -1;
}

// mode: 0 auto, 1 force RGB, 2 force YCbCr
static int phaseColour(int mode) {
    printf("\n-- colour space conversion --\n");
    int cs = sourceColourSpace();
    static const char* names[] = { "RGB", "YCbCr 4:2:2", "YCbCr 4:4:4", "reserved" };
    if (cs >= 0) printf("  source InfoFrame: %s (p2 48 = %02x)\n", names[cs], vidRead(2, 0x48));
    else         printf("  no AVI InfoFrame from the source (no signal yet?)\n");
    bool rgb;
    if (mode == 1)      rgb = true;
    else if (mode == 2) rgb = false;
    else if (cs < 0) {
        printf("  leaving the conversion as it is. Run ./start colour once the source is on.\n");
        return 1;
    } else rgb = (cs == 0);

    int ab = vidRead(0, 0xab);
    if (ab < 0) { printf("  front end not answering, run the cold init first\n"); return 2; }
    vidWrite(0, 0xab, (uint8_t)(ab | 0x80));            // hold
    vidWrite(0, 0x92, rgb ? kCscModeRgb : kCscModeYcbcr);
    const uint8_t* m = rgb ? kCscRgb : kCscYcbcr;
    for (int i = 0; i < 24; i++) vidWrite(0, (uint8_t)(0x93 + i), m[i]);
    vidWrite(0, 0xab, (uint8_t)(ab & 0x7f));            // release

    int bad = (vidRead(0, 0x92) != (rgb ? kCscModeRgb : kCscModeYcbcr));
    for (int i = 0; i < 24; i++) if (vidRead(0, (uint8_t)(0x93 + i)) != m[i]) bad++;
    printf("  set for %s input%s\n", rgb ? "RGB" : "YCbCr", bad ? "" : ", read back ok");
    if (bad) printf("  %d registers did not hold\n", bad);
    return bad ? 3 : 0;
}

// Read-only. Compare our register state against the state the vendor driver's
// card was actually in, taken from every read in the good capture. Anything
// that differs is a candidate for the colour fault.
static int phaseVdiff() {
    printf("\n-- our register state vs the vendor's working session --\n");
    int same=0, diff=0;
    printf("  differences:\n");
    for (int i = 0; i < kVendorRegsLen; i++) {
        const VendorReg& r = kVendorRegs[i];
        int v = (r.kind == 0) ? vidRead(r.page, r.reg) : mcuRead(r.reg);
        if (v == r.val) { same++; continue; }
        diff++;
        printf("    %s p%u reg %02x : ours %02x, vendor %02x   (%u samples)\n",
               r.kind == 0 ? "vid" : "MCU", r.page, r.reg, v, r.val, r.samples);
    }
    printf("\n  %d match, %d differ, of %d registers the vendor read\n",
           same, diff, kVendorRegsLen);
    return 0;
}

// Read-only. Dump the video front end's register space on each page and look
// for a received AVI InfoFrame (type 0x82, version 0x02, length 0x0D). Byte 1
// of its payload carries the source's colour space in bits 6:5, which is the
// thing in question: 0 RGB, 1 YCbCr 4:2:2, 2 YCbCr 4:4:4.
static int phaseScan() {
    uint8_t page[3][256];
    for (int p = 0; p < 3; p++) {
        printf("\n-- video front end page %d --\n", p);
        for (int r = 0; r < 256; r++) {
            int v = vidRead(p, (uint8_t)r);
            page[p][r] = (uint8_t)(v < 0 ? 0 : v);
            if ((r & 15) == 0) printf("  %02x:", r);
            printf(" %02x", page[p][r]);
            if ((r & 15) == 15) printf("\n");
        }
    }
    printf("\n-- searching for an AVI InfoFrame (82 02 0d) --\n");
    int found = 0;
    for (int p = 0; p < 3; p++)
        for (int r = 0; r < 253; r++)
            if (page[p][r] == 0x82 && page[p][r+1] == 0x02 && page[p][r+2] == 0x0d) {
                found++;
                printf("  page %d offset 0x%02x:", p, r);
                for (int k = 0; k < 14 && r + k < 256; k++) printf(" %02x", page[p][r+k]);
                // payload starts after type/version/length/checksum
                int y = (page[p][r+4] >> 5) & 3;
                static const char* cs[] = { "RGB", "YCbCr 4:2:2", "YCbCr 4:4:4", "reserved" };
                printf("\n    colour space bits = %d -> %s\n", y, cs[y]);
            }
    if (!found) printf("  no InfoFrame signature in the register space\n");
    return 0;
}

// Read the downstream display's EDID through the MCU's I2C proxy, which is
// what the vendor driver does immediately after the cold init and which this
// driver has never done. The transmitter that feeds the passthrough port
// plausibly will not enable its output until the sink's capabilities are known,
// and the reported behaviour matches: on the vendor driver the passthrough goes
// black and then comes back, whereas here it goes black and stays black.
//
// Proxy protocol, read off the capture:
//   0x10 = 01            select/enable the proxy
//   0x11 = a0            I2C address (0xA0 is DDC/EDID)
//   0x12 = offset        byte offset into the EDID
//   0x13 = length
//   0x14 = 00
//   0x15 = 03            trigger  (0x09 seen once as an init/reset)
//   0x16                 status, read back
//   0x17                 data port, read repeatedly
static int phaseSinkEdid() {
    printf("\n-- reading the downstream display's EDID via the MCU I2C proxy --\n");
    uint8_t edid[256]; memset(edid, 0, sizeof edid);
    mcuWrite(0x10, 0x01);
    mcuWrite(0x15, 0x09);
    mcuWrite(0x65, 0x02);
    for (int blk = 0; blk < 8; blk++) {
        uint8_t off = (uint8_t)(blk * 0x20);
        mcuWrite(0x10, 0x01);
        mcuWrite(0x11, 0xa0);
        mcuWrite(0x12, off);
        mcuWrite(0x13, 0x20);
        mcuWrite(0x14, 0x00);
        mcuWrite(0x15, 0x03);
        int st = -1;
        for (int t = 0; t < 40; t++) { st = mcuRead(0x16); if (st >= 0 && (st & 0x80)) break; usleep(2000); }
        for (int i = 0; i < 32; i++) {
            int v = mcuRead(0x17);
            edid[off + i] = (uint8_t)(v < 0 ? 0 : v);
        }
        if (blk == 0) printf("  status after first block: 0x%02x\n", st);
    }
    mcuWrite(0x65, 0x00);
    mcuWrite(0x0f, 0x00);

    printf("  EDID bytes 0-31:");
    for (int i = 0; i < 32; i++) printf(" %02x", edid[i]);
    printf("\n");
    static const uint8_t hdr[8] = {0x00,0xff,0xff,0xff,0xff,0xff,0xff,0x00};
    bool ok = !memcmp(edid, hdr, 8);
    printf("  header %s\n", ok ? "VALID - the sink's EDID was read"
                               : "not an EDID (proxy sequence is wrong, or nothing downstream)");
    return ok ? 0 : 1;
}

// Replay a decode_trace.py --seq file verbatim: every control transfer in
// capture order, reads included. `init` only replays a filtered subset of the
// writes, and reads are not always side-effect free here -- the MCU I2C proxy
// is driven by reading a status and a data port, and status reads can clear
// latched bits. This is the closest reproduction of what the vendor driver did.
//
// It is a blind replay, which is normally the wrong tool. It is defensible only
// because the trace was taken from a cold plug of this same card, so running it
// on a freshly power-cycled card starts from the state it was recorded in.
static int phaseReplay(const char* path, int maxOps) {
    FILE* f = fopen(path, "r");
    if (!f) { printf("  cannot open %s\n", path); return -1; }
    printf("\n-- replaying %s verbatim --\n", path);
    char line[4096];
    int n = 0, out = 0, in = 0, skipped = 0, failed = 0;
    while (fgets(line, sizeof line, f) && (maxOps <= 0 || n < maxOps)) {
        char dir[8], breq[16], wv[16], wi[16];
        int len = 0;
        if (sscanf(line, "%7[^|]|%15[^|]|%15[^|]|%15[^|]|%d|", dir, breq, wv, wi, &len) != 5)
            continue;
        if (!strcmp(dir, "RET")) continue;
        unsigned br = (unsigned)strtol(breq, nullptr, 16);
        unsigned v  = (unsigned)strtol(wv,   nullptr, 16);
        unsigned ix = (unsigned)strtol(wi,   nullptr, 16);
        // standard requests: the host stack owns these
        if (br == 0x06 || br == 0x00 || br == 0x0b || br == 0x31) { skipped++; continue; }
        uint8_t buf[72]; memset(buf, 0, sizeof buf);
        if (len > (int)sizeof buf) len = sizeof buf;
        bool isIn = !strcmp(dir, "IN");
        if (!isIn && len > 0) {
            // payload is the sixth field
            const char* d = line;
            for (int k = 0; k < 5 && d; k++) d = strchr(d + 1, '|');
            if (d) { d++;
                for (int k = 0; k < len; k++) {
                    while (*d == ' ') d++;
                    if (!*d || *d == '\n') break;
                    buf[k] = (uint8_t)strtol(d, (char**)&d, 16);
                } }
        }
        IOReturn r = ctl((uint8_t)br, (uint16_t)v, (uint16_t)ix,
                         len ? buf : nullptr, (uint16_t)len, isIn, 120);
        if (r != kIOReturnSuccess) failed++;
        isIn ? in++ : out++;
        n++;
        if (n % 2000 == 0) printf("  %d ops (%d failed)\n", n, failed);
    }
    fclose(f);
    printf("  replayed %d ops: %d OUT, %d IN, %d standard skipped, %d failed\n",
           n, out, in, skipped, failed);
    return 0;
}

// Interface 1 has an interrupt IN endpoint (0x81, 64 bytes) that nothing here
// has ever touched. 48 kHz stereo 16-bit is 192 KB/s, which fits, so this is
// the first place to look for the audio path.
static int phaseAudio(int secs) {
    IOUSBFindInterfaceRequest rq = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                     kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    io_iterator_t ii; (*gDev)->CreateInterfaceIterator(gDev, &rq, &ii);
    io_service_t is; IOUSBInterfaceInterface500** f1 = nullptr;
    while ((is = IOIteratorNext(ii))) {
        IOCFPlugInInterface** pl = nullptr; SInt32 sc; IOUSBInterfaceInterface500** x = nullptr;
        if (IOCreatePlugInInterfaceForService(is, kIOUSBInterfaceUserClientTypeID,
                kIOCFPlugInInterfaceID, &pl, &sc) == KERN_SUCCESS && pl) {
            (*pl)->QueryInterface(pl, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500), (LPVOID*)&x);
            (*pl)->Release(pl);
        }
        IOObjectRelease(is);
        if (!x) continue;
        UInt8 n = 0; (*x)->GetInterfaceNumber(x, &n);
        if (n != 1) { (*x)->Release(x); continue; }
        f1 = x; break;
    }
    IOObjectRelease(ii);
    if (!f1) { printf("  interface 1 not found\n"); return 1; }
    IOReturn r = (*f1)->USBInterfaceOpen(f1);
    printf("\n-- interface 1, interrupt endpoint --\n  open -> %s\n", ioerr(r));
    if (r != kIOReturnSuccess) return 1;
    UInt8 np = 0; (*f1)->GetNumEndpoints(f1, &np);
    UInt8 pipe = 0, dir, num, tt, ival; UInt16 mps = 0;
    for (UInt8 p = 1; p <= np; p++) {
        if ((*f1)->GetPipeProperties(f1, p, &dir, &num, &tt, &mps, &ival) != kIOReturnSuccess) continue;
        printf("  pipe %u: ep 0x%02x %s mps=%u interval=%u\n", p,
               (unsigned)(num | (dir == kUSBIn ? 0x80 : 0)),
               tt == kUSBInterrupt ? "interrupt" : (tt == kUSBIsoc ? "isoc" : "bulk"), mps, ival);
        if (dir == kUSBIn && tt == kUSBInterrupt) pipe = p;
    }
    if (!pipe) { printf("  no interrupt IN pipe\n"); return 1; }
    uint8_t buf[512]; UInt64 total = 0; int reads = 0, to = 0;
    printf("  reading for %d s ...\n", secs);
    for (int i = 0; i < secs * 50; i++) {
        UInt32 n2 = mps > sizeof buf ? sizeof buf : mps;
        IOReturn rr = (*f1)->ReadPipeTO(f1, pipe, buf, &n2, 50, 100);
        reads++;
        if (rr == kIOReturnSuccess && n2) {
            total += n2;
            if (reads < 4 || total == n2) {
                printf("    %u bytes:", n2);
                for (UInt32 k = 0; k < n2 && k < 24; k++) printf(" %02x", buf[k]);
                printf("\n");
            }
        } else if (rr == kIOUSBTransactionTimeout) to++;
        else if (rr == kIOUSBPipeStalled) (*f1)->ClearPipeStallBothEnds(f1, pipe);
    }
    printf("  %d reads, %d timeouts, %llu bytes total (%.1f KB/s)\n",
           reads, to, (unsigned long long)total, total / 1024.0 / secs);
    if (!total) printf("  nothing on the interrupt endpoint\n");
    (*f1)->USBInterfaceClose(f1); (*f1)->Release(f1);
    return 0;
}

// -------------------------------------------------------------- isochronous

#define NF 64          // microframe entries per transfer
#define NB 8           // transfers in flight
static UInt8  gPipe = 0;
static UInt32 gReq  = 0;     // bytes requested per microframe
static UInt64 gTot = 0, gFrame = 0;
static UInt32 gN = 0, gD = 0, gErr = 0, gFrames = 0, gNSizes = 0, gLastSize = 0;
static UInt32 gSizes[8];
static FILE*  gOut = nullptr;
struct X { uint8_t* b; IOUSBLowLatencyIsocFrame* f; };
static X gX[NB];
static void submit(X* x);
static CFRunLoopRef gLoop = nullptr;
static void onStreamInt(int) { if (gLoop) CFRunLoopStop(gLoop); }

static void isoCB(void* rc, IOReturn r, void*) {
    X* x = (X*)rc; gN++;
    UInt32 got = 0;
    for (int i = 0; i < NF; i++) {
        UInt32 n = x->f[i].frActCount;
        if (x->f[i].frStatus && x->f[i].frStatus != kIOReturnUnderrun) gErr++;
        if (n) { got += n; gFrames++;
                 if (n != gLastSize) { gSizes[gNSizes % 8] = n; gNSizes++; gLastSize = n; }
                 if (gOut) fwrite(x->b + (size_t)i * gReq, 1, n, gOut); }
    }
    if (got) { gD++; gTot += got; }
    if (gN <= 3)
        printf("  cb#%u result=0x%08x first frames:", gN, r),
        printf(" [%u/0x%x] [%u/0x%x] [%u/0x%x]\n",
               x->f[0].frActCount, x->f[0].frStatus,
               x->f[1].frActCount, x->f[1].frStatus,
               x->f[2].frActCount, x->f[2].frStatus);
    submit(x);
}
static void submit(X* x) {
    for (int i = 0; i < NF; i++) {
        x->f[i].frActCount = 0;
        x->f[i].frReqCount = (UInt16)gReq;
        x->f[i].frStatus   = (IOReturn)0;
    }
    UInt64 now = 0; AbsoluteTime at;
    if ((*gIf)->GetBusFrameNumber(gIf, &now, &at) == kIOReturnSuccess) {
        UInt64 floor = now + 8;
        if (gFrame < floor) gFrame = floor;
    }
    UInt64 u = gFrame;
    gFrame += NF / 8;                     // 8 microframes per bus frame
    (*gIf)->LowLatencyReadIsochPipeAsync(gIf, gPipe, x->b, u, NF, 1, x->f, isoCB, x);
}

// Alt 4 is a bulk endpoint. No frame scheduling, no bandwidth reservation,
// no low-latency buffers: if the card is producing anything at all, a plain
// ReadPipeTO will show it. This separates "card is silent" from "my isoc
// scheduling is wrong", which the isoc path alone cannot do.
static int phaseBulk(int secs, const char* out) {
    IOReturn sa = (*gIf)->SetAlternateInterface(gIf, 4);
    printf("\n-- bulk test on alt 4 -> 0x%08x --\n", sa);
    if (sa != kIOReturnSuccess) return 5;
    UInt8 np = 0; (*gIf)->GetNumEndpoints(gIf, &np);
    UInt8 pipe = 0, dir, num, tt, ival, burst, mult; UInt16 mps = 0, bpi = 0;
    for (UInt8 p = 1; p <= np; p++) {
        if ((*gIf)->GetPipePropertiesV2(gIf, p, &dir, &num, &tt, &mps, &ival,
                                        &burst, &mult, &bpi) != kIOReturnSuccess) continue;
        if (dir == kUSBIn && tt == kUSBBulk) { pipe = p; break; }
    }
    if (!pipe) { printf("  no bulk IN pipe on alt 4\n"); return 5; }
    printf("  ep 0x%02x bulk mps=%u, reading for %d s\n", (unsigned)(num | 0x80), mps, secs);

    const UInt32 CH = 262144;
    uint8_t* buf = (uint8_t*)malloc(CH);
    FILE* f = out ? fopen(out, "wb") : nullptr;
    UInt64 total = 0; int reads = 0, timeouts = 0, stalls = 0;
    for (int i = 0; i < secs * 4; i++) {
        UInt32 n = CH;
        IOReturn r = (*gIf)->ReadPipeTO(gIf, pipe, buf, &n, 250, 500);
        reads++;
        // ReadPipeTO leaves n at the requested size on timeout, so counting it
        // unconditionally reports megabytes of zeros that were never read.
        if (r == kIOReturnSuccess) {
            if (n) { total += n; if (f) fwrite(buf, 1, n, f); }
        } else if (r == kIOUSBTransactionTimeout) {
            timeouts++;
        } else if (r == kIOUSBPipeStalled) {
            stalls++; (*gIf)->ClearPipeStallBothEnds(gIf, pipe);
        } else if (i < 3) printf("  read %d -> 0x%08x %s\n", i, r, ioerr(r));
        if (total && reads <= 3)
            printf("  first bytes:"),
            printf(" %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7]);
    }
    if (f) fclose(f);
    free(buf);
    printf("\n  reads %d, timeouts %d, stalls %d\n", reads, timeouts, stalls);
    printf("  total bytes : %llu (%.2f MB)\n", (unsigned long long)total, total / 1e6);
    if (total) printf("  DATA on bulk -> %s\n", out ? out : "(discarded)");
    else       printf("  nothing on bulk either: the card is not producing frames.\n");
    (*gIf)->SetAlternateInterface(gIf, 0);
    return total ? 0 : 7;
}

// Issued immediately after SET_INTERFACE, before the first isochronous read.
// Nothing in the cold init or the arming block covers this, and without it the
// chroma slots carry a luma-dependent value instead of real colour.
// MCU register 0x0F is a page select, so 0x58..0x65 are on MCU page 1.
static void phasePostStart() {
    printf("  post-switch configuration:\n");
    mcuWrite(0x0f, 0x00); mcuWrite(0xc1, 0x01); mcuWrite(0xc6, 0x03);

    // The vendor reads these three then writes the same byte back, so the read
    // is part of a read-modify-write whose mask we do not know. Our card reads
    // different values, so writing ours back is not equivalent. Use the
    // vendor's settled values; --rmw keeps the read-back behaviour instead.
    int b2 = vidRead(0, 0xb2), b5 = vidRead(0, 0xb5);
    int r0f = vidRead(0, 0x0f), r17 = vidRead(0, 0x17);
    printf("    read p0 b2=%02x b5=%02x 0f=%02x 17=%02x  (vendor saw 00 00 28 02)\n",
           b2, b5, r0f, r17);
    vidWrite(0, 0xb2, gRmw && b2 >= 0 ? (uint8_t)b2 : 0x00);
    vidWrite(0, 0xb5, gRmw && b5 >= 0 ? (uint8_t)b5 : 0x00);
    vidWrite(0, 0x03, 0xc8);
    vidWrite(0, 0x0f, gRmw && r0f >= 0 ? (uint8_t)r0f : 0x28);
    vidWrite(0, 0x12, 0x04);
    vidWrite(0, 0x17, gRmw && r17 >= 0 ? (uint8_t)r17 : 0x02);
    vidWrite(0, 0x12, 0x00);
    vidWrite(0, 0x13, 0x14);

    // MCU page 1, the block the vendor writes at every capture start
    static const uint8_t mp1[][2] = {
        {0x58,0x10},{0x59,0xa8},{0x5a,0x00},{0x5b,0x10},{0x5c,0x00},{0x5e,0x00},
        {0x5f,0x00},{0x60,0x00},{0x61,0x00},{0x62,0x00},{0x63,0x00},{0x64,0x00},
        {0x65,0x00},{0x5d,0xa7},
    };
    mcuWrite(0x0f, 0x01);
    for (auto& kv : mp1) mcuWrite(kv[0], kv[1]);
    mcuWrite(0x0f, 0x00);
    mcuWrite(0xcd, 0x03);
    printf("    MCU page 1: 58=10 59=a8 5b=10 5d=a7, rest 00; then cd=03\n");

    int r60 = vidRead(0, 0x60);
    vidWrite(0, 0x60, gRmw && r60 >= 0 ? (uint8_t)r60 : 0x10);
    printf("    p0 60 read %02x, wrote %02x\n", r60, gRmw && r60 >= 0 ? r60 : 0x10);
}

// Write the MCU registers that ./start vdiff shows sitting at a different value
// from the vendor's working session, skipping the ones that are plainly status.
// Experimental: this is here to test whether the colour fault is one of them.
static void phaseVfix() {
    static const uint8_t regs[][2] = {
        {0x04,0x05},{0x08,0x10},{0x10,0x91},{0x16,0x83},{0x20,0x00},
        {0x58,0x15},{0x59,0x0b},{0x68,0x80},{0x70,0x40},{0x71,0x1c},{0x72,0x03},
    };
    printf("  vfix: writing %zu MCU registers to the vendor's values\n",
           sizeof regs / sizeof regs[0]);
    for (auto& kv : regs) mcuWrite(kv[0], kv[1]);
    printf("    after: p1 34 = %02x (vendor 90), p2 4a = %02x (vendor 50), p2 48 = %02x (vendor 82)\n",
           vidRead(1, 0x34), vidRead(2, 0x4a), vidRead(2, 0x48));
}

static int phaseStream(int alt, int secs, const char* out) {
    IOReturn sa = (*gIf)->SetAlternateInterface(gIf, alt);
    printf("\n-- SetAlternateInterface(%d) -> 0x%08x --\n", alt, sa);
    if (sa != kIOReturnSuccess) return 5;

    if (gPost) phasePostStart();

    UInt8 np = 0; (*gIf)->GetNumEndpoints(gIf, &np);
    UInt16 mps = 0, bpi = 0; UInt8 dir, num, tt, ival, burst, mult;
    for (UInt8 p = 1; p <= np; p++) {
        if ((*gIf)->GetPipePropertiesV2(gIf, p, &dir, &num, &tt, &mps, &ival,
                                        &burst, &mult, &bpi) != kIOReturnSuccess) continue;
        if (dir == kUSBIn && tt == kUSBIsoc) { gPipe = p; break; }
    }
    if (!gPipe) { printf("  no isochronous IN pipe on alt %d\n", alt); return 5; }
    // At SuperSpeed the device sends bytesPerInterval per microframe, not
    // wMaxPacketSize. Asking for mps here is what starved the old version.
    gReq = bpi ? bpi : mps;
    printf("  ep 0x%02x mps=%u burst=%u mult=%u bytesPerInterval=%u -> requesting %u/microframe\n",
           (unsigned)(num | 0x80), mps, burst, mult, bpi, gReq);

    CFRunLoopSourceRef src = nullptr;
    (*gIf)->CreateInterfaceAsyncEventSource(gIf, &src);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    for (int i = 0; i < NB; i++) {
        (*gIf)->LowLatencyCreateBuffer(gIf, (void**)&gX[i].b, (size_t)NF * gReq,
                                       kUSBLowLatencyReadBuffer);
        (*gIf)->LowLatencyCreateBuffer(gIf, (void**)&gX[i].f,
                                       NF * sizeof(IOUSBLowLatencyIsocFrame),
                                       kUSBLowLatencyFrameListBuffer);
        if (!gX[i].b || !gX[i].f) { printf("  buffer allocation failed at %d\n", i); return 6; }
    }
    UInt64 cur = 0; AbsoluteTime at;
    (*gIf)->GetBusFrameNumber(gIf, &cur, &at);
    gFrame = cur + 16;
    // A fifo here blocks until the reader opens it, which is what lets the OBS
    // pipeline be started in any order.
    if (out) gOut = fopen(out, "wb");
    if (out && !gOut) { printf("  cannot open %s\n", out); return 6; }
    double t0 = CFAbsoluteTimeGetCurrent();
    pthread_t th;
    if (gPollWanted) {
        gPoll = true;
        pthread_create(&th, nullptr, pollThread, nullptr);
        printf("  vendor maintenance loop running alongside the capture\n");
    }
    // secs <= 0 streams until interrupted, which is how the OBS pipeline runs.
    // The run loop is stopped from the signal handler rather than the capture
    // being torn down under it.
    if (secs <= 0) {
        gLoop = CFRunLoopGetCurrent();
        signal(SIGINT, onStreamInt);
        signal(SIGTERM, onStreamInt);
        printf("  streaming until interrupted ...\n");
    } else {
        printf("  streaming %d s ...\n", secs);
    }
    for (int i = 0; i < NB; i++) submit(&gX[i]);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, secs > 0 ? secs : 1e9, false);
    if (gPollWanted) { gPoll = false; pthread_join(th, nullptr);
                       printf("  maintenance cycles run: %u\n", gPollN); }
    if (gOut) fclose(gOut);
    double elapsed = CFAbsoluteTimeGetCurrent() - t0;
    if (elapsed < 0.001) elapsed = 0.001;

    printf("\n  transfers   : %u  (%u carried data, %u frame errors)\n", gN, gD, gErr);
    printf("  microframes : %u of %u carried data (%.1f%%)\n", gFrames, gN * NF,
           gN ? 100.0 * gFrames / (gN * (double)NF) : 0.0);
    printf("  packet sizes seen (first few distinct):");
    for (UInt32 i = 0; i < gNSizes && i < 8; i++) printf(" %u", gSizes[i]);
    printf("\n");
    printf("  total bytes : %llu (%.1f MB)\n", (unsigned long long)gTot, gTot / 1e6);
    if (gTot) {
        printf("  rate        : %.1f MB/s -> %s\n",
               gTot / 1e6 / (secs > 0 ? secs : elapsed), out ? out : "(discarded)");
        printf("  1080p60 YUY2 is ~249 MB/s; 1080p30 is ~124 MB/s.\n");
    } else {
        printf("  NO DATA. The pipe is healthy but the card is not producing frames.\n");
    }
    (*gIf)->SetAlternateInterface(gIf, 0);
    return gTot ? 0 : 7;
}

// --------------------------------------------------------------------- main

static void usage() {
    printf(
"usage: start <command> [options]\n"
"\n"
"  wake                  the six opening operations of the vendor cold init,\n"
"                        checked at each step. 0xC2 resets part of the card and\n"
"                        WILL drop HDMI passthrough.\n"
"  scan                  read-only. Dump all three video pages and look for a\n"
"                        received AVI InfoFrame.\n"
"  mcuw REG VAL          write one MCU register, reporting before and after.\n"
"  vidw PAGE REG VAL     write one video front end register.\n"
"  sinkedid              read the downstream display's EDID through the MCU\n"
"                        I2C proxy, as the vendor does after init.\n"
"  replay FILE [N]       replay a --seq capture verbatim, reads included.\n"
"  colour [rgb|ycbcr]    set the front end colour conversion from the source's\n"
"                        AVI InfoFrame, or force it. Run after the cold init.\n"
"  audio [secs]          read interface 1's interrupt endpoint, looking for the\n"
"                        audio path.\n"
"  vdiff                 read-only. Our register state against the vendor's\n"
"                        working session, recovered from the good capture.\n"
"  measure               read-only. The incoming signal measurement against the\n"
"                        reference from the good capture.\n"
"  format [--force]      write the video timing block. Refuses unless the\n"
"                        measurement matches the reference.\n"
"  dump                  read-only. Every register the cold init touches,\n"
"                        current value against the vendor's value.\n"
"  init [--verify]       apply the cold init. Opens with its own 0xC2 reset, so\n"
"                        it needs no wake first; wake recovers it if it goes\n"
"                        wrong, so no physical replug is needed.\n"
"  watch [secs]          read-only. Poll the signal registers and report any\n"
"                        movement, against the locked values from the capture.\n"
"  probe                 read-only. Registers, readiness value, alt settings,\n"
"                        endpoint burst sizes. Writes nothing to the card.\n"
"  arm                   arm the front end and poll until ready. No interface\n"
"                        switch, no streaming.\n"
"  stream [secs] [file]  arm, then switch to alt 2 and capture. Default 5 s to\n"
"                        stream.yuy2.\n"
"  stop                  return interface 0 to alt 0.\n"
"\n"
"options:\n"
"  --prologue            also issue the FX3 capture-start block (0xC2/0xC7/0xC1\n"
"                        and the 0x004C port writes). Off by default: it resets\n"
"                        part of the card and will drop HDMI passthrough.\n"
"  --maintain            write p0 0xB3=0xFF, p2 0x27=0xFF, p2 0x20=0x00 before\n"
"                        arming: the steady state the vendor idle loop holds.\n"
"  --poll                run the vendor maintenance cycle at 8 Hz during capture.\n"
"  --limit N             apply only the first N cold-init operations, for\n"
"                        bisecting which one breaks the input.\n"
"  --arm --post          re-enable the arming block / post-switch block. Neither\n"
"                        is needed after `replay`, and both kill the passthrough.\n"
"  --no-edid --no-tx --no-ctl   leave out part of the cold init: the 417 EDID\n"
"                        writes, the 5 HDMI transmitter writes, or the 5 bare\n"
"                        control requests.\n"
"  --alt N               alternate setting to stream on (default 2).\n"
"  --iter N              readiness poll iterations (default 1500, ~10 s).\n"
"  -v                    log every control transfer.\n");
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { usage(); return 1; }
    const char* cmd = argv[1];

    bool prologue = false, maintain = false, verify = false;
    bool force = false, doFormat = false; int alt = 2, iter = 1500, secs = 5;
    const char* out = "stream.yuy2";
    int positional = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--prologue")) prologue = true;
        else if (!strcmp(argv[i], "--maintain")) maintain = true;
        else if (!strcmp(argv[i], "--verify")) verify = true;
        else if (!strcmp(argv[i], "--poll")) gPollWanted = true;
        else if (!strcmp(argv[i], "--force")) force = true;
        else if (!strcmp(argv[i], "--rmw")) gRmw = true;
        else if (!strcmp(argv[i], "--vfix")) gVfix = true;
        else if (!strcmp(argv[i], "--nomatrix")) gNoMatrix = true;
        else if (!strcmp(argv[i], "--no-edid")) gNoEdid = true;
        else if (!strcmp(argv[i], "--no-tx"))   gNoTx = true;
        else if (!strcmp(argv[i], "--no-ctl"))  gNoCtl = true;
        else if (!strcmp(argv[i], "--no-p80")) gNoP80 = true;
        else if (!strcmp(argv[i], "--arm")) gArm = true;
        else if (!strcmp(argv[i], "--post")) gPost = true;
        else if (!strcmp(argv[i], "--limit") && i+1 < argc) gLimit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--format")) doFormat = true;
        else if (!strcmp(argv[i], "-v")) gVerbose = 1;
        else if (!strcmp(argv[i], "--alt")  && i+1 < argc) alt  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--iter") && i+1 < argc) iter = atoi(argv[++i]);
        else if (argv[i][0] == '-') { usage(); return 1; }
        else if (positional++ == 0) secs = atoi(argv[i]);
        else out = argv[i];
    }

    if (!openDevice()) return 1;
    int rc = 0;

    if (!strcmp(cmd, "probe")) {
        rc = phaseProbe();
    } else if (!strcmp(cmd, "stop")) {
        if (claimInterface0()) { (*gIf)->SetAlternateInterface(gIf, 0);
                                 printf("  interface 0 set to alt 0\n"); }
    } else if (!strcmp(cmd, "scan")) {
        if (mcuRead(0x0e) < 0) { printf("  MCU asleep, run ./start wake first\n"); rc = 2; }
        else rc = phaseScan();
    } else if (!strcmp(cmd, "mcuw") && argc >= 4) {
        int reg = (int)strtol(argv[2], nullptr, 0), val = (int)strtol(argv[3], nullptr, 0);
        int before = mcuRead((uint8_t)reg);
        IOReturn r = mcuWrite((uint8_t)reg, (uint8_t)val);
        printf("  MCU 0x%02x: was 0x%02x, wrote 0x%02x -> %s, now reads 0x%02x\n",
               reg, before, val, ioerr(r), mcuRead((uint8_t)reg));
    } else if (!strcmp(cmd, "vidw") && argc >= 5) {
        int pg = (int)strtol(argv[2], nullptr, 0), reg = (int)strtol(argv[3], nullptr, 0),
            val = (int)strtol(argv[4], nullptr, 0);
        int before = vidRead((uint8_t)pg, (uint8_t)reg);
        IOReturn r = vidWrite((uint8_t)pg, (uint8_t)reg, (uint8_t)val);
        printf("  vid p%d 0x%02x: was 0x%02x, wrote 0x%02x -> %s, now reads 0x%02x\n",
               pg, reg, before, val, ioerr(r), vidRead((uint8_t)pg, (uint8_t)reg));
    } else if (!strcmp(cmd, "sinkedid")) {
        if (mcuRead(0x0e) < 0) { printf("  MCU asleep, run ./start wake first\n"); rc = 2; }
        else rc = phaseSinkEdid();
    } else if (!strcmp(cmd, "replay") && argc >= 3) {
        rc = phaseReplay(argv[2], argc >= 4 ? atoi(argv[3]) : 0);
    } else if (!strcmp(cmd, "txdump")) {
        // read-only: HDMI transmitter (IT66121, I2C 0x9A) bank 0 registers
        for (int r = 0; r < 256; r++) {
            int v = mcuRead((uint8_t)r);
            if ((r & 15) == 0) printf("  %02x:", r);
            printf(" %02x", v < 0 ? 0xee : v);
            if ((r & 15) == 15) printf("\n");
        }
    } else if (!strcmp(cmd, "colour") || !strcmp(cmd, "color")) {
        int mode = 0;
        if (argc >= 3 && !strcmp(argv[2], "rgb"))   mode = 1;
        if (argc >= 3 && !strcmp(argv[2], "ycbcr")) mode = 2;
        rc = phaseColour(mode);
    } else if (!strcmp(cmd, "audio")) {
        rc = phaseAudio(secs);
    } else if (!strcmp(cmd, "vdiff")) {
        if (mcuRead(0x0e) < 0) { printf("  MCU asleep, run ./start wake first\n"); rc = 2; }
        else rc = phaseVdiff();
    } else if (!strcmp(cmd, "measure")) {
        if (mcuRead(0x0e) < 0) { printf("  MCU asleep, run ./start wake first\n"); rc = 2; }
        else rc = phaseMeasure();
    } else if (!strcmp(cmd, "format")) {
        if (mcuRead(0x0e) < 0) { printf("  MCU asleep, run ./start wake first\n"); rc = 2; }
        else rc = phaseFormat(force) < 0 ? 2 : 0;
    } else if (!strcmp(cmd, "dump")) {
        if (mcuRead(0x0e) < 0) printf("  MCU asleep, run ./start wake first\n");
        else rc = phaseDump();
    } else if (!strcmp(cmd, "init")) {
        rc = phaseInit(verify) < 0 ? 2 : 0;   // the sequence opens with its own 0xC2
    } else if (!strcmp(cmd, "watch")) {
        if (mcuRead(0x0e) < 0) printf("  MCU asleep, run ./start wake first\n");
        else rc = phaseWatch(secs);
    } else if (!strcmp(cmd, "wake")) {
        rc = phaseWake() < 0 ? 2 : 0;
    } else if (!strcmp(cmd, "arm") || !strcmp(cmd, "stream")) {
        if (mcuRead(0x0e) < 0) {
            printf("  MCU is not answering. Nothing to arm.\n");
            (*gDev)->USBDeviceClose(gDev); return 2;
        }
        if (prologue) phasePrologue();
        if (maintain) phaseMaintain();
        if (doFormat && phaseFormat(force) < 0) { rc = 4; goto done; }
        if (gVfix) phaseVfix();
        if (gArm) 
        phaseArm();
        int r = gArm ? phaseWaitReady(iter) : 0;
        if (r < 0) { rc = 4; }
        else {
            printf("  ready after %d iterations (%.2f s)\n", r, r * 0.007);
            if (!strcmp(cmd, "stream")) {
                if (!claimInterface0()) rc = 3;
                else rc = (alt == 4) ? phaseBulk(secs, out) : phaseStream(alt, secs, out);
            }
        }
    } else { usage(); rc = 1; }

done:
    if (gIf) (*gIf)->USBInterfaceClose(gIf);
    (*gDev)->USBDeviceClose(gDev);
    return rc;
}
