// DVFS probe for Symbian 9.2 / OMAP 2420 (reference device: Nokia N95 8GB).
//
// Changes nothing. Three tasks:
//   1. capture what the system itself reports about frequency (HAL);
//   2. measure REAL core and memory performance - an independent instrument
//      that will later prove the frequency change is real, not fabricated;
//   3. determine if the kernel-side DVFS driver is visible from user space.
//
// Item 2 is key here: all known Symbian "overclocks" failed precisely
// because they measured the label, not the actual work.
//
// Log strings are intentionally ASCII: GCCE 3.4.3 and 8-bit descriptors are not worth the risk
// of Cyrillic in literals.

#include <e32base.h>
#include <e32std.h>
#include <e32cmn.h>
#include <f32file.h>
#include <hal.h>
#include <fbs.h>
#include <bitdev.h>
#include <bitstd.h>
#include <gdi.h>

#include "ocengine.h"

_LIT(KLogFile, "E:\\ocprobe.log");
_LIT(KLddFile, "dvfs_interface.ldd");
// Registered name is taken from TFindLogicalDevice enumeration on a live device.
// The string "DVFSInterfaceDriver" inside .ldd is the factory name, not the channel;
// attempting to open using it yields KErrNotFound.
_LIT(KLddName, "dvfs_interface");
_LIT(KLddNameAlt, "DVFSInterfaceDriver");

// Power API test driver. Located in Z:\sys\bin, not loaded by default.
_LIT(KPwrFile, "d_powerapi.ldd");
_LIT(KPwrName, "TestPowerAPI");

// RomPatcher+ driver. It is NOT in ROM - the daemon pushes file0 to E:\sys\bin
// before the run. Channel name is from the string inside .ldd, verified against the
// DoControl dispatcher (func 100..105, findings 14). func 105 = single write:
//   DoControl(105, &addr, &desc) -> memmove(dest=addr, src=desc.Ptr(), len=desc.Length())
_LIT(KPatcherFile, "patcher.ldd");
_LIT(KPatcherName, "RomPatcherPlus.Driver");

static RFs gFs;
static RFile gLog;
static TUint32 gFcFreq = 0;

static void Log(const TDesC8 &aLine)
    {
    gLog.Write(aLine);
    gLog.Write(_L8("\r\n"));
    gLog.Flush();
    }

static void LogF(TRefByValue<const TDesC8> aFmt, ...)
    {
    VA_LIST list;
    VA_START(list, aFmt);
    TBuf8<512> buf;
    buf.AppendFormatList(aFmt, list);
    VA_END(list);
    Log(buf);
    }

// --- instrument -------------------------------------------------------------

// Loop with data dependency: each iteration waits for the result of the previous one,
// so execution time scales with the core clock frequency, rather than pipeline width
// or memory (working set is a single register, fits in any cache).
static TUint32 SpinAlu(TUint32 aIter)
    {
    TUint32 x = 0x12345678u;
    for (TUint32 i = 0; i < aIter; ++i)
        {
        x = x * 1664525u + 1013904223u;
        x ^= x >> 13;
        }
    return x;
    }

// Traversing a buffer larger than cache: bounded by L3/SDRAM. On OMAP2, core frequency
// and bus frequency change together, so this measurement distinguishes between the two.
// Reads EVERY word, so the byte count comes from the buffer size and no cache
// line length has to be assumed.
//
// FIXED 2026-09-13. This used to step 64 bytes and count "reads x 64", which
// assumed a 64-byte cache line. A stride sweep on the device puts the plateau
// at 32 bytes (findings 46.1), so every figure it produced was exactly twice
// the truth. The loop is unrolled by eight because the rolled version is bound
// by its own counter and branch rather than by memory.
static TUint32 SpinMem(TUint32 *aBuf, TInt aWords, TInt aPasses)
    {
    TUint32 acc = 0;
    const TInt n = aWords & ~7;
    for (TInt p = 0; p < aPasses; ++p)
        for (TInt i = 0; i < n; i += 8)
            {
            acc += aBuf[i]   + aBuf[i+1] + aBuf[i+2] + aBuf[i+3];
            acc += aBuf[i+4] + aBuf[i+5] + aBuf[i+6] + aBuf[i+7];
            }
    return acc;
    }

// ABSOLUTE frequency measurement. 256 independent additions in a row: ARM1136 is
// single-issue, each such addition costs exactly one cycle, there are no dependencies
// between them, and no I-cache misses (1 KB of code within a 16 KB cache). Thus cycles divide
// directly by time - unlike a multiplication loop where cycles per iteration
// are not known precisely and 206 cannot be distinguished from 332 MHz.
const TUint32 KNopCyclesPerIter = 258;   // 256 add + subs + bne

static void SpinNop(TUint32 aIter)
    {
    __asm__ __volatile__(
        "1:                  \n"
        ".rept 256           \n"
        "   add r3, r3, #1   \n"
        ".endr               \n"
        "   subs %0, %0, #1  \n"
        "   bne  1b          \n"
        : "+r" (aIter) : : "r3", "cc");
    }

// Microseconds between two fast counter readings.
static TUint32 ElapsedUs(TUint32 aStart, TUint32 aEnd)
    {
    TUint32 d = aEnd - aStart;      // unsigned subtraction survives wrapping
    if (gFcFreq == 0)
        return 0;
    return (TUint32)(((TUint64)d * 1000000u) / gFcFreq);
    }

// --- driver channel ---------------------------------------------------------

// Layout taken from disassembly of dvfs_interface.ldd (HandleMsg @0x80148698):
//   iValue == 0x80000000 -> channel close
//   iValue <  0          -> DoRequest, request number = ~iValue
//   iValue >= 0          -> DoControl
// DoRequest(0, status, a1, a2) leads to SetPerformance @0x80148460, where the decision
// is made EXCLUSIVELY based on a2: cmp r2,#50, greater - reserve, less than or
// equal - release. a1 is not read. The level is hardcoded as a constant (mov r0,#66),
// so this interface does not accept arbitrary frequencies.
class RDvfs : public RBusLogicalChannel
    {
public:
    TInt Open(const TDesC &aName, const TVersion &aVer)
        {
        return DoCreate(aName, aVer, KNullUnit, NULL, NULL, EOwnerProcess);
        }
    // DoControl(1, ...) @0x80148410 calls GetClockFrequency(MPU) and divides by
    // 3300000, returning the percentage of nominal. Remaining opcodes return 0.
    TInt PerfPercent() { return DoControl(1, (TAny *)0); }
    // DoControl is protected in RBusLogicalChannel, so expose it explicitly.
    TInt Ctl(TInt aFunc) { return DoControl(aFunc, (TAny *)0); }
    // After the READ patch, DoControl(1) returns r0 from ldr [a1]. TInt here is only
    // transport: the caller immediately casts the result to TUint32.
    TInt ReadRaw(TUint32 aAddress) { return DoControl(1, (TAny *)aAddress); }
    // After the EXEC patch, the same DoControl(1) becomes "bx r1", i.e., calling code
    // at address a1 in kernel mode. A distinct name so the call site makes it
    // obvious which patch is currently applied.
    TInt Call(TUint32 aAddress) { return DoControl(1, (TAny *)aAddress); }
    // The driver completes the request via Kern::RequestComplete, so wait for the status.
    TInt SetPerformance(TInt aValue)
        {
        TRequestStatus s;
        DoRequest(0, s, (TAny *)0, (TAny *)aValue);
        User::WaitForRequest(s);
        return s.Int();
        }
    };

// Layout from disassembly of d_powerapi.ldd (HandleMsg @0x80148FB8):
//   r0 = ~iValue           -> request number, i.e., sub-action (0 = acquire,
//                             1 = read state, 3 = enumerate)
//   r1 = msg[32] = a1      -> jump table index, 0..12
//   result is returned via Kern::RequestComplete into TRequestStatus
// Indices: 1..6 -> resources 1..5, 6 -> 65, 7 -> 66 (CPU performance),
//          8 -> 67, 9 -> 69, 10 -> 70.
// Static, not on the stack: if a request does not complete and execution continues, the driver
// will eventually write to this address. A stack variable by that time would already be foreign memory.
static TRequestStatus gPwrStatus;

class RPwr : public RBusLogicalChannel
    {
public:
    TInt Open(const TVersion &aVer)
        {
        return DoCreate(KPwrName, aVer, KNullUnit, NULL, NULL, EOwnerProcess);
        }
    // Returns KErrTimedOut if the driver does not respond within aMs. There is no way
    // to cancel the request - the driver lacks Cancel, - so after a timeout the caller must
    // stop using the channel rather than sending another request.
    TInt Req(TInt aSubOp, TInt aIndex, TInt aMs)
        {
        gPwrStatus = KRequestPending;
        DoRequest(aSubOp, gPwrStatus, (TAny *)aIndex, (TAny *)0);

        RTimer timer;
        if (timer.CreateLocal() != KErrNone)
            {                                   // better not risk running without a timer
            return KErrGeneral;
            }
        TRequestStatus ts;
        timer.After(ts, aMs * 1000);
        User::WaitForRequest(gPwrStatus, ts);

        if (gPwrStatus == KRequestPending)
            {
            timer.Close();
            return KErrTimedOut;
            }
        timer.Cancel();
        User::WaitForRequest(ts);
        timer.Close();
        return gPwrStatus.Int();
        }
    };

// Read-only: sub-action 1 for ONE index per run.
//
// Iterating through all indices consecutively resulted in a hang: index=6 (resource 65)
// never completes the request. There is no way to cancel it - the driver lacks Cancel, - so
// the only reliable way to proceed is a new process per index: the hanging
// request dies with the process instead of lingering into the next attempt.
static void ProbePowerApi(TInt aIndex)
    {
    LogF(_L8("--- TestPowerAPI: index %d (read-only) ---"), aIndex);
    TInt r = User::LoadLogicalDevice(KPwrFile);
    LogF(_L8("LoadLogicalDevice(d_powerapi.ldd) -> %d   [0=ok, -11=already loaded]"), r);
    if (r != KErrNone && r != KErrAlreadyExists)
        {
        Log(_L8("  driver not loadable - skipped"));
        return;
        }

    RPwr ch;
    TInt o = ch.Open(TVersion(0,0,0));
    LogF(_L8("DoCreate(TestPowerAPI, v0.0.0) -> %d"), o);
    if (o != KErrNone)
        return;

    TInt v = ch.Req(1, aIndex, 3000);
    LogF(_L8("  get(index=%d) -> %d"), aIndex, v);
    if (v == KErrTimedOut)
        {
        Log(_L8("  request never completed - channel left open on purpose"));
        return;                         // do NOT close channel: request is still pending
        }

    ch.Close();
    Log(_L8("channel closed"));
    }

// --- overclock via shared engine -------------------------------------------
//
// All delicate logic - READ/EXEC patches, chain to SRAM copy, and relock blob -
// lives in ocengine.cpp and is shared with the ocgui application. Having two
// copies of this sequence is unacceptable: diverging, they hang the device.
// Only the measurement scenario remains here.

static TUint32 MeasureMhz();          // defined below, in the instrument section

// A single measurement has several percent jitter: interrupts and foreign
// threads interfere. But interference can only SLOW DOWN a run, never speed it up,
// so the best of a series is the cleanest, not "luck".
static TUint32 BestMhz(TInt aRuns)
    {
    TUint32 best = 0;
    for (TInt i = 0; i < aRuns; ++i)
        {
        TUint32 v = MeasureMhz();
        if (v > best)
            best = v;
        }
    return best;
    }

static TUint32 BestAlu(TInt aRuns, TUint32 aIter)
    {
    TUint32 best = 0;
    for (TInt i = 0; i < aRuns; ++i)
        {
        TUint32 t0 = User::FastCounter();
        TUint32 sink = SpinAlu(aIter);
        TUint32 t1 = User::FastCounter();
        TUint32 us = ElapsedUs(t0, t1);
        TUint32 m100 = us ? (TUint32)(((TUint64)aIter * 100u) / us) : 0;
        if (sink == 0)
            m100 ^= 1;
        if (m100 > best)
            best = m100;
        }
    return best;
    }

static TUint32 BestMem(TInt aRuns)
    {
    const TInt kWords = 512 * 1024;               // 2 MB - clearly bypasses cache
    TUint32 *buf = (TUint32 *)User::Alloc(kWords * sizeof(TUint32));
    if (!buf)
        return 0;
    TInt i;
    for (i = 0; i < kWords; ++i)
        buf[i] = (TUint32)i;
    TUint32 best = 0;
    for (TInt r = 0; r < aRuns; ++r)
        {
        TUint32 t0 = User::FastCounter();
        TUint32 sink = SpinMem(buf, kWords, 8);
        TUint32 t1 = User::FastCounter();
        TUint32 us = ElapsedUs(t0, t1);
        const TUint64 bytes = (TUint64)(kWords & ~7) * 4u * 8u;
        TUint32 mbs = us ? (TUint32)(bytes / us) : 0;
        if (sink == 0)
            mbs ^= 1;
        if (mbs > best)
            best = mbs;
        }
    User::Free(buf);
    return best;
    }

static TInt MemoryIntegrity(TInt aWords)
    {
    TUint32 *buf = (TUint32 *)User::Alloc(aWords * sizeof(TUint32));
    if (!buf)
        return KErrNoMemory;
    TUint32 x = 0x2545f491u;
    TInt i;
    for (i = 0; i < aWords; ++i)
        {
        x = x * 1664525u + 1013904223u;
        buf[i] = x;
        }
    x = 0x2545f491u;
    TInt bad = 0;
    for (i = 0; i < aWords; ++i)
        {
        x = x * 1664525u + 1013904223u;
        if (buf[i] != x)
            ++bad;
        }
    User::Free(buf);
    return bad;
    }

static void LogClocks(const TDesC8 &aTag, const TOcClocks &aC)
    {
    LogF(_L8("  %S: m=%d DPLL=%u CORE=%u MPU=%u BUS=%u DSS1=%u kHz (l3div=%d dssdiv=%d)"),
         &aTag, aC.iMult, aC.DpllKhz(), aC.CoreKhz(), aC.MpuKhz(), aC.L3Khz(),
         aC.Dss1Khz(), aC.iL3Div, aC.iDss1Div);
    }

// Shows EXACTLY what will be written to CM_CLKSEL1_CORE and what frequencies
// will result. The main thing here is DSS1: shifting it broke rendering (findings §21).
static void LogCompensation(COcEngine *aE, TInt aMult)
    {
    TUint32 newCore = aE->CompensatedCore(aMult);
    TOcClocks c;
    aE->ClocksFor(aMult, c);
    // Recalculate with NEW dividers.
    TOcClocks comp = c;
    comp.iL3Div = (TInt)(newCore & KCoreL3Mask);
    comp.iDss1Div = (TInt)((newCore & KCoreDss1Mask) >> KCoreDss1Shift);
    LogF(_L8("  CLKSEL1_CORE %08x -> %08x"), aE->StockCore(), newCore);
    LogClocks(_L8("compensated"), comp);
    const TOcClocks &st = aE->StockClocks();
    TInt dp = st.Dss1Khz() ? (TInt)(((TInt64)comp.Dss1Khz() - st.Dss1Khz()) * 1000
                                    / st.Dss1Khz()) : 0;
    TInt lp = st.L3Khz() ? (TInt)(((TInt64)comp.L3Khz() - st.L3Khz()) * 1000
                                  / st.L3Khz()) : 0;
    LogF(_L8("  vs stock: DSS1 %d.%d%%  BUS %d.%d%%  (0 = display untouched)"),
         dp / 10, (dp < 0 ? -dp : dp) % 10, lp / 10, (lp < 0 ? -lp : lp) % 10);
    }

// Snapshot of all four layout registers in ONE application of the READ patch:
// otherwise the fields would be sampled at different times, while the governor moves them at runtime.
struct TClockSnap
    {
    TUint32 iSel1;
    TUint32 iSel2;
    TUint32 iMpu;
    TUint32 iCore;
    TUint32 iRfr;      // SDRC_RFR_CTRL0
    };

// SDRC_RFR_CTRL0 is sampled together with the rest intentionally. Its ARCV field counts
// L3 clock cycles, so with an unchanged ARCV, a slower bus stretches the auto-refresh
// interval in real time (§26.1a). The question of whether firmware adjusts ARCV on an
// OPP transition remained open precisely because we only read this register between
// runs, rather than when the governor sits at a lower OPP. Now
// it is in the same snapshot as CM_CLKSEL2_PLL, so the answer is directly visible.
static TInt SnapClocks(COcEngine *aE, TClockSnap &aOut)
    {
    const TUint32 regs[5] =
        { KRegClkSel1Pll, KRegClkSel2Pll, KRegClkSelMpu, KRegClkSel1Core,
          KRegSdrcRfr0 };
    TUint32 v[5] = { 0, 0, 0, 0, 0 };
    TInt r = aE->ReadRegs(regs, v, 5);
    aOut.iSel1 = v[0];
    aOut.iSel2 = v[1];
    aOut.iMpu  = v[2];
    aOut.iCore = v[3];
    aOut.iRfr  = v[4];
    return r;
    }

// The two frequencies from this snapshot - MPU and L3 - are what this is all about,
// so we print both raw words and decoded values. CORE here is calculated from the INSTANTANEOUS
// CM_CLKSEL2_PLL, which toggles between 1 and 2 under the governor (findings §12, §18):
// a reading with sel2=1 merely means the snapshot caught a lower OPP, not that the layout
// was removed.
static void LogSnap(const TDesC8 &aTag, const TClockSnap &aS)
    {
    TOcClocks c;
    c.Decode(aS.iSel1, aS.iSel2, aS.iMpu, aS.iCore);
    LogF(_L8("  %S: PLL=%08x SEL2=%08x MPU=%08x CORE=%08x RFR=%08x"),
         &aTag, aS.iSel1, aS.iSel2, aS.iMpu, aS.iCore, aS.iRfr);
    // Auto-refresh interval in nanoseconds: ARCV bus cycles. We print the actual
    // time rather than the raw field: the question is whether we fit within the charge
    // retention window, not what the register holds.
    const TUint32 arcv = (aS.iRfr >> 8) & 0xffffu;
    const TUint32 busKhz = c.L3Khz();
    const TUint32 refrNs = busKhz ? (TUint32)(((TUint64)arcv * 1000000u) / busKhz) : 0;
    LogF(_L8("  %S: m=%d mpudiv=%d l3div=%d dssdiv=%d sel2=%d -> CORE=%u MPU=%u BUS=%u DSS1=%u kHz"),
         &aTag, c.iMult, c.iMpuDiv, c.iL3Div, c.iDss1Div, c.iCoreSel,
         c.CoreKhz(), c.MpuKhz(), c.L3Khz(), c.Dss1Khz());
    LogF(_L8("  %S: refresh arcv=%u -> %u ns (stock 7520 ns; longer = charge loss risk)"),
         &aTag, arcv, refrNs);
    }

// Layout holds as long as neither the multiplier nor either of the two dividers has shifted.
// We intentionally do NOT compare CM_CLKSEL2_PLL: its movement is normal governor operation,
// not a loss of our configuration.
static TBool SameLayout(const TClockSnap &aA, const TClockSnap &aB)
    {
    return ((aA.iSel1 >> 12) & 0x3ff) == ((aB.iSel1 >> 12) & 0x3ff) &&
           (aA.iMpu & KMpuDivMask) == (aB.iMpu & KMpuDivMask) &&
           (aA.iCore & (KCoreL3Mask | KCoreDss1Mask)) ==
           (aB.iCore & (KCoreL3Mask | KCoreDss1Mask));
    }

// Selected device profile. Enabled in E32Main when the word "e90" is present in
// the command. Defaults to RM-320. The engine takes addresses from the profile, so a wrong
// profile on another device is not just incorrect numbers, but patching into a foreign kernel; therefore
// the selection is explicit and unique.
static TBool gUseE90Profile = EFalse;

static COcEngine *PrepareEngine()
    {
    COcEngine *e = NULL;
    TRAPD(err, e = COcEngine::NewL());
    if (err != KErrNone || !e)
        {
        LogF(_L8("engine alloc failed: %d"), err);
        return NULL;
        }
    // Profile is selected according to device. The word "e90" remains an EXPLICIT override
    // for verification, but default behavior is auto-selection: otherwise the same
    // "patcher oc 111" launched on E90 without this word would take RM-320 addresses
    // and patch them into a foreign kernel.
    const TOcProfile* prof = gUseE90Profile ? &KProfileE90 : ProfileForThisDevice();
    if (!prof)
        {
        TInt uid = 0;
        HAL::Get(HALData::EMachineUid, uid);
        LogF(_L8("unknown device (machine uid %08x) - no profile, refusing"),
             (TUint32)uid);
        delete e;
        return NULL;
        }
    e->SetProfile(*prof);
    LogF(_L8("profile: %s%s"), prof->iId,
         gUseE90Profile ? "  (forced by command line)" : "  (auto by machine uid)");
    TInt r = e->Prepare();
    if (r != KErrNone)
        {
        TBuf8<64> step;
        step.Copy(COcEngine::StepName(e->FailedAt()));
        LogF(_L8("engine NOT ready: err %d at step [%S]"), r, &step);
        {
        TBuf8<64> fw;
        fw.Copy(e->Firmware());
        LogF(_L8("  device firmware read as [%S] (empty = SysUtil failed)"), &fw);
        }
        delete e;
        return NULL;
        }
    LogF(_L8("engine ready: SRAM @%08x, stock mult %d"),
         e->SramAddress(), e->StockMult());
    LogF(_L8("orig body (%d words, live ROM): %08x %08x %08x %08x %08x %08x"),
         e->OrigCount(), e->OrigWord(0), e->OrigWord(1), e->OrigWord(2),
         e->OrigWord(3), e->OrigWord(4), e->OrigWord(5));
    LogClocks(_L8("stock"), e->StockClocks());
    // The reference point is fixed (KStockClkSel1Core), so any deviation from it must
    // be immediately visible in the log. Previously, this case quietly became the new norm:
    // a "raise - lower" cycle shifted dssdiv by one step each time while reporting
    // that the display was untouched.
    if (e->BootedNonStock())
        LogF(_L8("  NOT STOCK AT START: m=%d CLKSEL1_CORE=%08x CLKSEL_MPU=%08x"
                 "  (stock: m=%d %08x %08x)"),
              e->BootMult(), e->BootCore(), e->BootMpu(),
              e->Profile().iStockMult, e->Profile().iStockClkSel1Core,
              e->Profile().iStockClkSelMpu);
    return e;
    }

// Headless engine verification: the same sequence executed by the application,
// but without a UI. Changes nothing.
static void ProbeEngine()
    {
    Log(_L8("--- engine self-check (read only) ---"));
    COcEngine *e = PrepareEngine();
    if (!e)
        return;
    const TUint32 regs[] =
        { KRegClkSel1Pll, KRegClkSel2Pll, KRegClkSelMpu, KRegClkEnPll,
          KRegIdlestPll, KRegClkSel1Core, KRegSdrcRfr0, KRegSdrcActimA };
    const char *names[] =
        { "CM_CLKSEL1_PLL ", "CM_CLKSEL2_PLL ", "CM_CLKSEL_MPU  ",
          "CM_CLKEN_PLL   ", "CM_IDLEST_PLL  ", "CM_CLKSEL1_CORE",
          "SDRC_RFR_CTRL0 ", "SDRC_ACTIM_A_0 " };
    for (TInt i = 0; i < 8; ++i)
        {
        TUint32 v = 0;
        e->ReadReg(regs[i], v);
        TPtrC8 nm((const TUint8 *)names[i]);
        LogF(_L8("  %S @%08x = %08x"), &nm, regs[i], v);
        }
    // Most important line of this run: whether the patch is removed. Nonzero percentage
    // means stock code is back in the kernel and no landmine remains after process
    // exit. Overwriting bytes is not enough for this - unshadow is required.
    e->EndPatchSession();
    TInt pct = e->VerifyRestored();
    LogF(_L8("  restore check: stock DoControl(1,0) -> %d  (nonzero = original code back)"),
         pct);
    delete e;
    Log(_L8("engine self-check done"));
    }

// Test ARM code blob for execution in kernel mode.
// Consists of two instructions and a constant in the literal pool:
//   0x00: ldr r0, [pc, #0]   ; loads constant at address PC+8 = 0x08 (0xe59f0000)
//   0x04: bx  lr             ; return (0xe12fff1e)
//   0x08: .word 0x5a5a0001   ; constant (0x5a5a0001)
// On ARM1136 (ARMv6), movw/movt instructions are absent, so constant is fetched from the pool.
static void ProbeBlobTest()
    {
    Log(_L8("--- custom kernel blob test ---"));
    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    static const TUint8 kTestBlob[] =
        {
        0x00, 0x00, 0x9f, 0xe5,   // ldr r0, [pc, #0] ; loads @PC+8
        0x1e, 0xff, 0x2f, 0xe1,   // bx  lr
        0x01, 0x00, 0x5a, 0x5a    // .word 0x5a5a0001
        };
    TPtrC8 blob(kTestBlob, sizeof(kTestBlob));

    TInt result = 0;
    TInt r = e->ExecCustomBlob(blob, result);
    LogF(_L8("  ExecCustomBlob -> r=%d, result=0x%08x (%d)"), r, result, result);

    const TUint32 expected = 0x5a5a0001u;
    if (r == KErrNone && (TUint32)result == expected)
        Log(_L8("  MATCH: custom blob executed in kernel mode and returned expected 0x5a5a0001"));
    else
        LogF(_L8("  MISMATCH: expected 0x%08x, got 0x%08x (r=%d)"), expected, result, r);

    // Remove patch and verify kernel code restoration
    e->EndPatchSession();
    TInt pct = e->VerifyRestored();
    LogF(_L8("  restore check: stock DoControl(1,0) -> %d  (nonzero = original code back)"),
         pct);

    delete e;
    Log(_L8("custom blob test done"));
    }

// ARM1136 performance counters (CP15 c15 c12). The question answered by
// this run: do these registers count at all on this device, or does firmware
// keep PMU disabled and writing to PMCR does nothing.
//
// Two blobs. The first writes configuration to PMCR: bit 0 - enable, 1 - reset
// PMN0/PMN1, 2 - reset CCNT, 3 - divide CCNT by 64 (without divider, cycle counter
// overflows in ten seconds), [19:12] - PMN0 event, [27:20] -
// PMN1 event. The second reads all three counters; only one word can be returned
// in r0, so the rest is placed in the blob's own literal pool, i.e., in SRAM, where it
// is later visible via the READ patch.
//
// ARM1136 events: 0x07 - instructions executed, 0x0b - data cache miss.
static void ProbePmuTest()
    {
    Log(_L8("--- ARM1136 performance counters ---"));
    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    // ldr r0,[pc,#8] / mcr p15,0,r0,c15,c12,0 / mov r0,#0 / bx lr / .word cfg
    TUint8 setup[] =
        {
        0x08, 0x00, 0x9f, 0xe5,
        0x1c, 0x0f, 0x0f, 0xee,
        0x00, 0x00, 0xa0, 0xe3,
        0x1e, 0xff, 0x2f, 0xe1,
        0x00, 0x00, 0x00, 0x00
        };
    // Both counters are set to the same event 0x07 (instructions executed): if
    // the labels in the blob are correct, both numbers must match. A discrepancy
    // would mean that fields evtCount0 and evtCount1 are mapped in reverse.
    const TUint32 cfg = (0x07u << 20) | (0x07u << 12) | 0x0fu;
    setup[16] = (TUint8)(cfg & 0xff);
    setup[17] = (TUint8)((cfg >> 8) & 0xff);
    setup[18] = (TUint8)((cfg >> 16) & 0xff);
    setup[19] = (TUint8)((cfg >> 24) & 0xff);

    // mrc CCNT/PMN0/PMN1 / add r0,pc,#8 / stmia r0,{r1,r2,r3} / mov r0,r1 / bx lr
    static const TUint8 kRead[] =
        {
        0x3c, 0x1f, 0x1f, 0xee,
        0x5c, 0x2f, 0x1f, 0xee,
        0x7c, 0x3f, 0x1f, 0xee,
        0x08, 0x00, 0x8f, 0xe2,
        0x0e, 0x00, 0x80, 0xe8,
        0x01, 0x00, 0xa0, 0xe1,
        0x1e, 0xff, 0x2f, 0xe1,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
        };

    TInt res = 0;
    TPtrC8 sblob(setup, sizeof(setup));
    TInt r = e->ExecCustomBlob(sblob, res);
    LogF(_L8("  PMCR write cfg=0x%08x -> r=%d"), cfg, r);

    TPtrC8 rblob(kRead, sizeof(kRead));
    r = e->ExecCustomBlob(rblob, res);
    LogF(_L8("  first read -> r=%d ccnt=%u"), r, (TUint32)res);
    const TUint32 first = (TUint32)res;

    // Compute something so there is visible work between the two snapshots.
    volatile TUint32 acc = 0;
    for (TInt i = 0; i < 2000000; i++)
        acc += (TUint32)i;
    User::After(100000);

    r = e->ExecCustomBlob(rblob, res);
    LogF(_L8("  second read -> r=%d ccnt=%u delta=%u"),
         r, (TUint32)res, (TUint32)res - first);

    // The rest of the snapshot resides in the blob pool, i.e., in SRAM at offset 28 bytes.
    const TUint32 base = e->SramAddress() + 0x800u + 28u;
    const TUint32 addr[3] = { base, base + 4, base + 8 };
    TUint32 out[3] = { 0, 0, 0 };
    const TInt rr = e->ReadRegs(addr, out, 3);
    LogF(_L8("  sram snapshot r=%d ccnt=%u instr=%u dcmiss=%u"),
         rr, out[0], out[1], out[2]);
    if (out[0] == 0 && out[1] == 0 && out[2] == 0)
        Log(_L8("  ALL ZERO: the counters are not running"));
    else
        Log(_L8("  COUNTERS RUNNING"));

    e->EndPatchSession();
    LogF(_L8("  restore check -> %d"), e->VerifyRestored());
    delete e;
    Log(_L8("pmu test done"));
    }


// DSP domain: READ-ONLY, no writes to PRCM. Task - docs/TASK_DSP_CLOCK.md.
//
// The question answered by this run: in which field of CM_CLKSEL_DSP is the
// C55x divider located and is it really 6 (i.e., DSP at 110 MHz instead of nominal 220).
//
// Why raw value specifically. Field layout was derived from two setters in
// ehwaomap.pdd, neither of which touches the low field, so reversing the
// setters alone does not show which field clocks the C55x core. But it is visible in power.dll: ord60
// @0x800B1644 masks exactly 0xF and divides CORE by this value - this is the number driver
// ehwaomap.pdd stores into symbol _BRIDGEINIT_DSP_FREQ, and its bimodality
// (110000/220000) was observed by the sibling project. Thus we compute here the same thing as
// firmware and verify against its own number.
static void ProbeDsp(TInt aSamples)
    {
    Log(_L8("--- DSP clock domain probe (READ ONLY, no PRCM writes) ---"));
    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    const TUint32 regs[] =
        { KRegFclkenDsp, KRegIclkenDsp, KRegIdlestDsp, KRegClkSelDsp };
    const char *names[] =
        { "CM_FCLKEN_DSP", "CM_ICLKEN_DSP", "CM_IDLEST_DSP", "CM_CLKSEL_DSP" };
    TUint32 v[4] = { 0, 0, 0, 0 };
    // All four within a single patch application: a single window, and a consistent snapshot
    // (domain state and divider are read virtually simultaneously).
    TInt r = e->ReadRegs(regs, v, 4);
    if (r != KErrNone)
        {
        LogF(_L8("ReadRegs failed: %d"), r);
        delete e;
        return;
        }
    for (TInt i = 0; i < 4; ++i)
        {
        TPtrC8 nm((const TUint8 *)names[i]);
        LogF(_L8("  %S @%08x = %08x"), &nm, regs[i], v[i]);
        }

    const TUint32 sel = v[3];
    const TInt fwDiv = (TInt)(sel & KDspFwDivMask);          // [3:0] - as seen by ord60
    const TInt dsDiv = (TInt)(sel & KDspDivMask);            // [4:0] - as in datasheet
    const TInt ifDiv = (TInt)((sel & KDspIfMask) >> 5);
    const TInt bit7  = (TInt)((sel & KDspBit7Mask) >> 7);
    const TInt iva   = (TInt)((sel & KDspIvaMask) >> 8);
    const TInt bit13 = (TInt)((sel & KDspBit13Mask) >> 13);
    LogF(_L8("  fields: DSP[3:0]=%d [4:0]=%d  IF[6:5]=%d  bit7=%d  IVA[12:8]=%d  bit13=%d"),
         fwDiv, dsDiv, ifDiv, bit7, iva, bit13);

    const TUint32 coreKhz = e->StockClocks().CoreKhz();
    if (fwDiv)
        LogF(_L8("  firmware math: CORE %u kHz / [3:0]=%d -> %u kHz (this is _BRIDGEINIT_DSP_FREQ)"),
             coreKhz, fwDiv, coreKhz / (TUint32)fwDiv);
    else
        Log(_L8("  firmware math: [3:0] is zero -> ord60 returns 0 (domain divider not set)"));

    // Multiple samples: observed value is bimodal, so a single read could
    // catch the domain in either state and mislead conclusions. Each
    // sample is a separate short patch window; keeping the patch applied for several
    // seconds would be worse because total time with a landmine in kernel for
    // foreign DoControl(1) calls would become orders of magnitude larger.
    if (aSamples > 1)
        {
        LogF(_L8("  sampling CM_CLKSEL_DSP %dx, 250 ms apart:"), aSamples);
        TUint32 seen[8];
        TInt seenCount = 0;
        for (TInt s = 0; s < aSamples; ++s)
            {
            TUint32 pair[2] = { 0, 0 };
            const TUint32 addr[2] = { KRegClkSelDsp, KRegIdlestDsp };
            if (e->ReadRegs(addr, pair, 2) != KErrNone)
                break;
            LogF(_L8("    [%2d] CLKSEL=%08x ([3:0]=%d)  IDLEST=%08x"),
                 s, pair[0], (TInt)(pair[0] & KDspFwDivMask), pair[1]);
            TBool known = EFalse;
            for (TInt k = 0; k < seenCount; ++k)
                if (seen[k] == pair[0])
                    known = ETrue;
            if (!known && seenCount < 8)
                seen[seenCount++] = pair[0];
            User::After(250000);
            }
        LogF(_L8("  distinct CM_CLKSEL_DSP values seen: %d"), seenCount);
        for (TInt k = 0; k < seenCount; ++k)
            LogF(_L8("    %08x -> [3:0]=%d"), seen[k], (TInt)(seen[k] & KDspFwDivMask));
        }

    // Verdict is formulated here, rather than later from the log: a number without interpretation
    // is too easy to read backwards.
    if (fwDiv == 6)
        Log(_L8("  VERDICT: divider is 6 -> DSP runs at half rate. Step 2 (6->3) is justified."));
    else if (fwDiv == 3)
        Log(_L8("  VERDICT: divider is 3 -> DSP already at nominal. Do NOT write; slowness is elsewhere."));
    else
        LogF(_L8("  VERDICT: unexpected divider %d - do NOT write, report raw value first."), fwDiv);
    if (dsDiv != fwDiv)
        Log(_L8("  WARNING: bit [4] is set, so ord60 (mask 0xF) misreports the DSP frequency."));

    e->EndPatchSession();
    TInt pct = e->VerifyRestored();
    LogF(_L8("  restore check: stock DoControl(1,0) -> %d  (nonzero = original code back)"),
         pct);
    delete e;
    Log(_L8("DSP probe done"));
    }

// Changing the DSP divider. Called ONLY with an explicit argument.
//
// Risk is localized to the DSP domain: CORE, SDRAM, display, and USB are unaffected;
// PRCM_CLKCFG_CTRL latch is not needed. In the worst case, audio breaks until
// reboot - DSP/BIOS in the already loaded base image configured
// its timers for the old frequency.
//
// We do NOT leave the divider: the run finishes by restoring stock. To keep it,
// there is a separate command "patcher dsphold".
// Soak mode. Three variants exist not for convenience, but to DISTINGUISH
// reasons for reboot with a non-stock divider (findings 24.5):
//   ESoakPoll  - wake up once every 5 s. System does not manage to enter deep
//                idle. This is exactly how the only clean 120 s soak succeeded;
//   ESoakQuiet - single long User::After, no wakeups. The divider is held by
//                OUR process, but nothing prevents the system from entering retention.
//                If it crashes here, but not under ESoakPoll - the issue is idle, not
//                who owns the divider;
//   ESoakRun   - inside the window we launch a child process (DSP benchmark).
//                We hold the divider the entire time and are guaranteed to restore it, so
//                this is the only way to measure DSP under load without leaving
//                a non-stock divider in the system.
enum TSoakMode
    {
    ESoakPoll = 0,
    ESoakQuiet,
    ESoakRun
    };

// Launches a process and waits for it, but NOT longer than the cap. Without a cap, a hung
// child would also hold our process, and with it - the non-stock divider, which is exactly
// the condition we avoid.
static TInt RunChild(const TDesC& aExe, const TDesC& aArgs, TInt aCapSec)
    {
    RProcess p;
    TInt r = p.Create(aExe, aArgs);
    if (r != KErrNone)
        {
        LogF(_L8("  child: create failed %d"), r);
        return r;
        }
    TRequestStatus done;
    p.Logon(done);
    RTimer timer;
    r = timer.CreateLocal();
    if (r != KErrNone)
        {
        p.Kill(KErrNone);
        p.Close();
        return r;
        }
    TRequestStatus cap;
    timer.After(cap, aCapSec * 1000000);
    p.Resume();
    Log(_L8("  child: started"));

    User::WaitForRequest(done, cap);
    TInt exitCode = KErrNone;
    if (done != KRequestPending)
        {
        timer.Cancel();
        User::WaitForRequest(cap);
        exitCode = done.Int();
        LogF(_L8("  child: exited, status=%d"), exitCode);
        }
    else
        {
        // Cap expired. Kill the child and wait for it - otherwise it
        // outlives us along with the divider.
        Log(_L8("  child: TIMED OUT, killing"));
        p.Kill(KErrTimedOut);
        User::WaitForRequest(done);
        exitCode = KErrTimedOut;
        }
    timer.Close();
    p.Close();
    return exitCode;
    }

static void ProbeDspSet(TInt aDiv, TBool aKeep, TInt aSoakSec, TInt aMode)
    {
    LogF(_L8("--- DSP divider set: %d, keep=%d, soak=%d s, mode=%d ---"),
         aDiv, aKeep ? 1 : 0, aSoakSec, aMode);
    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    TInt before = -1;
    if (e->LiveDspDiv(before) != KErrNone)
        {
        Log(_L8("cannot read CM_CLKSEL_DSP - aborting"));
        delete e;
        return;
        }
    LogF(_L8("  before: div=%d -> DSP %u kHz  (stock div=%d)"),
         before, e->DspKhz(before), e->StockDspDiv());

    TInt rc = KErrGeneral;
    TInt r = e->ApplyDspDiv(aDiv, rc);
    LogF(_L8("  ApplyDspDiv(%d) -> r=%d, firmware setter rc=%d"), aDiv, r, rc);
    if (r != KErrNone)
        {
        Log(_L8("  not applied (our own validation or patch path)"));
        delete e;
        return;
        }
    if (rc == KErrArgument)
        {
        // -6: value was rejected by firmware itself. This is not a failure of our path, but
        // its own guards triggering.
        Log(_L8("  firmware REJECTED the value; register untouched"));
        delete e;
        return;
        }
    if (rc != KErrNone)
        {
        LogF(_L8("  blob returned %d (1 = PRCM_CLKCFG_CTRL latch never cleared)"), rc);
        delete e;
        return;
        }

    // Read back rather than trusting what was written - project rule.
    TInt after = -1;
    e->LiveDspDiv(after);
    LogF(_L8("  after:  div=%d -> DSP %u kHz"), after, e->DspKhz(after));
    if (after != aDiv)
        Log(_L8("  MISMATCH: register did not take the value"));
    else
        Log(_L8("  register confirmed by read-back"));

    // Soak. Every line is written to file with Flush, so if the device dies
    // in the middle of it, the log on E: survives and shows at what second it happened -
    // which is why the soak is split into steps rather than done in a single User::After.
    const TInt soak = (aSoakSec > 0) ? aSoakSec : 3;
    if (aMode == ESoakQuiet)
        {
        // No wakeups: exactly one sleep for the entire soak. If reboot
        // is caused by deep idle, it must happen right here.
        LogF(_L8("  soak %d s QUIET - single sleep, no wakeups"), soak);
        User::After(soak * 1000000);
        TInt still = -1;
        e->LiveDspDiv(still);
        LogF(_L8("  quiet soak survived: div=%d ntick=%u"), still,
             User::NTickCount());
        }
    else if (aMode == ESoakRun)
        {
        LogF(_L8("  soak %d s with CHILD process - divider held by us"), soak);
        _LIT(KChild, "E:\\sys\\bin\\dsptest.exe");
        _LIT(KChildArgs, "baseonlydcdopnobench");
        TInt before2 = -1;
        e->LiveDspDiv(before2);
        LogF(_L8("  div before child: %d"), before2);
        RunChild(KChild, KChildArgs, soak);
        TInt still = -1;
        TInt rr = e->LiveDspDiv(still);
        LogF(_L8("  div after child: %d (read r=%d) ntick=%u"), still, rr,
             User::NTickCount());
        }
    else
        {
        LogF(_L8("  soak %d s, sampling every 5 s (log is flushed per line)"),
             soak);
        for (TInt t = 0; t < soak; t += 5)
            {
            User::After(5000000);
            TInt still = -1;
            TInt rr = e->LiveDspDiv(still);
            LogF(_L8("    t=%3d s: div=%d (read r=%d) ntick=%u"),
                 t + 5, still, rr, User::NTickCount());
            }
        }
    Log(_L8("  soak finished, system alive"));

    if (aKeep)
        {
        e->KeepOnExit(ETrue);
        Log(_L8("  KEEPING the new divider (reboot restores stock)"));
        // Not advice, but a measured fact: two reboots out of two, one of them in complete
        // idle, with zero DSP load (findings 24.5). Meanwhile, a soak with
        // restoration within the process passes cleanly.
        if (aDiv != e->StockDspDiv())
            Log(_L8("  WARNING: leaving a non-stock DSP divider rebooted the "
                    "device 2/2 times - use 'dspset <d> <sec>' instead"));
        }
    delete e;
    Log(_L8("DSP divider run done"));
    }

// Bisecting reboot cause. One level per run; after each,
// the device should be left idle to see if it survives.
// Verifying power-down global discovery where the correct answer is known from
// the profile. Prints found value alongside expected - a match means the mechanism
// relied upon by test profiles works.
static void ProbeDiscover()
    {
    Log(_L8("--- discover pwrdown global (mechanism check) ---"));
    COcEngine *e = PrepareEngine();
    if (!e)
        return;
    TUint32 found = 0, sram = 0;
    TInt r = e->TryDiscoverGlobal(found, sram);
    LogF(_L8("  search -> r=%d  found=%08x  sram=%08x"), r, found, sram);
    LogF(_L8("  engine resolved sram=%08x"), e->SramAddress());
    if (r == KErrNone && sram == e->SramAddress())
        Log(_L8("  MECHANISM OK: search reproduced the known SRAM address"));
    else
        Log(_L8("  MECHANISM FAILED: search did not reproduce the known answer"));
    delete e;
    }

static void ProbeLevel(TInt aLevel)
    {
    LogF(_L8("--- LEVEL TEST %d ---"), aLevel);
    COcEngine *e = NULL;
    TRAPD(err, e = COcEngine::NewL());
    if (err != KErrNone || !e)
        {
        LogF(_L8("engine alloc failed: %d"), err);
        return;
        }
    TInt verify = -1;
    TUint32 readVal = 0;
    TInt r = e->SelfTestLevel(aLevel, verify, readVal);
    TBuf8<64> step;
    step.Copy(COcEngine::StepName(e->FailedAt()));
    LogF(_L8("  result %d, last step [%S]"), r, &step);
    if (aLevel >= 2)
        {
        LogF(_L8("  read CM_CLKSEL1_PLL = %08x"), readVal);
        LogF(_L8("  restore check -> %d  (nonzero = original kernel code back)"),
             verify);
        }
    delete e;
    Log(_L8("level test done - now leave the phone idle and watch"));
    }

// Apply multiplier and KEEP it. The device continues running at the new
// frequency; a reboot restores everything (nothing is written to flash), or
// a repeated call with nominal stock value 86.
//
// This also works around the transition issue from §20: relock is performed headlessly when
// display is inactive, and the user then operates at the already established frequency -
// only the change window itself is dangerous, not steady-state operation.
static void HoldOverclock(TInt aMult)
    {
    LogF(_L8("--- HOLD: set mult %d and keep it ---"), aMult);
    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    TOcClocks want;
    e->ClocksFor(aMult, want);
    LogClocks(_L8("target"), want);
    LogCompensation(e, aMult);

    TUint32 mhzBefore = BestMhz(5);
    TUint32 aluBefore = BestAlu(3, 3000000);
    TUint32 memBefore = BestMem(3);
    LogF(_L8("  BEFORE: %u MHz, %u.%02u Mitr/s, %u MB/s"),
         mhzBefore, aluBefore / 100, aluBefore % 100, memBefore);

    TUint32 status = 0;
    TInt r = e->ApplyMultRamp(aMult, status);
    LogF(_L8("  ApplyMultRamp -> %d, status=%u"), r, status);

    TInt live = 0;
    e->LiveMult(live);
    LogF(_L8("  live mult now %d"), live);
    {
    // Dividers are checked SEPARATELY from the multiplier. Hardware silently rejects
    // the entire write to CM_CLKSEL1_CORE if any field is invalid; previously
    // logs reported intention rather than the result (findings 38).
    // Intention: in these two paths dividers are chosen automatically.
    const TUint32 wantCore = e->CompensatedCore(aMult);
    TUint32 liveCore = 0;
    if (e->LiveCore(liveCore) == KErrNone)
        LogF(_L8("  live CLKSEL1_CORE %08x (wanted %08x) -> l3=%d dss1=%d  %s"),
             liveCore, wantCore,
             (TInt)(liveCore & KCoreL3Mask),
             (TInt)((liveCore & KCoreDss1Mask) >> KCoreDss1Shift),
             (liveCore == wantCore) ? "DIVIDERS APPLIED" : "*** DIVIDERS REJECTED ***");
    }
    if (r != KErrNone || live != aMult)
        {
        Log(_L8("  NOT held: ramp failed, engine will restore stock"));
        delete e;
        return;
        }

    // Read back the ENTIRE layout, not just the multiplier. The line "CLKSEL1_CORE X -> Y"
    // above is printed BEFORE the write, representing intention rather than fact - exactly
    // the difference the sibling project lacked when investigating whether divider
    // compensation held. Now the fact is visible right here.
    TClockSnap hs;
    SnapClocks(e, hs);
    LogSnap(_L8("held"), hs);
    if ((hs.iCore & (KCoreL3Mask | KCoreDss1Mask)) !=
        (e->CompensatedCore(aMult) & (KCoreL3Mask | KCoreDss1Mask)))
        Log(_L8("  WARNING: compensated dividers are NOT in the register right after the write"));

    TUint32 mhzAfter = BestMhz(5);
    TUint32 aluAfter = BestAlu(3, 3000000);
    TUint32 memAfter = BestMem(3);
    LogF(_L8("  AFTER : %u MHz, %u.%02u Mitr/s, %u MB/s"),
         mhzAfter, aluAfter / 100, aluAfter % 100, memAfter);

    TInt bad = MemoryIntegrity(256 * 1024);
    LogF(_L8("  SDRAM integrity 1 MB: %d bad words"), bad);

    if (mhzBefore && mhzAfter)
        {
        TInt p = (TInt)(((TInt64)mhzAfter - mhzBefore) * 1000 / mhzBefore);
        LogF(_L8("  RESULT NOP: %u -> %u MHz = %d.%d%%"),
             mhzBefore, mhzAfter, p / 10, (p < 0 ? -p : p) % 10);
        }
    if (memBefore && memAfter)
        {
        TInt p = (TInt)(((TInt64)memAfter - memBefore) * 1000 / memBefore);
        LogF(_L8("  RESULT MEM: %u -> %u MB/s = %d.%d%%"),
             memBefore, memAfter, p / 10, (p < 0 ? -p : p) % 10);
        }

    // We keep the frequency, but NOT the kernel patch: it is dangerous for foreign
    // channel calls (findings 20). The destructor will remove the patch and shadow page, but
    // will leave the frequency untouched.
    e->KeepOnExit(ETrue);
    delete e;
    LogF(_L8("  HELD at mult %d. Kernel patch removed; frequency stays."), aMult);
    Log(_L8("  to go back: patcher hold 86, or just reboot"));
    }

// --- full clock layout ------------------------------------------------------
//
// Task - docs/TASK_MPU_DIVIDER.md from the sibling PS1Emulator project.
//
// The question this answers: how much of the frame budget is ARM1136 execution
// itself, and how much is memory waiting. They can only be separated by
// a configuration where ONE domain changes while the other does not. Stock "patcher hold"
// does not provide this: it moves the DPLL multiplier, along with CORE, and from CORE both core,
// bus, and display simultaneously - all by the exact same percentages. Therefore, all four fields
// are specified separately here: DPLL multiplier, MPU divider (CM_CLKSEL_MPU), and L3 and DSS1 dividers
// (both in CM_CLKSEL1_CORE).

_LIT8(KLayoutHeld, "HELD");
_LIT8(KLayoutDrift, "DRIFTED");

// Saw drift - rewrite the layout back. Disabled by default: the
// ReassertDividers blob has never run on hardware, and unverified
// kernel code must not be enabled silently. Switch hold=1.
//
// Returns ETrue if rewritten.
static TBool ReassertIfDrifted(COcEngine *aE, const TClockSnap &aWant,
                               const TClockSnap &aNow, TBool aHold)
    {
    if (!aHold || SameLayout(aWant, aNow))
        return EFalse;
    TInt rc = -1;
    TInt r = aE->ReassertDividers(aWant.iCore, aWant.iMpu, rc);
    LogF(_L8("      reassert %08x/%08x -> r=%d rc=%d (rc=1 = CLKCFG latch never cleared)"),
         aWant.iCore, aWant.iMpu, r, rc);
    return ETrue;
    }

// "key=value" from command line; key is passed including the equals sign.
static TBool KeyVal(const TDesC &aCmd, const TDesC &aKey, TInt &aOut)
    {
    TInt at = aCmd.Find(aKey);
    if (at == KErrNotFound)
        return EFalse;
    TLex lex(aCmd.Mid(at + aKey.Length()));
    TInt v = 0;
    if (lex.Val(v) != KErrNone)
        return EFalse;
    aOut = v;
    return ETrue;
    }

enum TChildKind
    {
    EChildNone = 0,
    EChildPs1,          // E:\sys\bin\ps1stage1.exe - client testbed
    EChildDsp           // E:\sys\bin\dsptest.exe   - DSP project testbed
    };

// Running child with periodic layout snapshot while it executes.
//
// State BEFORE and AFTER alone proves nothing: the core question is whether
// the layout held the ENTIRE time. The DVFS governor rewrites CM_CLKSEL1_CORE at
// runtime - directly observed by the client, - and the transition can only be caught
// from within the window. The time cap remains mandatory: a hanging child would
// otherwise hold us as well, and with us the non-stock layout.
static TInt RunChildWatched(const TDesC &aExe, const TDesC &aArgs, TInt aCapSec,
                            COcEngine *aE, TInt aSampleSec, TInt &aDrift,
                            TBool aHold)
    {
    aDrift = 0;
    RProcess p;
    TInt r = p.Create(aExe, aArgs);
    if (r != KErrNone)
        {
        LogF(_L8("  child: create failed %d"), r);
        return r;
        }
    TRequestStatus done;
    p.Logon(done);
    // A failed Logon completes the request immediately, which would make the very first wait
    // appear as "child exited instantaneously": we would remove the layout while the child
    // kept running. Therefore, abort such a launch rather than watching blindly.
    if (done != KRequestPending)
        {
        LogF(_L8("  child: Logon failed (%d) - not running it unwatched"), done.Int());
        User::WaitForRequest(done);
        p.Kill(KErrGeneral);
        p.Close();
        return KErrGeneral;
        }
    RTimer timer;
    r = timer.CreateLocal();
    if (r != KErrNone)
        {
        p.Kill(KErrNone);
        User::WaitForRequest(done);
        p.Close();
        return r;
        }

    TClockSnap want;
    SnapClocks(aE, want);
    p.Resume();
    LogF(_L8("  child: started, cap %d s, sampling every %d s"), aCapSec, aSampleSec);

    TInt elapsed = 0;
    TInt exitCode = KErrNone;
    TBool finished = EFalse;
    while (elapsed < aCapSec)
        {
        TInt step = aSampleSec;
        if (elapsed + step > aCapSec)
            step = aCapSec - elapsed;
        TRequestStatus tick;
        timer.After(tick, step * 1000000);
        User::WaitForRequest(done, tick);
        if (done != KRequestPending)
            {
            timer.Cancel();
            User::WaitForRequest(tick);
            exitCode = done.Int();
            finished = ETrue;
            LogF(_L8("  child: exited at t~%d s, status=%d"), elapsed, exitCode);
            break;
            }
        elapsed += step;
        TClockSnap now;
        TInt rr = SnapClocks(aE, now);
        const TBool same = SameLayout(want, now);
        if (!same)
            ++aDrift;
        const TDesC8 *verdict = same ? &KLayoutHeld : &KLayoutDrift;
        LogF(_L8("    t=%3d s: PLL=%08x SEL2=%08x MPU=%08x CORE=%08x %S (read r=%d)"),
             elapsed, now.iSel1, now.iSel2, now.iMpu, now.iCore, verdict, rr);
        ReassertIfDrifted(aE, want, now, aHold);
        }
    if (!finished)
        {
        Log(_L8("  child: TIMED OUT, killing"));
        p.Kill(KErrTimedOut);
        User::WaitForRequest(done);
        exitCode = KErrTimedOut;
        }
    timer.Close();
    p.Close();
    return exitCode;
    }

// Soak without child: the same polling, but under our own workload.
// This executes the zero-cost step from §5 of the task - checking whether the
// governor rewrites CM_CLKSEL_MPU the same way it rewrites CM_CLKSEL1_CORE.
// Workload is necessary here: the governor only shifts OPP when there is work, and
// checking in empty idle would reveal nothing.
// Pseudorandom filler and verification. Separated intentionally: time
// must pass between fill and check, otherwise it measures write rather than retention.
static void RetentionFill(TUint32 *aBuf, TInt aWords)
    {
    TUint32 x = 0x2545f491u;
    for (TInt i = 0; i < aWords; ++i)
        {
        x = x * 1664525u + 1013904223u;
        aBuf[i] = x;
        }
    }

static TInt RetentionCheck(const TUint32 *aBuf, TInt aWords)
    {
    TUint32 x = 0x2545f491u;
    TInt bad = 0;
    for (TInt i = 0; i < aWords; ++i)
        {
        x = x * 1664525u + 1013904223u;
        if (aBuf[i] != x)
            ++bad;
        }
    return bad;
    }

static TInt WatchLayout(COcEngine *aE, TInt aSec, TInt aSampleSec, TBool aHold)
    {
    TClockSnap want;
    SnapClocks(aE, want);
    TInt drift = 0;
    const TInt kRetWords = 256 * 1024;          // 1 MB, definitely bypasses cache
    TUint32 *ret = (TUint32 *)User::Alloc(kRetWords * sizeof(TUint32));
    if (ret)
        RetentionFill(ret, kRetWords);
    else
        Log(_L8("  retention buffer alloc failed - SDRAM check skipped"));
    // Measure time step from real clock rather than sum of delays: ALU loop
    // at half the core frequency takes twice as long, and the "t=" label would
    // otherwise lie in the very configuration this is done for.
    const TUint start = User::NTickCount();
    for (TInt t = aSampleSec; t <= aSec; t += aSampleSec)
        {
        // ALU first, then sleep: the first forces the governor to raise OPP, the second -
        // to lower it, so transitions happen in both directions. ~20M iterations is
        // about 1.2 s at stock 330 MHz and 2.4 s at half rate.
        BestAlu(1, 20000000u);
        const TInt leftMs = t * 1000 - (TInt)(User::NTickCount() - start);
        if (leftMs > 0)
            User::After(leftMs * 1000);
        TClockSnap now;
        TInt rr = SnapClocks(aE, now);
        const TBool same = SameLayout(want, now);
        if (!same)
            ++drift;
        const TDesC8 *verdict = same ? &KLayoutHeld : &KLayoutDrift;
        LogF(_L8("    t=%3d s: PLL=%08x SEL2=%08x MPU=%08x CORE=%08x RFR=%08x %S (r=%d)"),
             t, now.iSel1, now.iSel2, now.iMpu, now.iCore, now.iRfr, verdict, rr);
        // Charge retention in SDRAM - direct test of hypothesis §26.1a. The buffer
        // was filled ONCE before the window, so an entire sample interval passes
        // between write and check. The length of this pause is precisely what makes
        // the measurement sensitive: MemoryIntegrity() verifies immediately after write,
        // where only fractions of a millisecond elapse, and stretched refresh would
        // corrupt nothing.
        if (ret)
            {
            TInt bad = RetentionCheck(ret, kRetWords);
            LogF(_L8("      SDRAM retention over %d s: %d bad words in 1 MB"),
                 aSampleSec, bad);
            RetentionFill(ret, kRetWords);
            }
        ReassertIfDrifted(aE, want, now, aHold);
        }
    if (ret)
        User::Free(ret);
    return drift;
    }

// Main command. Step order is not arbitrary.
//
// WHY TWO-STEP APPLICATION. A naive ramp with final dividers at each
// step would kill the device: configuration "mult 43, mpu_div 1" at intermediate
// multiplier 86 would give the core 660 MHz, and the bus 330 - twice the stock value,
// far beyond the 426 MHz limit established by this project. Therefore, a standard
// ramp with AUTOMATIC compensation runs first (the proven path: at each step frequencies
// stay near stock), and only at the final multiplier is ONE explicit write
// of the desired layout performed. At the final multiplier it is correct by construction.
static void RunClocks(TInt aMult, TInt aMpu, TInt aL3, TInt aDss, TInt aSec,
                      TInt aChild, const TDesC &aChildArgs, TInt aSampleSec,
                      TBool aHold)
    {
    LogF(_L8("--- CLOCKS: mult=%d mpu=%d l3=%d dss=%d sec=%d child=%d hold=%d ---"),
         aMult, aMpu, aL3, aDss, aSec, aChild, aHold ? 1 : 0);
    if (aHold)
        Log(_L8("  hold=1: divider reassert is ENABLED and has never run on hardware"));

    // Guards are taken from validation in stock power.dll setters, not our own
    // assumptions: MPU divider 3 does not exist on this SoC, even though the field could hold it.
    if (aMult < KOcMultMin || aMult > KOcMultMax)
        { LogF(_L8("  mult %d out of range %d..%d"), aMult, KOcMultMin, KOcMultMax); return; }
    if (aMult > KOcMultTested)
        LogF(_L8("  NOTE: mult %d is above the tested %d - nothing beyond that is measured"),
             aMult, KOcMultTested);
    if (aMpu < 0 || aMpu > 31 || !(KMpuAllowed & (1u << aMpu)))
        { LogF(_L8("  mpu divider %d not allowed (1, 2, 4, 6, 8)"), aMpu); return; }
    if (aL3 < 1 || aL3 > 31 || !(CoreField(ECoreL3).iAllowed & (1u << aL3)))
        { LogF(_L8("  l3 divider %d not allowed"), aL3); return; }
    if (aDss < 1 || aDss > 31 || !(CoreField(ECoreDss1).iAllowed & (1u << aDss)))
        { LogF(_L8("  dss divider %d not allowed"), aDss); return; }

    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    TUint32 coreReg = CoreFieldSet(e->StockCore(), ECoreL3, aL3);
    coreReg = CoreFieldSet(coreReg, ECoreDss1, aDss);
    const TUint32 mpuReg = (e->StockMpu() & ~KMpuDivMask) | ((TUint32)aMpu & KMpuDivMask);

    // Calculate expected outcome BEFORE writing so the log retains the intended configuration,
    // rather than only what was subsequently read.
    TOcClocks want;
    e->ClocksFor(aMult, want);
    want.iMpuDiv = aMpu;
    want.iL3Div = aL3;
    want.iDss1Div = aDss;
    LogF(_L8("  target: CORE=%08x MPU=%08x -> CORE=%u MPU=%u BUS=%u DSS1=%u kHz"),
         coreReg, mpuReg, want.CoreKhz(), want.MpuKhz(), want.L3Khz(), want.Dss1Khz());

    TClockSnap snap;
    SnapClocks(e, snap);
    LogSnap(_L8("before"), snap);

    // Instrument BEFORE the change. Register read indicates what we wrote; NOP loop and
    // throughput indicate what actually resulted - and project rules
    // require precisely the latter.
    TUint32 mhzBefore = BestMhz(5);
    TUint32 memBefore = BestMem(3);
    LogF(_L8("  BEFORE: %u MHz (NOP), %u MB/s (MEM)"), mhzBefore, memBefore);

    // Step 1: ramp to target multiplier with stock compensation.
    //
    // We execute it HERE rather than calling ApplyMultRamp solely for logging: each
    // step is written to file with flush, so if the device dies mid-ramp, the
    // log will show at which exact multiplier. Without this, the single attempt to drop
    // to 43 left emptiness after the BEFORE line, leaving no way to isolate the failure
    // (§26.10). Semantics are identical: one step at a time, automatic divider selection,
    // 20 ms settling time, abort on non-zero status.
    TUint32 status = 0;
    TInt r = KErrNone;
    TInt cur = 0;
    r = e->LiveMult(cur);
    if (r != KErrNone)
        {
        LogF(_L8("  cannot read live multiplier: %d"), r);
        delete e;
        return;
        }
    LogF(_L8("  ramp %d -> %d, one step at a time"), cur, aMult);
    while (cur != aMult && r == KErrNone)
        {
        cur += (aMult > cur) ? 1 : -1;
        TUint32 st = 0;
        r = e->ApplyMultAndCore(cur, e->CompensatedCore(cur), 0, st);
        status |= st;
        // Print what the engine COMMITTED to hardware, not what we computed:
        // it clamps the l3 field to the live value (findings §38.2), and printing intent
        // would show a bus clock that does not exist.
        const TUint32 stepCore = e->LastCoreWritten();
        TOcClocks sc;
        e->ClocksFor(cur, sc);
        sc.iL3Div = (TInt)(stepCore & KCoreL3Mask);
        sc.iDss1Div = (TInt)((stepCore & KCoreDss1Mask) >> KCoreDss1Shift);
        LogF(_L8("    m=%3d core=%08x BUS=%u DSS1=%u -> r=%d st=%u"),
             cur, stepCore, sc.L3Khz(), sc.Dss1Khz(), r, st);
        if (r != KErrNone || st != 0)
            break;
        User::After(20000);
        }
    LogF(_L8("  ramp done at m=%d -> r=%d status=%u"), cur, r, status);
    if (r != KErrNone || status != 0)
        {
        Log(_L8("  ramp failed; engine restores stock on exit"));
        delete e;
        return;
        }

    // Step 2: final layout - via the EXACT SAME path used by the application.
    //
    // Previously direct ApplyMultAndCore was used here, while the application used
    // ApplyRampWithCore. That discrepancy concealed bug §38.1: the probe
    // always wrote the layout, whereas the application only did so when multiplier changed. Now
    // there is a single path, making this run a genuine verification of GUI behavior.
    status = 0;
    r = e->ApplyRampWithCore(aMult, coreReg, mpuReg, status);
    LogF(_L8("  layout write -> r=%d status=%u (0=ok,1=bypass,2=cfg,4=lock,8=rollback failed)"),
         r, status);
    if (r != KErrNone || status != 0)
        {
        Log(_L8("  layout NOT applied; engine restores stock on exit"));
        delete e;
        return;
        }

    SnapClocks(e, snap);
    LogSnap(_L8("applied"), snap);
    // Compare against what the engine ACTUALLY sent to hardware, not what
    // was requested: it clamps the l3 field to the live value because raising the bus
    // is fatal (findings §38.2). Otherwise an intentional clamp would be interpreted as hardware
    // failure, meaning the instrument would misreport its own operation.
    const TUint32 wroteCore = e->LastCoreWritten();
    const TInt wroteL3 = (TInt)(wroteCore & KCoreL3Mask);
    if (wroteL3 != aL3)
        LogF(_L8("  bus clamp: l3 %d -> %d (raising the bus is 4/4 fatal; reboot for stock)"),
             aL3, wroteL3);
    if ((snap.iMpu & KMpuDivMask) != (TUint32)aMpu ||
        (TInt)(snap.iCore & KCoreL3Mask) != wroteL3 ||
        ((snap.iCore & KCoreDss1Mask) >> KCoreDss1Shift) != (TUint32)aDss)
        Log(_L8("  MISMATCH: hardware did not take the layout"));
    else
        Log(_L8("  layout confirmed by read-back"));

    TUint32 mhzAfter = BestMhz(5);
    TUint32 memAfter = BestMem(3);
    LogF(_L8("  AFTER : %u MHz (NOP), %u MB/s (MEM)"), mhzAfter, memAfter);
    if (mhzBefore && memBefore)
        {
        TInt pc = (TInt)(((TInt64)mhzAfter - mhzBefore) * 1000 / mhzBefore);
        TInt pm = (TInt)(((TInt64)memAfter - memBefore) * 1000 / memBefore);
        LogF(_L8("  RESULT: CPU %d.%d%%  MEM %d.%d%%  <- these two must differ, that is the point"),
             pc / 10, (pc < 0 ? -pc : pc) % 10, pm / 10, (pm < 0 ? -pm : pm) % 10);
        }

    const TInt sec = (aSec > 0) ? aSec : 30;
    const TInt sample = (aSampleSec > 0) ? aSampleSec : 10;
    TInt drift = 0;
    if (aChild == EChildNone)
        {
        LogF(_L8("  watching layout for %d s under own load, sample %d s"), sec, sample);
        drift = WatchLayout(e, sec, sample, aHold);
        }
    else
        {
        _LIT(KPs1, "E:\\sys\\bin\\ps1stage1.exe");
        _LIT(KDsp, "E:\\sys\\bin\\dsptest.exe");
        _LIT(KDspArgs, "baseonlydcdopnobench");
        const TDesC &exe = (aChild == EChildPs1) ? KPs1() : KDsp();
        // Arguments are taken from command line (after "args:"). Only the DSP
        // testbed has a fixed default set; we do not invent defaults for the client
        // testbed - let it receive an empty string and decide on its own.
        const TDesC &args = (aChildArgs.Length() > 0) ? aChildArgs
                          : ((aChild == EChildDsp) ? KDspArgs() : KNullDesC());
        TBuf8<160> a8;
        a8.Copy(args.Left(a8.MaxLength()));
        LogF(_L8("  child args: [%S]"), &a8);
        RunChildWatched(exe, args, sec, e, sample, drift, aHold);
        }

    // Read AFTER child - acceptance criterion from §8 of task. It also answers
    // question §5: does the layout hold without a resident patch (between our
    // snapshots no patch lives in the kernel - EndPatchSession removes it each time).
    SnapClocks(e, snap);
    LogSnap(_L8("after"), snap);
    LogF(_L8("  drift samples: %d (0 = layout held the whole window)"), drift);

    // Restoration is outlined step-by-step: each step logs its intention BEFORE
    // execution. The reason is device death at this exact point (§26.14): the log always
    // truncated after `drift samples`, concealing what happened.
    Log(_L8("  restore: step 1, read live layout"));
    SnapClocks(e, snap);
    LogSnap(_L8("restore-before"), snap);

    // Restoration is performed by the engine ITSELF. Previously probe bypassed it and called
    // ReassertDividers(KStockClkSel1Core, ...) directly - which, first, used RM-320 constants
    // on any device, and second, was the exact write that killed E90.
    // Now both paths, probe and GUI, pass through the same guard.
    const TInt liveMult = (TInt)((snap.iSel1 >> 12) & 0x3ff);
    LogF(_L8("  restore: step 2, RestoreStock (live mult %d, stock %d)"),
         liveMult, e->StockMult());
    status = 0;
    r = e->RestoreStock(status);
    LogF(_L8("  restore: done -> r=%d status=%u"), r, status);
    if (e->RestoreNeedsReboot())
        Log(_L8("  restore: BUS LEFT LOWERED ON PURPOSE - raising l3div back is 4/4 fatal (findings 38.2). Reboot to get stock clocking."));

    Log(_L8("  restore: step 3, final read"));
    SnapClocks(e, snap);
    LogSnap(_L8("restored"), snap);

    e->EndPatchSession();
    LogF(_L8("  restore check: stock DoControl(1,0) -> %d  (nonzero = original code back)"),
         e->VerifyRestored());
    delete e;
    Log(_L8("CLOCKS run done"));
    }

// Overclock stage: changes multiplier, measures with three instruments, soaks under
// load, restores stock value.
static void RunOverclock(TInt aMult)
    {
    LogF(_L8("--- OVERCLOCK: target mult %d ---"), aMult);
    COcEngine *e = PrepareEngine();
    if (!e)
        return;

    TOcClocks want;
    e->ClocksFor(aMult, want);
    LogClocks(_L8("target"), want);
    LogCompensation(e, aMult);

    const TUint32 kAluIter = 3000000;
    TUint32 mhzBefore = BestMhz(5);
    TUint32 aluBefore = BestAlu(3, kAluIter);
    TUint32 memBefore = BestMem(3);
    LogF(_L8("  BEFORE: %u MHz (NOP), %u.%02u Mitr/s (ALU), %u MB/s (MEM)"),
         mhzBefore, aluBefore / 100, aluBefore % 100, memBefore);

    TUint32 status = 0;
    TInt r = e->ApplyMultRamp(aMult, status);
    LogF(_L8("  ApplyMultRamp -> %d, status=%u (0=ok,1=bypass,2=cfg,4=lock,8=rollback failed)"),
         r, status);
    if (r != KErrNone)
        {
        delete e;
        return;
        }
    TInt live = 0;
    e->LiveMult(live);
    LogF(_L8("  live mult now %d"), live);
    {
    // Dividers are checked SEPARATELY from the multiplier. Hardware silently rejects
    // the entire write to CM_CLKSEL1_CORE if any field is invalid; previously
    // logs reported intention rather than the result (findings 38).
    // Intention: in these two paths dividers are chosen automatically.
    const TUint32 wantCore = e->CompensatedCore(aMult);
    TUint32 liveCore = 0;
    if (e->LiveCore(liveCore) == KErrNone)
        LogF(_L8("  live CLKSEL1_CORE %08x (wanted %08x) -> l3=%d dss1=%d  %s"),
             liveCore, wantCore,
             (TInt)(liveCore & KCoreL3Mask),
             (TInt)((liveCore & KCoreDss1Mask) >> KCoreDss1Shift),
             (liveCore == wantCore) ? "DIVIDERS APPLIED" : "*** DIVIDERS REJECTED ***");
    }

    TUint32 mhzAfter = BestMhz(5);
    TUint32 aluAfter = BestAlu(3, kAluIter);
    TUint32 memAfter = BestMem(3);
    LogF(_L8("  AFTER : %u MHz (NOP), %u.%02u Mitr/s (ALU), %u MB/s (MEM)"),
         mhzAfter, aluAfter / 100, aluAfter % 100, memAfter);

    Log(_L8("  soak under OC:"));
    TInt badTotal = 0;
    for (TInt s = 0; s < 5; ++s)
        {
        TUint32 a = BestAlu(1, kAluIter);
        TInt b = MemoryIntegrity(256 * 1024);
        if (b > 0)
            badTotal += b;
        LogF(_L8("    round %d: %u.%02u Mitr/s, %d bad words"),
             s + 1, a / 100, a % 100, b);
        }
    LogF(_L8("  soak total bad words: %d"), badTotal);

    TUint32 st2 = 0;
    e->RestoreStock(st2);
    e->LiveMult(live);
    LogF(_L8("  restored: live mult %d, status=%u"), live, st2);
    if (e->RestoreNeedsReboot())
        Log(_L8("  restore: BUS LEFT LOWERED ON PURPOSE - raising l3div back is 4/4 fatal (findings 38.2). Reboot to get stock clocking."));

    TUint32 mhzRestored = BestMhz(5);
    TUint32 aluRestored = BestAlu(3, kAluIter);
    LogF(_L8("  RESTORED: %u MHz (NOP), %u.%02u Mitr/s (ALU)"),
         mhzRestored, aluRestored / 100, aluRestored % 100);

    // Three independent instruments must yield the same percentage. A discrepancy between
    // them is reason to trust none (CLAUDE.md: measure actual work, not the label).
    if (mhzBefore && mhzAfter)
        {
        TInt p = (TInt)(((TInt64)mhzAfter - mhzBefore) * 1000 / mhzBefore);
        LogF(_L8("  RESULT NOP: %u -> %u MHz = %d.%d%%"),
             mhzBefore, mhzAfter, p / 10, (p < 0 ? -p : p) % 10);
        }
    if (aluBefore && aluAfter)
        {
        TInt p = (TInt)(((TInt64)aluAfter - aluBefore) * 1000 / aluBefore);
        LogF(_L8("  RESULT ALU: %u.%02u -> %u.%02u Mitr/s = %d.%d%%"),
             aluBefore / 100, aluBefore % 100, aluAfter / 100, aluAfter % 100,
             p / 10, (p < 0 ? -p : p) % 10);
        }
    if (memBefore && memAfter)
        {
        TInt p = (TInt)(((TInt64)memAfter - memBefore) * 1000 / memBefore);
        LogF(_L8("  RESULT MEM: %u -> %u MB/s = %d.%d%%"),
             memBefore, memAfter, p / 10, (p < 0 ? -p : p) % 10);
        }
    delete e;
    }

// Terminates hanging instances from previous runs: otherwise they hold
// an open channel to the driver. The same thing the daemon does for itself.
static void KillStaleInstances()
    {
    const TProcessId myId = RProcess().Id();
    TFindProcess finder(_L("ocprobe*"));
    TFullName name;
    TInt killed = 0;
    while (finder.Next(name) == KErrNone)
        {
        RProcess p;
        if (p.Open(name) != KErrNone)
            continue;
        if (p.Id() != myId && p.ExitType() == EExitPending)
            {
            p.Kill(KErrNone);
            ++killed;
            }
        p.Close();
        }
    if (killed)
        LogF(_L8("killed %d stale ocprobe instance(s)"), killed);
    }

static void ProbeHal()
    {
    Log(_L8("--- HAL ---"));
    struct TEntry { HALData::TAttribute iAttr; const char *iName; };
    const TEntry attrs[] =
        {
            { HALData::ECPUSpeed,             "ECPUSpeed(kHz)"       },
            { HALData::EFastCounterFrequency, "EFastCounterFreq(Hz)" },
            { HALData::ENanoTickPeriod,       "ENanoTickPeriod(us)"  },
            { HALData::ESystemTickPeriod,     "ESystemTickPeriod"    },
            { HALData::ECPU,                  "ECPU"                 },
            { HALData::ECPUABI,               "ECPUABI"              },
            { HALData::EMemoryRAM,            "EMemoryRAM"           },
            { HALData::EMemoryRAMFree,        "EMemoryRAMFree"       },
            // Device identification for GUI auto-profile selection: there is no
            // command line, and taking the wrong profile means patching foreign
            // addresses into the kernel.
            { HALData::EMachineUid,           "EMachineUid"          },
            { HALData::EModel,                "EModel"               },
            { HALData::EManufacturer,         "EManufacturer"        },
            // Candidates for hardware autostart switch: external power and
            // clamshell state. Both exist in the SDK, but whether THIS device reports them
            // is a matter of measurement, not documentation. KErrNotSupported is also
            // an answer here: meaning the switch must be different.
            { HALData::EPowerExternal,        "EPowerExternal"       },
            { HALData::EPowerBatteryStatus,   "EPowerBatteryStatus"  },
            { HALData::ECaseState,            "ECaseState"           }
        };
    const TInt n = sizeof(attrs) / sizeof(attrs[0]);
    for (TInt i = 0; i < n; ++i)
        {
        TInt v = 0;
        TInt r = HAL::Get(attrs[i].iAttr, v);
        TPtrC8 nm((const TUint8 *)attrs[i].iName);
        if (r == KErrNone)
            LogF(_L8("%S = %d"), &nm, v);
        else
            LogF(_L8("%S -> err %d"), &nm, r);
        }
    }

// Enumerates already loaded drivers. This provides the EXACT registered channel name,
// instead of guessing from strings inside the file.
static void ListDrivers()
    {
    TFullName name;

    Log(_L8("--- loaded LDDs ---"));
    TFindLogicalDevice findL(_L("*"));
    TInt count = 0;
    while (findL.Next(name) == KErrNone)
        {
        TBuf8<256> n8; n8.Copy(name.Left(250));
        LogF(_L8("  LDD %S"), &n8);
        ++count;
        }
    LogF(_L8("  total: %d"), count);

    Log(_L8("--- loaded PDDs ---"));
    TFindPhysicalDevice findP(_L("*"));
    count = 0;
    while (findP.Next(name) == KErrNone)
        {
        TBuf8<256> n8; n8.Copy(name.Left(250));
        LogF(_L8("  PDD %S"), &n8);
        ++count;
        }
    LogF(_L8("  total: %d"), count);
    }

static void ProbeDriver()
    {
    Log(_L8("--- DVFS driver ---"));

    TInt r = User::LoadLogicalDevice(KLddFile);
    LogF(_L8("LoadLogicalDevice(dvfs_interface.ldd) -> %d   [0=ok, -11=already loaded]"), r);

    // Do not guess the version blindly: drivers typically accept anything not higher
    // than their own, so start from zero and work upwards.
    const TVersion vers[] =
        { TVersion(0,0,0), TVersion(1,0,0), TVersion(1,1,0), TVersion(2,0,0) };
    const TInt nv = sizeof(vers) / sizeof(vers[0]);
    const TDesC *names[2]; names[0] = &KLddName; names[1] = &KLddNameAlt;

    for (TInt n = 0; n < 2; ++n)
        {
        TBuf8<64> nm8; nm8.Copy(*names[n]);
        for (TInt i = 0; i < nv; ++i)
            {
            RDvfs ch;
            TInt o = ch.Open(*names[n], vers[i]);
            LogF(_L8("DoCreate(%S, v%d.%d.%d) -> %d"), &nm8,
                 (TInt)vers[i].iMajor, (TInt)vers[i].iMinor, (TInt)vers[i].iBuild, o);
            if (o == KErrNone)
                {
                Log(_L8("  CHANNEL OPENED - user-side access to DVFS exists"));
                ch.Close();
                return;
                }
            }
        }
    }

// --- E90: list of loaded drivers (READ patch host candidates) ----------------
//
// There is no dvfs on E90, so the standard RM-320 READ primitive is unavailable. The patch itself
// (cmp r1,#0; ldrne r0,[r1]; moveq r0,#0; bx lr) is driver-agnostic and can
// be placed in the opcode body of any user-openable driver
// whose body resides in ROM.
//
// IMPORTANT (lesson of 2026-09-02): this scan only ENUMERATES drivers via
// TFindLogicalDevice - no DoCreate. A previous version blindly opened
// a channel to every loaded LDD; one crashed the kernel and the device
// rebooted. Kernel channels must not be opened blindly - some drivers
// perform initialization on DoCreate that crashes without valid parameters.
// Choosing a specific host and analyzing its DoControl is done OFFLINE using
// the resident XIP; the next probe version opens exactly ONE selected
// driver, doing so safely, logging the name BEFORE the attempt.
static void E90ScanReaders()
    {
    Log(_L8("=== E90: loaded drivers (read-patch host candidates) ==="));
    Log(_L8("(list only - NO channel is opened here; opening is done later, one vetted driver)"));

    TFullName name;
    TFindLogicalDevice findL(_L("*"));
    TInt total = 0;
    while (findL.Next(name) == KErrNone)
        {
        TBuf8<256> n8; n8.Copy(name.Left(250));
        LogF(_L8("  LDD %S"), &n8);
        ++total;
        }
    LogF(_L8("--- %d LDD total ---"), total);

    TFindPhysicalDevice findP(_L("*"));
    TInt tp = 0;
    while (findP.Next(name) == KErrNone)
        {
        TBuf8<256> n8; n8.Copy(name.Left(250));
        LogF(_L8("  PDD %S"), &n8);
        ++tp;
        }
    LogF(_L8("=== %d LDD, %d PDD ==="), total, tp);
    }

// --- Is XIP ROM readable from user mode, and is it what we expect?
//
// The base 0x80000000 for out/roms/e90.rom was assumed by analogy with RM-320 rather than
// measured. If incorrect, all E90 profile addresses are shifted, and the patch lands on
// a foreign page - silently, without crashing, exactly as observed.
//
// The probe is safe for the device: if ROM is inaccessible to user mode,
// OUR process dies (KERN-EXEC 3), not the system. The log is flushed after each
// line, so the address of the last attempt remains visible.
static void RomRead(TBool aIsE90)
    {
    Log(_L8("=== romread: is XIP readable from user mode, and does the base match ==="));

    struct { TUint32 iAddr; TUint32 iVal; const char* iName; } known[3];
    known[0].iAddr = 0x80000000u; known[0].iVal = 0xea00007eu; known[0].iName = "reset";
    known[1].iAddr = aIsE90 ? 0x800b76dcu : 0x800b16ecu;
    known[1].iVal  = 0xc6008540u; known[1].iName = "mult-pool";
    known[2].iAddr = aIsE90 ? 0x80056874u : 0x800a7f6cu;
    known[2].iVal  = 0xe92d40feu; known[2].iName = "sram-routine";

    TInt ok = 0;
    for (TInt i = 0; i < 3; ++i)
        {
        TBuf8<24> nm; nm.Copy(TPtrC8((const TUint8*)known[i].iName));
        LogF(_L8("  reading [0x%08x] (%S) ..."), known[i].iAddr, &nm);
        const TUint32 got = *(volatile TUint32*)known[i].iAddr;
        const TBool match = (got == known[i].iVal);
        if (match) ++ok;
        LogF(_L8("    -> 0x%08x  expect 0x%08x  %s"),
             got, known[i].iVal, match ? "MATCH" : "*** MISMATCH ***");
        }
    LogF(_L8("  === %d/3 -> XIP %s from user mode, base %s ==="),
         ok, (ok > 0) ? "READS" : "does not read",
         (ok == 3) ? "CONFIRMED" : "in doubt");
    }

// --- Signature scan of live XIP from user mode ------------------------------
//
// Implication of romread: ROM is readable from user mode. Consequently, firmware-dependent
// addresses can be found ON THE DEVICE rather than inferred from an image - that is, without
// a kernel read primitive, which previously blocked the entire offline branch.
//
// We look for constants that clocking code holds in literal pools. These are the PRCM
// register addresses themselves, which do not depend on firmware (this is OMAP2420), so
// the signature is identical across all devices in this family.
static void RomScan()
    {
    Log(_L8("=== romscan: signature search in the live XIP (user mode) ==="));

    TInt romSize = 0;
    const TInt hr = HAL::Get(HALData::EMemoryROM, romSize);
    LogF(_L8("HAL EMemoryROM -> %d (%d bytes, %d KB)"), hr, romSize, romSize / 1024);
    // HAL reports ROM in kilobytes on some devices; take the more conservative
    // of the two interpretations to avoid overshooting and killing our own process.
    TUint32 span = (romSize <= 0) ? 0x00200000u
                 : ((romSize < 0x10000) ? (TUint32)romSize * 1024u : (TUint32)romSize);
    if (span > 0x04000000u) span = 0x04000000u;
    const TUint32 base = 0x80000000u;
    LogF(_L8("scanning [0x%08x .. 0x%08x)"), base, base + span);

    // Literals by which target functions are recognized. SDRC and physical
    // SRAM bases are most valuable: on RM-320 the former was present only in power.dll (findings 17),
    // the latter is in the constructor copying the routine to SRAM.
    const TInt KSigCount = 6;
    struct { TUint32 iVal; const char* iName; } sig[KSigCount];
    sig[0].iVal = 0xc6008540u; sig[0].iName = "CM_CLKSEL1_PLL (virt)";
    sig[1].iVal = 0x48008540u; sig[1].iName = "CM_CLKSEL1_PLL (phys)";
    sig[2].iVal = 0xc6008140u; sig[2].iName = "CM_CLKSEL_MPU (virt)";
    sig[3].iVal = 0xc6129000u; sig[3].iName = "SDRC base (virt)";
    sig[4].iVal = 0x4029e000u; sig[4].iName = "OMAP internal SRAM (phys)";
    sig[5].iVal = 0xc6014000u; sig[5].iName = "CONTROL base (virt)";

    // Standard DLddUsbcChannel::DoControl prologue - four words in a row. This exact
    // host was chosen for the READ/EXEC primitive (findings 32); the device disproved
    // the address from the offline image, so we search for it here.
    const TUint32 KDoCtlProlog[4] =
        { 0xe92d4ff0u, 0xe24dd044u, 0xe1a04000u, 0xe1a06002u };

    TInt hits = 0;
    const TUint32 KProgressStep = 0x100000u;   // 1 MB
    const TUint32 last = base + span - 16;
    for (TUint32 a = base; a < last; a += 4)
        {
        if (((a - base) % KProgressStep) == 0)
            LogF(_L8("  .. at 0x%08x"), a);
        const TUint32 w = *(volatile TUint32*)a;

        for (TInt i = 0; i < KSigCount; ++i)
            if (w == sig[i].iVal)
                {
                TBuf8<32> nm; nm.Copy(TPtrC8((const TUint8*)sig[i].iName));
                LogF(_L8("  POOL 0x%08x = 0x%08x  %S"), a, w, &nm);
                ++hits;
                }

        if (w == KDoCtlProlog[0]
            && *(volatile TUint32*)(a + 4)  == KDoCtlProlog[1]
            && *(volatile TUint32*)(a + 8)  == KDoCtlProlog[2]
            && *(volatile TUint32*)(a + 12) == KDoCtlProlog[3])
            {
            LogF(_L8("  DOCTL-PROLOG 0x%08x"), a);
            ++hits;
            }

        // HandleMsg dispatcher: ldr rX,[r1,#24] ; cmp rX,#0x80000000
        // (the same criterion used to enumerate them offline).
        if ((w & 0xffff0fffu) == 0xe5910018u)
            {
            const TUint32 rd = (w >> 12) & 0xfu;
            const TUint32 nx = *(volatile TUint32*)(a + 4);
            if (nx == (0xe3500102u | (rd << 16)))
                {
                LogF(_L8("  DISPATCHER 0x%08x (r%d)"), a, (TInt)rd);
                ++hits;
                }
            }

        if (hits >= 200) { Log(_L8("  (200 hits - stopping)")); return; }
        }
    LogF(_L8("=== romscan done: %d hits ==="), hits);
    }

// --- Dumping a live XIP window to file --------------------------------------
//
// Since ROM is readable from user mode, actual bytes are obtained from the device rather
// than an offline image. This eliminates dependency on FPSX layout, which on E90
// proved non-linear.
//
//   ocprobe romdump <hex-address> <hex-length>
//
// Typically - the window around pools found by romscan.
static void RomDump(const TDesC& aCmd)
    {
    Log(_L8("=== romdump: dumping a window of the live XIP ==="));

    TUint32 start = 0x80050000u;
    TUint32 len   = 0x00020000u;   // 128 KB

    // Two hexadecimal tokens after the word romdump, if present.
    TInt p = aCmd.Find(_L("romdump"));
    if (p != KErrNotFound)
        {
        TLex lex(aCmd.Mid(p + 7));
        TUint32 v = 0;
        lex.SkipSpace();
        if (lex.Val(v, EHex) == KErrNone && v != 0)
            {
            start = v;
            lex.SkipSpace();
            if (lex.Val(v, EHex) == KErrNone && v != 0) len = v;
            }
        }
    if (len > 0x00400000u) len = 0x00400000u;   // 4 MB cap: BT link is slow
    LogF(_L8("window [0x%08x .. 0x%08x)  %d KB"), start, start + len, (TInt)(len / 1024));

    RFs fs;
    TInt r = fs.Connect();
    LogF(_L8("RFs::Connect -> %d"), r);
    if (r != KErrNone) return;

    RFile f;
    r = f.Replace(fs, _L("E:\\romdump.bin"), EFileWrite | EFileStream);
    LogF(_L8("RFile::Replace(E:\\romdump.bin) -> %d"), r);
    if (r != KErrNone) { fs.Close(); return; }

    // Write in chunks; ROM is read like regular memory.
    const TInt KChunk = 4096;
    TUint32 done = 0;
    while (done < len && r == KErrNone)
        {
        const TInt n = ((len - done) < (TUint32)KChunk) ? (TInt)(len - done) : KChunk;
        TPtrC8 chunk((const TUint8*)(start + done), n);
        r = f.Write(chunk);
        done += n;
        }
    LogF(_L8("written %d bytes, last write -> %d"), (TInt)done, r);
    f.Close();
    fs.Close();
    Log(_L8("=== romdump done ==="));
    }

// --- Bootstrapping E90 READ primitive via Usbc host (findings §32) ----------
//
// E90 lacks dvfs, so the read/call primitive is established via RomPatcher+ write
// into the DoControl body of standard USB client driver DLddUsbcChannel (LDD name
// "Usbc"): on both RM-320 and E90, the DoControl prologue is byte-for-byte identical, and
// argument a1 (address) arrives in r2. READ stub (r2 variant):
//   cmp r2,#0 ; ldrne r0,[r2] ; moveq r0,#0 ; bx lr
// HandleMsg dispatcher specifically allows opcode 69 (0x45) through to DoControl even
// when channel is not ready (USB unconnected), so we read via DoControl(69,addr).
// Prologue is bypassed (stmdb/sub skipped), sp is untouched, bx lr returns
// r0 into user DoControl result.

_LIT(KUsbcName, "Usbc");

static const TUint32 KUsbcReadStub[4] =
    { 0xe3520000u,   // cmp   r2, #0
      0x15920000u,   // ldrne r0, [r2]
      0x03a00000u,   // moveq r0, #0
      0xe12fff1eu }; // bx    lr

// Standard first 4 words of DLddUsbcChannel::DoControl (identical on both devices),
// for rollback. The remainder of prologue behind them is untouched.
static const TUint32 KUsbcOriginal[4] =
    { 0xe92d4ff0u,   // stmdb sp!, {r4,r5,r6,r7,r8,r9,sl,fp,lr}
      0xe24dd044u,   // sub   sp, sp, #68
      0xe1a04000u,   // mov   r4, r0
      0xe1a06002u }; // mov   r6, r2

// Signature stub: does not dereference anything, only returns a recognizable constant.
// Distinguishes two states that otherwise blend into a single zero: "stub executed, but
// address did not arrive in r2" and "stub did not execute at all, stock DoControl
// responded". The second is expected on E90 if page is already shadowed by Open4all
// patches: our write then lands in existing shadow page, no new one is
// created, and therefore I-cache is not flushed (findings 19).
static const TUint32 KUsbcSigStub[4] =
    { 0xe3a0005au,   // mov   r0, #0x5a
      0xe12fff1eu,   // bx    lr
      0xe1a00000u,   // nop
      0xe1a00000u }; // nop

const TInt KUsbcSigValue = 0x5a;

// Host channel: only required action is DoControl(69, addr) for reading after patch.
class RUsbcHost : public RBusLogicalChannel
    {
public:
    TInt Open(const TVersion& aVer)
        { return DoCreate(KUsbcName, aVer, KNullUnit, NULL, NULL, EOwnerProcess); }
    TInt Read(TUint32 aAddress) { return DoControl(69, (TAny*)aAddress); }
    };

// Three words known from ROM - independent primitive verification (same on both
// devices): reset vector, multiplier pool, first word of SRAM routine.
struct TKnownWord { TUint32 iAddr; TUint32 iVal; const char* iName; };

static void UsbcBoot(TBool aIsE90)
    {
    Log(_L8("=== usbcboot: bootstrap READ primitive via Usbc host (findings 32) ==="));

    // E90: address MEASURED on live device (romscan -> romdump -> objdump),
    // not derived from out/roms/e90.rom - the layout of that image was disproven
    // by the device. RM-320 remains as it was.
    const TUint32 host = aIsE90 ? 0x800bf874u : 0x8010bd04u;
    LogF(_L8("device=%s  host DLddUsbcChannel::DoControl=0x%08x  a1 in r2"),
         aIsE90 ? "E90" : "RM-320", host);

    // Known words: pool is identical in value, addresses differ between devices.
    // Check words. For E90 all three were read from live device
    // (romread/romdump), so a match here proves the primitive itself, not the layout.
    const TUint32 poolMult = aIsE90 ? 0x8005a610u : 0x800b16ecu;
    TKnownWord known[3];
    known[0].iAddr = 0x80000000u; known[0].iVal = 0xea00007eu; known[0].iName = "reset";
    known[1].iAddr = poolMult;    known[1].iVal = 0xc6008540u; known[1].iName = "mult-pool";
    known[2].iAddr = aIsE90 ? 0x800c09a4u : 0x800a7f6cu;
    known[2].iVal  = aIsE90 ? 0xe5915018u : 0xe92d40feu;
    known[2].iName = aIsE90 ? "dispatcher" : "sram-routine";

    // 1) Patcher.
    TInt r = User::LoadLogicalDevice(KPatcherFile);
    LogF(_L8("LoadLogicalDevice(patcher.ldd) -> %d  [0=ok, -11=already]"), r);
    if (r != KErrNone && r != KErrAlreadyExists)
        { Log(_L8("  patcher not loadable - ABORT (nothing patched)")); return; }
    RPatcherChannel patcher;
    r = patcher.Open();
    LogF(_L8("patcher.Open() -> %d"), r);
    if (r != KErrNone) { Log(_L8("  ABORT (nothing patched)")); return; }

    // 1a) Does the patcher write path work on this device AT ALL. We write to
    //     our own process memory (an address outside ROM uses direct str, findings 10)
    //     and read back in user mode. Safe by construction: the target is
    //     our own variable. Distinguishes "patcher writes nothing" from "writes, but
    //     to ROM shadow without I-cache flush".
    volatile TUint32 canary = 0x11111111u;   // volatile: value changed by kernel,
    TUint32 canaryAddr = (TUint32)&canary;  // compiler must not keep it in a register
    TBuf8<4> mark;
    const TUint32 KCanaryValue = 0xdeadbeefu;
    mark.Copy((const TUint8*)&KCanaryValue, 4);
    TInt cw = patcher.WriteBytes(canaryAddr, mark);
    LogF(_L8("RAM canary: WriteBytes(0x%08x, 0xdeadbeef) -> %d, readback 0x%08x  %s"),
         (TUint32)&canary, cw, (TUint32)canary,
         (canary == KCanaryValue) ? "WRITE PATH OK" : "*** PATCHER WROTE NOTHING ***");

    // 2) Usbc host. Log the name BEFORE opening (lesson of 2026-09-02). Open
    //    EXACTLY this single vetted driver.
    Log(_L8("opening host channel \"Usbc\" (DLddUsbcChannel) ..."));
    const TVersion vers[] = { TVersion(0,0,0), TVersion(1,0,0), TVersion(2,0,0) };
    RUsbcHost usb;
    TInt o = KErrNotFound;
    for (TInt i = 0; i < 3 && o != KErrNone; ++i)
        {
        o = usb.Open(vers[i]);
        LogF(_L8("  DoCreate(Usbc, v%d.%d.%d) -> %d"),
             (TInt)vers[i].iMajor, (TInt)vers[i].iMinor, (TInt)vers[i].iBuild, o);
        }
    if (o != KErrNone)
        { Log(_L8("  Usbc channel did NOT open - ABORT (nothing patched)")); patcher.Close(); return; }
    Log(_L8("  Usbc channel OPENED"));

    // 3) Signature stub first: does what we wrote actually execute.
    //    It does not dereference anything, so cannot crash even on false
    //    register assumptions.
    TUint32 addr = host;
    TBuf8<16> stub;
    stub.Copy((const TUint8*)KUsbcSigStub, sizeof(KUsbcSigStub));
    r = patcher.WriteBytes(addr, stub);
    LogF(_L8("WriteBytes(sig-stub, 4 words @0x%08x) -> %d"), host, r);

    TBool sigOk = EFalse;
    if (r == KErrNone)
        {
        const TInt got = usb.Read(0);
        sigOk = (got == KUsbcSigValue);
        LogF(_L8("  SIG DoControl(69,0) -> 0x%02x  expect 0x%02x  %s"),
             got, KUsbcSigValue,
             sigOk ? "STUB RUNS" : "*** STUB NOT EXECUTING (stale I-cache?) ***");
        }

    // 4) READ stub - only if signature proved the written code executes.
    if (r == KErrNone && !sigOk)
        Log(_L8("  skipping known-word reads: patch is not in effect"));

    if (r == KErrNone && sigOk)
        {
        addr = host;
        stub.Copy((const TUint8*)KUsbcReadStub, sizeof(KUsbcReadStub));
        r = patcher.WriteBytes(addr, stub);
        LogF(_L8("WriteBytes(read-stub, 4 words @0x%08x) -> %d"), host, r);
        }

    if (r == KErrNone && sigOk)
        {
        // 5) Read three known words via host.
        TInt ok = 0;
        for (TInt i = 0; i < 3; ++i)
            {
            TUint32 got = (TUint32)usb.Read(known[i].iAddr);
            const TBool match = (got == known[i].iVal);
            if (match) ++ok;
            TBuf8<24> nm; nm.Copy(TPtrC8((const TUint8*)known[i].iName));
            LogF(_L8("  READ [0x%08x] (%S) -> 0x%08x  expect 0x%08x  %s"),
                 known[i].iAddr, &nm, got, known[i].iVal, match ? "MATCH" : "*** MISMATCH ***");
            }
        LogF(_L8("  === %d/3 known words matched -> read primitive %s ==="),
             ok, (ok == 3) ? "WORKS" : "FAILED");

        // Stock clocking - with the same primitive, read-only. Until now multiplier
        // for E90 was a guess in the profile (86); here it is measured.
        if (ok == 3)
            {
            const TUint32 pll  = (TUint32)usb.Read(0xc6008540u);  // CM_CLKSEL1_PLL
            const TUint32 pll2 = (TUint32)usb.Read(0xc6008544u);  // CM_CLKSEL2_PLL
            const TUint32 mpu  = (TUint32)usb.Read(0xc6008140u);  // CM_CLKSEL_MPU
            const TUint32 core = (TUint32)usb.Read(0xc6008240u);  // CM_CLKSEL1_CORE
            const TUint32 en   = (TUint32)usb.Read(0xc6008500u);  // CM_CLKEN_PLL
            LogF(_L8("  CLK CM_CLKSEL1_PLL  = 0x%08x  mult=%d div=%d sysclk_sel=%d"),
                 pll, (TInt)((pll >> 12) & 0x3ff), (TInt)((pll >> 8) & 0xf),
                 (TInt)((pll >> 23) & 0x7));
            LogF(_L8("  CLK CM_CLKSEL2_PLL  = 0x%08x  core_mult=%d"),
                 pll2, (TInt)(pll2 & 0x3));
            LogF(_L8("  CLK CM_CLKSEL_MPU   = 0x%08x  mpudiv=%d"),
                 mpu, (TInt)(mpu & 0x1f));
            LogF(_L8("  CLK CM_CLKSEL1_CORE = 0x%08x"), core);
            LogF(_L8("  CLK CM_CLKEN_PLL    = 0x%08x  (lock=3 in field [1:0])"), en);
            }
        }

    // 6) Rollback ALWAYS: original words + FreeShadowPage (I-cache flush).
    addr = host;
    stub.Copy((const TUint8*)KUsbcOriginal, sizeof(KUsbcOriginal));
    TInt rr = patcher.WriteBytes(addr, stub);
    LogF(_L8("restore WriteBytes(original, 4 words) -> %d"), rr);
    TUint32 faddr = host, flen = sizeof(KUsbcOriginal);
    TInt fs = patcher.FreeShadow(faddr, flen);
    LogF(_L8("FreeShadowPage(0x%08x, %d) -> %d"), host, (TInt)sizeof(KUsbcOriginal), fs);

    usb.Close();
    patcher.Close();
    Log(_L8("=== usbcboot done: host restored, channels closed ==="));
    }

// The most important question after the first run: is 206 MHz idle or the ceiling?
// We keep the core busy while concurrently sampling what HAL reports about frequency.
// If the governor raises frequency under load, it will be visible here.
static void SampleUnderLoad()
    {
    Log(_L8("--- CPU speed under sustained load ---"));
    for (TInt i = 0; i < 10; ++i)
        {
        TUint32 t0 = User::FastCounter();
        SpinAlu(3000000);
        TUint32 t1 = User::FastCounter();
        TInt sp = 0;
        HAL::Get(HALData::ECPUSpeed, sp);
        TUint32 us = ElapsedUs(t0, t1);
        TUint32 m100 = us ? (TUint32)(((TUint64)3000000u * 100u) / us) : 0;
        LogF(_L8("  t=%d ECPUSpeed=%d kHz  %u.%02u Mitr/s"),
             i, sp, m100 / 100, m100 % 100);
        }
    }

// Reads driver caps. RDevice returns a structure that in the vast majority
// of drivers begins with TVersion - from there we learn the real version without
// iterating through versions blindly.
static void ProbeDeviceCaps()
    {
    Log(_L8("--- dvfs_interface caps ---"));
    RDevice dev;
    TInt r = dev.Open(KLddName);
    LogF(_L8("RDevice::Open -> %d"), r);
    if (r != KErrNone)
        return;

    TBuf8<64> caps;
    caps.FillZ(caps.MaxLength());
    dev.GetCaps(caps);
    LogF(_L8("GetCaps -> len %d"), caps.Length());
    TBuf8<160> hex;
    for (TInt i = 0; i < caps.Length() && i < 32; ++i)
        hex.AppendFormat(_L8("%02x "), caps[i]);
    LogF(_L8("caps bytes: %S"), &hex);
    if (caps.Length() >= 6)
        LogF(_L8("as TVersion: %d.%d.%d"),
             (TInt)caps[0] | ((TInt)caps[1] << 8),
             (TInt)caps[2] | ((TInt)caps[3] << 8),
             (TInt)caps[4] | ((TInt)caps[5] << 8));
    dev.Close();
    }

// A single absolute frequency measurement. Returns MHz to allow comparing
// "before" and "after" states within a single run, without carrying over across reboots.
static TUint32 MeasureMhz()
    {
    const TUint32 kIter = 400000;
    TUint32 t0 = User::FastCounter();
    SpinNop(kIter);
    TUint32 t1 = User::FastCounter();
    TUint32 us = ElapsedUs(t0, t1);
    return us ? (TUint32)(((TUint64)kIter * KNopCyclesPerIter) / us) : 0;
    }

static void MeasureAbsoluteClock()
    {
    Log(_L8("--- absolute clock ---"));
    SpinNop(200000);                       // warmup
    for (TInt run = 0; run < 3; ++run)
        LogF(_L8("NOP run %d: %u MHz"), run + 1, MeasureMhz());
    }

// The only state change in the entire probe. Reserve maximum performance,
// measure, release, measure again. Reserve/release are paired, so the counter
// in the driver returns right back to where we started.
static void ExperimentReserve()
    {
    Log(_L8("--- reserve experiment ---"));
    RDvfs ch;
    TInt r = ch.Open(KLddName, TVersion(0,0,0));
    if (r != KErrNone)
        {
        LogF(_L8("open failed: %d - experiment skipped"), r);
        return;
        }

    // According to code, opcodes other than 1 should return 0 - verify this assertion.
    for (TInt op = 0; op < 4; ++op)
        LogF(_L8("DoControl(%d) -> %d"), op, ch.Ctl(op));

    LogF(_L8("before      : %u MHz, driver says %d%%"), MeasureMhz(), ch.PerfPercent());

    TInt rr = ch.SetPerformance(100);          // > 50 -> reserve
    LogF(_L8("reserve(100): status %d"), rr);
    LogF(_L8("reserved    : %u MHz, driver says %d%%"), MeasureMhz(), ch.PerfPercent());
    LogF(_L8("reserved    : %u MHz (2nd)"), MeasureMhz());

    TInt rl = ch.SetPerformance(0);            // <= 50 -> release
    LogF(_L8("release(0)  : status %d"), rl);
    LogF(_L8("released    : %u MHz, driver says %d%%"), MeasureMhz(), ch.PerfPercent());

    ch.Close();
    Log(_L8("channel closed, reservation balanced"));
    }

static void Benchmark()
    {
    Log(_L8("--- measurement ---"));

    // Warmup: the first run catches cache misses and possible frequency elevation
    // by the DVFS governor, so it is discarded from scoring.
    SpinAlu(2000000);

    const TUint32 kIter = 8000000;
    TInt run;
    for (run = 0; run < 3; ++run)
        {
        TUint32 t0 = User::FastCounter();
        TUint32 sink = SpinAlu(kIter);
        TUint32 t1 = User::FastCounter();
        TUint32 us = ElapsedUs(t0, t1);
        // Miterations/s in hundredths: integer arithmetic, without floating point.
        TUint32 m100 = us ? (TUint32)(((TUint64)kIter * 100u) / us) : 0;
        LogF(_L8("ALU run %d: %u us, %u.%02u Mitr/s (sink=%u)"),
             run + 1, us, m100 / 100, m100 % 100, sink);
        }

    const TInt kWords = 512 * 1024;          // 2 MB - guaranteed to bypass cache
    TUint32 *buf = (TUint32 *)User::Alloc(kWords * sizeof(TUint32));
    if (!buf)
        {
        Log(_L8("MEM: allocation failed, skipped"));
        return;
        }
    TInt i;
    for (i = 0; i < kWords; ++i)
        buf[i] = (TUint32)i;

    for (run = 0; run < 2; ++run)
        {
        TUint32 t0 = User::FastCounter();
        TUint32 sink = SpinMem(buf, kWords, 8);
        TUint32 t1 = User::FastCounter();
        TUint32 us = ElapsedUs(t0, t1);
        const TUint64 bytes = (TUint64)(kWords & ~7) * 4u * 8u;
        TUint32 mbs = us ? (TUint32)(bytes / us) : 0;                    // MB/s
        LogF(_L8("MEM run %d: %u us, ~%u MB/s (sink=%u)"), run + 1, us, mbs, sink);
        }
    User::Free(buf);
    }

// Verifying XIP scanner on a device where the correct answer is known independently.
// Patches nothing: only reads ROM from user mode.
static void RunRomFind()
    {
    Log(_L8("--- romfind: signature scan of the live XIP ---"));
    COcEngine* e = NULL;
    TRAPD(err, e = COcEngine::NewL());
    if (err != KErrNone || !e)
        {
        LogF(_L8("engine alloc failed: %d"), err);
        return;
        }
    TUint32 body = 0, sram = 0;
    const TInt r = e->TryDiscoverRom(body, sram);
    LogF(_L8("TryDiscoverRom -> %d"), r);
    LogF(_L8("  usbc DoControl body : 0x%08x"), body);
    LogF(_L8("  relock routine      : 0x%08x"), sram);
    delete e;
    }

// --- ALU bench --------------------------------------------------------------
//
// Why. The ALU test in the application counts LCG iterations and reports Mitr/s, but
// how many cycles one iteration costs is not visible from this number, making it impossible
// to compare against anything. Here each kernel is written in assembly with .rept, so the number
// of instructions per iteration is known EXACTLY and does not depend on compiler whims, while
// frequency is taken from an independent NOP measurement. The output is cycles per operation.
//
// Kernels are chosen to separate two distinct metrics. A dependent chain measures
// instruction LATENCY: the next cannot begin until the previous is ready.
// An independent chain measures THROUGHPUT: how many such instructions the pipeline issues per
// cycle. ARM1136 is single-issue, so for add both numbers must converge to
// one; a discrepancy would mean the benchmark is not measuring what we think.

// 64 dependent additions: expected latency 1 cycle.
static void SpinAddDep(TUint32 aIter)
    {
    __asm__ __volatile__(
        "1:                  \n"
        ".rept 64            \n"
        "   add r3, r3, #1   \n"
        ".endr               \n"
        "   subs %0, %0, #1  \n"
        "   bne  1b          \n"
        : "+r" (aIter) : : "r3", "cc");
    }

// 64 independent additions across four registers: no dependency chain.
static void SpinAddIndep(TUint32 aIter)
    {
    __asm__ __volatile__(
        "1:                  \n"
        ".rept 16            \n"
        "   add r3, r3, #1   \n"
        "   add r4, r4, #1   \n"
        "   add r5, r5, #1   \n"
        "   add r6, r6, #1   \n"
        ".endr               \n"
        "   subs %0, %0, #1  \n"
        "   bne  1b          \n"
        : "+r" (aIter) : : "r3", "r4", "r5", "r6", "cc");
    }

// 64 dependent multiplications. Rd != Rm intentionally: dependency runs through Rs.
static void SpinMulDep(TUint32 aIter)
    {
    __asm__ __volatile__(
        "   mov r4, #3       \n"
        "1:                  \n"
        ".rept 64            \n"
        "   mul r3, r4, r3   \n"
        ".endr               \n"
        "   subs %0, %0, #1  \n"
        "   bne  1b          \n"
        : "+r" (aIter) : : "r3", "r4", "cc");
    }

// LCG core: multiplication and addition in a single chain - exactly what runs
// the ALU test in the application. 32 pairs per iteration, i.e., also 64 operations.
static void SpinLcg(TUint32 aIter)
    {
    __asm__ __volatile__(
        "   mov r4, #1       \n"
        "   mov r5, #7       \n"
        "1:                  \n"
        ".rept 32            \n"
        "   mul r3, r4, r3   \n"
        "   add r3, r3, r5   \n"
        ".endr               \n"
        "   subs %0, %0, #1  \n"
        "   bne  1b          \n"
        : "+r" (aIter) : : "r3", "r4", "r5", "cc");
    }

// Shift with XOR in a single chain: the second half of the LCG iteration in the GUI.
static void SpinShiftDep(TUint32 aIter)
    {
    __asm__ __volatile__(
        "1:                         \n"
        ".rept 64                   \n"
        "   eor r3, r3, r3, lsr #13 \n"
        ".endr                      \n"
        "   subs %0, %0, #1         \n"
        "   bne  1b                 \n"
        : "+r" (aIter) : : "r3", "cc");
    }

// Cycles per operation in hundredths. aOps - number of operations in the kernel per iteration.
static void ReportAlu(const TDesC8& aName, void (*aFn)(TUint32),
                      TUint32 aIter, TUint32 aOps, TUint32 aMhz)
    {
    TUint32 t0 = User::FastCounter();
    aFn(aIter);
    TUint32 t1 = User::FastCounter();
    const TUint32 us = ElapsedUs(t0, t1);
    const TUint64 ops = (TUint64)aIter * aOps;
    // picoseconds per operation, then cycles = ps * MHz / 1e6
    const TUint32 psPer = ops ? (TUint32)(((TUint64)us * 1000000u) / ops) : 0;
    const TUint32 cyc100 = (TUint32)(((TUint64)psPer * aMhz) / 10000u);
    LogF(_L8("%S: %u us, %u.%03u ns/op, %u.%02u cycles/op"),
         &aName, us, psPer / 1000, psPer % 1000, cyc100 / 100, cyc100 % 100);
    }

static void RunAluBench()
    {
    Log(_L8("--- ALU bench ---"));
    SpinNop(200000);                        // warmup: raise OPP
    const TUint32 mhz = MeasureMhz();
    LogF(_L8("clock (NOP chain): %u MHz"), mhz);
    if (!mhz)
        return;
    const TUint32 kIter = 200000;
    ReportAlu(_L8("add dep    "), SpinAddDep,   kIter, 64, mhz);
    ReportAlu(_L8("add indep  "), SpinAddIndep, kIter, 64, mhz);
    ReportAlu(_L8("mul dep    "), SpinMulDep,   kIter, 64, mhz);
    ReportAlu(_L8("eor+lsr dep"), SpinShiftDep, kIter, 64, mhz);
    ReportAlu(_L8("lcg mul+add"), SpinLcg,      kIter, 64, mhz);
    }

// --- memory bench -----------------------------------------------------------
//
// Why a separate tool. Memory bandwidth in GUI was computed assuming that
// a single read fetches 64 bytes. Cache line length is a hardware property and
// cannot be assumed: if the line is 32 bytes, the figure is overestimated by exactly 2x.
// Here it is MEASURED: traversing with a stride smaller than a line splits one fetched
// line among multiple reads, so time per read increases until stride
// reaches line length, and then plateaus.
static void RunMemBench()
    {
    Log(_L8("--- memory bench ---"));
    const TInt kWords = 1024 * 1024;             // 4 MB, guaranteed to bypass cache
    TUint32* buf = (TUint32*)User::Alloc(kWords * sizeof(TUint32));
    if (!buf)
        {
        Log(_L8("alloc failed"));
        return;
        }
    TInt i;
    for (i = 0; i < kWords; ++i)
        buf[i] = (TUint32)i + 1u;

    // 1. Stride sweep: nanoseconds per read.
    static const TInt KStrides[] = { 1, 2, 4, 8, 16, 32, 64 };   // in words
    const TInt nStrides = sizeof(KStrides) / sizeof(KStrides[0]);
    TInt s;
    for (s = 0; s < nStrides; ++s)
        {
        const TInt step = KStrides[s];
        volatile TUint32 sink = 0;
        TUint32 acc = 0;
        TUint32 t0 = User::FastCounter();
        for (i = 0; i < kWords; i += step)
            acc += buf[i];
        TUint32 t1 = User::FastCounter();
        sink = acc;
        const TUint32 us = ElapsedUs(t0, t1);
        const TUint32 reads = (TUint32)(kWords / step);
        // picoseconds per read: with integers we would otherwise lose all precision
        const TUint32 psPer = reads ? (TUint32)(((TUint64)us * 1000000u) / reads) : 0;
        LogF(_L8("stride %4d B: %u us, %u.%03u ns/read (sink=%u)"),
             step * 4, us, psPer / 1000, psPer % 1000, (TUint32)sink);
        }

    // 2. Sequential read: EVERY word, so bytes are counted without assumptions.
    TInt p;
    for (p = 0; p < 2; ++p)
        {
        volatile TUint32 sink = 0;
        TUint32 acc = 0;
        TUint32 t0 = User::FastCounter();
        for (i = 0; i < kWords; i += 8)
            {
            acc += buf[i] + buf[i+1] + buf[i+2] + buf[i+3];
            acc += buf[i+4] + buf[i+5] + buf[i+6] + buf[i+7];
            }
        TUint32 t1 = User::FastCounter();
        sink = acc;
        const TUint32 us = ElapsedUs(t0, t1);
        const TUint32 mbs = us ? (TUint32)(((TUint64)kWords * 4u) / us) : 0;
        LogF(_L8("seq read 4 MB: %u us, %u MB/s (sink=%u)"), us, mbs, (TUint32)sink);
        }

    // 3. Same within L1 (4 KB): core ceiling on reads from cache.
    {
    const TInt cWords = 1024;                    // 4 KB
    const TInt rounds = 2048;
    volatile TUint32 sink = 0;
    TUint32 acc = 0;
    for (i = 0; i < cWords; ++i)                 // warmup: fill lines
        acc += buf[i];
    TUint32 t0 = User::FastCounter();
    TInt r;
    for (r = 0; r < rounds; ++r)
        for (i = 0; i < cWords; i += 8)
            {
            acc += buf[i] + buf[i+1] + buf[i+2] + buf[i+3];
            acc += buf[i+4] + buf[i+5] + buf[i+6] + buf[i+7];
            }
    TUint32 t1 = User::FastCounter();
    sink = acc;
    const TUint32 us = ElapsedUs(t0, t1);
    const TUint32 bytes = (TUint32)cWords * 4u * (TUint32)rounds;
    const TUint32 mbs = us ? (TUint32)((TUint64)bytes / us) : 0;
    LogF(_L8("cache read 4 KB x%d: %u us, %u MB/s (sink=%u)"),
         rounds, us, mbs, (TUint32)sink);
    }

    // 4. Sequential write - second half of the picture.
    {
    TUint32 t0 = User::FastCounter();
    for (i = 0; i < kWords; i += 8)
        {
        buf[i] = (TUint32)i;     buf[i+1] = (TUint32)i;
        buf[i+2] = (TUint32)i;   buf[i+3] = (TUint32)i;
        buf[i+4] = (TUint32)i;   buf[i+5] = (TUint32)i;
        buf[i+6] = (TUint32)i;   buf[i+7] = (TUint32)i;
        }
    TUint32 t1 = User::FastCounter();
    const TUint32 us = ElapsedUs(t0, t1);
    const TUint32 mbs = us ? (TUint32)(((TUint64)kWords * 4u) / us) : 0;
    LogF(_L8("seq write 4 MB: %u us, %u MB/s"), us, mbs);
    }

    User::Free(buf);
    }

// --- font probe -------------------------------------------------------------

struct TProbeCharInfo
    {
    TUint16 iCode;
    TInt iPx;
    TUint32 iSum;
    };

static void DoFontProbeL()
    {
    static const TUint16 KProbeChars[] =
        {
        0x0041, // 'A'
        0x0430, // Cyrillic small letter a
        0x0456, // Cyrillic small letter Ukrainian i
        0x0457, // Cyrillic small letter yi
        0x0454, // Cyrillic small letter Ukrainian ie
        0x0491, // Cyrillic small letter ghe with upturn
        0x02bc, // modifier apostrophe
        0x4e00  // CJK control
        };
    const TInt KNumChars = sizeof(KProbeChars) / sizeof(KProbeChars[0]);
    const TInt KCjkIndex = KNumChars - 1;

    CFbsTypefaceStore* ts = CFbsTypefaceStore::NewL(NULL);
    CleanupStack::PushL(ts);

    CFbsBitmap* bmp = new (ELeave) CFbsBitmap();
    CleanupStack::PushL(bmp);
    User::LeaveIfError(bmp->Create(TSize(24, 24), EGray2));

    CFbsBitmapDevice* dev = CFbsBitmapDevice::NewL(bmp);
    CleanupStack::PushL(dev);

    CFbsBitGc* gc = NULL;
    User::LeaveIfError(dev->CreateContext(gc));
    CleanupStack::PushL(gc);

    const TInt numTypefaces = ts->NumTypefaces();
    LogF(_L8("font: typefaces=%d"), numTypefaces);

    TProbeCharInfo results[KNumChars];
    TUint8 pixBuf[24 * 24];

    for (TInt i = 0; i < numTypefaces; ++i)
        {
        TTypefaceSupport supp;
        ts->TypefaceSupport(supp, i);

        TFontSpec spec(supp.iTypeface.iName, 12);
        CFont* font = NULL;
        TInt fontErr = ts->GetNearestFontToDesignHeightInPixels(font, spec);
        if (fontErr != KErrNone || !font)
            {
            TBuf8<64> name8;
            name8.Copy(supp.iTypeface.iName);
            LogF(_L8("%S: GetNearestFont failed (%d)"), &name8, fontErr);
            continue;
            }

        gc->UseFont(font);

        TInt baseline = font->AscentInPixels() + 4;
        if (baseline < 4 || baseline > 20)
            baseline = 16;

        for (TInt c = 0; c < KNumChars; ++c)
            {
            gc->SetBrushColor(KRgbWhite);
            gc->SetBrushStyle(CGraphicsContext::ESolidBrush);
            gc->Clear();

            gc->SetBrushStyle(CGraphicsContext::ENullBrush);
            gc->SetPenColor(KRgbBlack);
            gc->SetPenStyle(CGraphicsContext::ESolidPen);

            TBuf<1> text;
            text.Append(TChar(KProbeChars[c]));
            gc->DrawText(text, TPoint(2, baseline));

            TInt darkCount = 0;
            TInt pIdx = 0;
            for (TInt y = 0; y < 24; ++y)
                {
                for (TInt x = 0; x < 24; ++x)
                    {
                    TRgb col;
                    bmp->GetPixel(col, TPoint(x, y));
                    TUint8 dark = (col.Value() != KRgbWhite.Value()) ? 1 : 0;
                    if (dark)
                        ++darkCount;
                    pixBuf[pIdx++] = dark;
                    }
                }

            TUint32 sum = 0;
            Mem::Crc32(sum, pixBuf, sizeof(pixBuf));

            results[c].iCode = KProbeChars[c];
            results[c].iPx = darkCount;
            results[c].iSum = sum;
            }

        gc->DiscardFont();
        ts->ReleaseFont(font);

        const TUint32 cjkSum = results[KCjkIndex].iSum;

        TBuf8<1024> line;
        TBuf8<64> name8;
        name8.Copy(supp.iTypeface.iName);
        line.Append(name8);
        line.Append(_L8(":"));

        for (TInt c = 0; c < KNumChars; ++c)
            {
            line.AppendFormat(_L8(" U+%04X px=%d sum=%08x"),
                results[c].iCode, results[c].iPx, results[c].iSum);
            if (results[c].iSum == cjkSum)
                {
                line.Append(_L8(" MISSING"));
                }
            }

        Log(line);
        }

    CleanupStack::PopAndDestroy(gc);
    CleanupStack::PopAndDestroy(dev);
    CleanupStack::PopAndDestroy(bmp);
    CleanupStack::PopAndDestroy(ts);
    }

static void RunFontProbe()
    {
    Log(_L8("--- font probe ---"));
    TInt err = RFbsSession::Connect();
    if (err != KErrNone)
        {
        LogF(_L8("RFbsSession::Connect failed: %d"), err);
        return;
        }

    TRAPD(probeErr, DoFontProbeL());
    RFbsSession::Disconnect();

    if (probeErr != KErrNone)
        {
        LogF(_L8("font probe leave: %d"), probeErr);
        }
    }

static void RunL()
    {
    User::LeaveIfError(gFs.Connect());
    gFs.MkDirAll(KLogFile);
    // Overwrite: each run is a self-contained report.
    User::LeaveIfError(gLog.Replace(gFs, KLogFile, EFileWrite | EFileShareAny));

    Log(_L8("== ocprobe: DVFS probe, read-only =="));
    // DIRECT proof of reboot. Network reachability does not show it:
    // the device manages to boot and respond again between our checks.
    // Tick counter increments from system start and is reset only by reboot,
    // so comparing this number between runs is the only reliable evidence.
    {
    TUint ntick = User::NTickCount();
    TInt np = 1000;
    HAL::Get(HALData::ENanoTickPeriod, np);
    TUint upSec = (TUint)(((TUint64)ntick * (TUint64)np) / 1000000u);
    LogF(_L8("UPTIME: %u s (%u min) - ntick=%u"), upSec, upSec / 60u, ntick);
    }
    KillStaleInstances();

    TInt fc = 0;
    if (HAL::Get(HALData::EFastCounterFrequency, fc) == KErrNone && fc > 0)
        gFcFreq = (TUint32)fc;
    if (gFcFreq)
        LogF(_L8("fast counter: %u Hz"), gFcFreq);
    else
        Log(_L8("fast counter: UNAVAILABLE - all timings will read zero"));

    // Runs with patcher are iterative, so we skip the slow survey part:
    // it adds nothing, and every extra minute is a minute on hardware.
    // We read command line ONCE into a 256-character buffer. The specific
    // reason: client testbed arguments ("gui play nosnd frames6100 dsa
    // light blend imggt.pbp") alone take fifty chars, and in previous
    // TBuf<64> command was silently truncated mid-string. 256 is exactly as much
    // as the deployment daemon accepts (tester_exec.cpp, TBuf<256> args).
    //
    // Everything after marker "args:" belongs to the CHILD, not us, and is cut off
    // immediately. Otherwise a word from foreign arguments would be caught by our own Find and
    // trigger a different branch.
    TBuf<256> full;
    User::CommandLine(full);
    TPtrC cmd(full);
    TPtrC childArgs(KNullDesC);
    TInt sep = full.Find(_L("args:"));
    if (sep != KErrNotFound)
        {
        cmd.Set(full.Left(sep));
        childArgs.Set(full.Mid(sep + 5));
        }
    const TBool patcherRun = (cmd.Find(_L("patcher")) != KErrNotFound);
    // E90 mode: only HAL + scan of openable channels, without slow survey
    // part and without dvfs probe (no dvfs on E90).
    const TBool e90scan = (cmd.Find(_L("e90scan")) != KErrNotFound);
    // Bootstrap READ primitive via Usbc host. "usbcboot e90" - E90 addresses,
    // otherwise RM-320 (where dvfs oracle exists to verify the mechanism itself).
    const TBool usbcboot = (cmd.Find(_L("usbcboot")) != KErrNotFound);
    // Direct reading of XIP from user mode: image base check (see RomRead).
    const TBool romread = (cmd.Find(_L("romread")) != KErrNotFound);
    const TBool romscan = (cmd.Find(_L("romscan")) != KErrNotFound);
    const TBool romdump = (cmd.Find(_L("romdump")) != KErrNotFound);
    const TBool fontRun = (cmd.Find(_L("font")) != KErrNotFound);
    const TBool membenchRun = (cmd.Find(_L("membench")) != KErrNotFound);
    const TBool alubenchRun = (cmd.Find(_L("alubench")) != KErrNotFound);
    const TBool romfindRun = (cmd.Find(_L("romfind")) != KErrNotFound);
    const TBool quiet = patcherRun || e90scan || usbcboot || romread || romscan || romdump || fontRun || membenchRun || alubenchRun || romfindRun;
    // E90 profile for patcher commands: "patcher ... e90" selects KProfileE90. Then
    // Prepare() (EReadDvfs path) will deliberately fail (EOcStepUnsupported) instead of
    // patching RM-320 addresses into foreign E90 kernel (findings §27). E90 overclock path
    // (via Usbc) is separate; here only safe refusal.
    if (patcherRun && cmd.Find(_L("e90")) != KErrNotFound)
        gUseE90Profile = ETrue;

    ProbeHal();
    if (e90scan)
        E90ScanReaders();
    if (romread)
        RomRead(cmd.Find(_L("e90")) != KErrNotFound);
    if (romscan)
        RomScan();
    if (romdump)
        RomDump(cmd);
    if (usbcboot)
        UsbcBoot(cmd.Find(_L("e90")) != KErrNotFound);
    if (!quiet)
        {
        ListDrivers();
        ProbeDriver();
        }

    // TestPowerAPI only on explicit request: "power <index>".
    TInt at = cmd.Find(_L("power"));
    if (at != KErrNotFound)
        {
        TLex lex(cmd.Mid(at + 5));
        lex.SkipSpace();
        TInt idx = -1;
        if (lex.Val(idx) == KErrNone && idx >= 0 && idx <= 12)
            ProbePowerApi(idx);
        else
            Log(_L8("--- TestPowerAPI: bad index, expected \"power <0..12>\" ---"));
        }
    else
        Log(_L8("--- TestPowerAPI skipped (pass \"power <0..12>\" to enable) ---"));

    if (!quiet)
        {
        ProbeDeviceCaps();
        ExperimentReserve();
        SampleUnderLoad();
        }
    // Patcher runs measure on their own, in a "before/after" pair within a single invocation.
    if (!quiet)
        {
        MeasureAbsoluteClock();
        Benchmark();
        }

    // RomPatcher+ is never loaded during a regular run. Commands are separate:
    //   "patcher selftest" - buffer of this process only;
    //   "patcher read"     - self-test, then READ patch without PRCM writes;
    //   "patcher sram"     - self-test, READ patch, chain to SRAM copy, SDRC;
    //   "patcher engine"       - headless engine check, read-only;
    //   "patcher blobtest"     - execution of test blob in kernel mode;
    //   "patcher dsp [n]"      - DSP domain registers, read-only (n snapshots);
    //   "patcher dspset <d>"   - change DSP divider, restore stock on exit;
    //   "patcher dsphold <d>"  - same, but keep it (reboot restores anyway);
    //   "patcher oc <mult>"    - overclock, measurement, soak, restore stock;
    //   "patcher clocks mult=86 mpu=4 l3=4 dss=6 sec=300 [sample=10]
    //                   [child=ps1|dsp] [args:<child arguments>]"
    //                          - full clock layout, supervised soak,
    //                            restore stock state. Child arguments
    //                            come LAST, after the "args:" marker.
    TInt lv = cmd.Find(_L("patcher level"));
    if (lv != KErrNotFound)
        {
        TLex lex(cmd.Mid(lv + 13));
        lex.SkipSpace();
        TInt n = -1;
        if (lex.Val(n) == KErrNone && n >= 0 && n <= 3)
            ProbeLevel(n);
        else
            Log(_L8("--- expected: patcher level <0..3> ---"));
        }
    else if (cmd.Find(_L("pmutest")) != KErrNotFound)
        ProbePmuTest();
    else if (cmd.Find(_L("blobtest")) != KErrNotFound)
        ProbeBlobTest();
    else if (cmd.Find(_L("patcher discover")) != KErrNotFound)
        ProbeDiscover();
    else if (cmd.Find(_L("patcher engine")) != KErrNotFound)
        ProbeEngine();
    else if (cmd.Find(_L("patcher clocks")) != KErrNotFound)
        {
        // Default is exactly stock layout: command without any switches
        // changes nothing, only measures and supervises.
        TInt mult = KStockMult, mpu = 2, l3 = 4, dss = 6, sec = 30, sample = 10;
        KeyVal(cmd, _L("mult="), mult);
        KeyVal(cmd, _L("mpu="), mpu);
        KeyVal(cmd, _L("l3="), l3);
        KeyVal(cmd, _L("dss="), dss);
        KeyVal(cmd, _L("sec="), sec);
        KeyVal(cmd, _L("sample="), sample);
        // Soak cap is the same as in dspset: a hanging child must not hold
        // a non-stock layout longer than deliberately permitted.
        if (sec < 0 || sec > 900)
            sec = 30;
        if (sample < 5 || sample > 60)
            sample = 10;
        TInt child = EChildNone;
        if (cmd.Find(_L("child=ps1")) != KErrNotFound)
            child = EChildPs1;
        else if (cmd.Find(_L("child=dsp")) != KErrNotFound)
            child = EChildDsp;
        // hold=1 enables rewriting dividers upon drift. A separate switch and
        // disabled by default: this blob has never executed on hardware yet.
        TInt hold = 0;
        KeyVal(cmd, _L("hold="), hold);
        RunClocks(mult, mpu, l3, dss, sec, child, childArgs, sample, hold != 0);
        }
    else if (cmd.Find(_L("patcher dspset")) != KErrNotFound ||
             cmd.Find(_L("patcher dsphold")) != KErrNotFound)
        {
        const TBool keep = (cmd.Find(_L("patcher dsphold")) != KErrNotFound);
        TInt at = cmd.Find(keep ? _L("patcher dsphold") : _L("patcher dspset"));
        TLex lex(cmd.Mid(at + (keep ? 15 : 14)));
        lex.SkipSpace();
        TInt n = -1;
        if (lex.Val(n) == KErrNone)
            {
            // Second optional argument - soak seconds.
            lex.SkipSpace();
            TInt soak = 0;
            if (lex.Val(soak) != KErrNone || soak < 0 || soak > 900)
                soak = 3;
            TInt mode = ESoakPoll;
            if (cmd.Find(_L("quiet")) != KErrNotFound)
                mode = ESoakQuiet;
            else if (cmd.Find(_L("child")) != KErrNotFound)
                mode = ESoakRun;
            ProbeDspSet(n, keep, soak, mode);
            }
        else
            Log(_L8("--- expected: dspset|dsphold <div> [sec] [quiet|child] ---"));
        }
    else if (cmd.Find(_L("patcher dsp")) != KErrNotFound)
        {
        TInt at = cmd.Find(_L("patcher dsp"));
        TLex lex(cmd.Mid(at + 11));
        lex.SkipSpace();
        TInt n = 0;
        // Without argument - 12 snapshots over 3 seconds: enough to see
        // bimodality, and short enough not to stall the device.
        if (lex.Val(n) != KErrNone || n < 1 || n > 60)
            n = 12;
        ProbeDsp(n);
        }
    else if (cmd.Find(_L("romfind")) != KErrNotFound)
        RunRomFind();
    else if (cmd.Find(_L("alubench")) != KErrNotFound)
        RunAluBench();
    else if (cmd.Find(_L("membench")) != KErrNotFound)
        RunMemBench();
    else if (cmd.Find(_L("font")) != KErrNotFound)
        RunFontProbe();
    else if (cmd.Find(_L("patcher hold")) != KErrNotFound)
        {
        TInt at = cmd.Find(_L("patcher hold"));
        TLex lex(cmd.Mid(at + 12));
        lex.SkipSpace();
        TInt mult = -1;
        if (lex.Val(mult) == KErrNone)
            HoldOverclock(mult);
        else
            Log(_L8("--- expected: patcher hold <multiplier> ---"));
        }
    else
        {
        TInt oc = cmd.Find(_L("patcher oc"));
        if (oc != KErrNotFound)
            {
            TLex lex(cmd.Mid(oc + 10));
            lex.SkipSpace();
            TInt mult = -1;
            if (lex.Val(mult) == KErrNone)
                RunOverclock(mult);
            else
                Log(_L8("--- expected: patcher oc <multiplier> ---"));
            }
        }

    Log(_L8("== end =="));
    gLog.Close();
    gFs.Close();
    }

TInt E32Main()
    {
    CTrapCleanup *tc = CTrapCleanup::New();
    if (!tc)
        return KErrNoMemory;
    TRAPD(err, RunL());
    delete tc;
    return err;
    }
