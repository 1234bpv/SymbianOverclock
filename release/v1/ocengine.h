// Overclock engine: shared between the console probe (ocprobe) and the
// on-device application (ocgui). Keeping it in a single place is critical - this
// holds the DPLL relock sequence, and two diverged copies would mean
// freezing the device.
//
// Full breakdown in docs/findings.md, sections 17 and 18. Briefly, three primitives:
//   1. READ  - patch the dvfs_interface DoControl(1) body @0x80148410 into "ldr r0,[r1]";
//   2. EXEC  - the same patch into "bx r1": calling an arbitrary kernel address;
//   3. SRAM  - staging ground outside SDRAM: the chunk that power.dll maps at boot.
//
// Everything is runtime-only. A reboot restores the device to its original state; nothing
// is written to flash.

#ifndef OCENGINE_H
#define OCENGINE_H

#include <e32std.h>
#include <e32base.h>

// DoCreate/DoControl in RBusLogicalChannel are protected, so we wrap the channels.
// The patcher channel name comes from the driver's own disassembly, not the filename.
class RPatcherChannel : public RBusLogicalChannel
    {
public:
    TInt Open();
    // func 105: a1 -> target address, a2 -> descriptor with bytes.
    TInt WriteBytes(TUint32& aAddress, const TDesC8& aBytes);
    // func 100: a1 -> address, a2 -> length. Frees ROM-shadow pages in the
    // range (0x848c -> ekern ordinal 238 FreeShadowPage page by page),
    // i.e., restores the region's stock mapping directly to ROM.
    TInt FreeShadow(TUint32& aAddress, TUint32& aLength);
    };

class RDvfsChannel : public RBusLogicalChannel
    {
public:
    TInt Open();
    // After the READ patch - read at the address; after the EXEC patch - call at it.
    TInt ReadOrCall(TUint32 aAddress);
    };

// Slider limits. 129 is exactly +50% from stock 86; 43 is symmetrically down.
// Stock value is read from the device, not assumed.
const TInt KOcMultMin = 43;
const TInt KOcMultMax = 129;

// Registers (virtual addresses; physical = minus 0x7E000000 for window 0x48xxxxxx).
const TUint32 KRegClkSel1Pll = 0xc6008540u;
const TUint32 KRegClkSel2Pll = 0xc6008544u;
const TUint32 KRegClkSelMpu  = 0xc6008140u;
const TUint32 KRegClkEnPll   = 0xc6008500u;
const TUint32 KRegIdlestPll  = 0xc6008520u;
const TUint32 KRegClkSel1Core= 0xc6008240u;
const TUint32 KRegSdrcRfr0   = 0xc61290a4u;
const TUint32 KRegSdrcActimA = 0xc612909cu;

// Clock layout decoded from registers. Formula is from power.dll code:
//   DPLL = sys_clk * mult / (div + 1)      @0x800B1544
//   CORE = DPLL * CM_CLKSEL2_PLL[1:0]      @0x800B15B0
//   MPU  = CORE / CM_CLKSEL_MPU[3:0]       @0x800B15DC
//   L3   = CORE / CM_CLKSEL1_CORE[4:0]     (SDRAM is clocked from here as well)
// Fields of CM_CLKSEL1_CORE. Derived from power.dll setters @0x800AC458..0x800AC598:
// each validates the value, shifts, and calls Modify32 with its own mask. The layout
// matches Linux headers for OMAP2420 - two independent sources.
const TUint32 KCoreL3Mask   = 0x0000001fu;   // [4:0]
const TUint32 KCoreL4Mask   = 0x00000060u;   // [6:5]
const TUint32 KCoreDss1Mask = 0x00001f00u;   // [12:8]
const TInt    KCoreDss1Shift = 8;
const TInt    KCoreL3Shift   = 0;

class TOcClocks
    {
public:
    void Decode(TUint32 aClkSel1, TUint32 aClkSel2, TUint32 aMpuDiv, TUint32 aCoreDiv);
    TUint32 DpllKhz() const;
    TUint32 CoreKhz() const;
    TUint32 MpuKhz() const;
    TUint32 L3Khz() const;

    TUint32 iSysKhz;      // 19200, 13000, or 12000 - according to field [25:23]
    TInt    iMult;        // [21:12]
    TInt    iDiv;         // [11:8], divided by (div+1)
    TInt    iCoreSel;     // CM_CLKSEL2_PLL[1:0]
    TInt    iMpuDiv;      // CM_CLKSEL_MPU[3:0]
    TInt    iL3Div;       // CM_CLKSEL1_CORE[4:0]
    TInt    iDss1Div;     // CM_CLKSEL1_CORE[12:8]
    TUint32 Dss1Khz() const;
    };

// Preparation step codes: provide meaningful diagnostics instead of "failed".
enum TOcStep
    {
    EOcStepIdle = 0,
    EOcStepLoadDriver,        // User::LoadLogicalDevice(patcher.ldd)
    EOcStepOpenPatcher,       // DoCreate("RomPatcherPlus.Driver")
    EOcStepSelfTest,          // func 105 into own buffer
    EOcStepReadPatch,         // applying READ patch
    EOcStepOpenDvfs,          // DoCreate("dvfs_interface")
    EOcStepResolveSram,       // chain to linear address of SRAM copy
    EOcStepVerifySram,        // verification of copy against ROM original
    EOcStepReadRegs,
    EOcStepReady
    };

class COcEngine : public CBase
    {
public:
    static COcEngine* NewL();
    ~COcEngine();

    // Performs everything up to readiness. KErrNone = ready to overclock.
    TInt Prepare();
    TBool IsReady() const { return iReady; }
    TOcStep FailedAt() const { return iStep; }
    static const TDesC& StepName(TOcStep aStep);

    TUint32 SramAddress() const { return iSram; }
    TInt StockMult() const { return iStockClocks.iMult; }
    const TOcClocks& StockClocks() const { return iStockClocks; }
    // Schedule for a hypothetical multiplier - to show "what will happen" before applying.
    void ClocksFor(TInt aMult, TOcClocks& aOut) const;
    // Selects L3 and DSS1 dividers so their frequencies remain as close as possible
    // to stock under the new CORE. Returns the prepared CM_CLKSEL1_CORE value.
    TUint32 CompensatedCore(TInt aMult) const;
    TUint32 StockCore() const { return iStockCore; }

    TInt ReadReg(TUint32 aAddr, TUint32& aOut);
    // Rereads CM_CLKSEL1_PLL from the device and returns the actual multiplier.
    TInt LiveMult(TInt& aOut);

    // Executes relock. aStatus: 0 = clean, bit0 = bypass timeout (multiplier was NOT
    // written), bit1 = CLKCFG timeout, bit2 = lock timeout.
    TInt ApplyMult(TInt aMult, TUint32& aStatus);
    // The same result, but STEPWISE by one multiplier step at a time, with a pause
    // to settle between them. Reason: a +1 step is stable even with an active
    // display, whereas +2 and more hangs the device (findings 20) - bypass window is longer,
    // and the display controller does not survive the L3 drop. One large jump is in no way
    // better than several small ones, so we only take small steps.
    TInt ApplyMultRamp(TInt aTarget, TUint32& aStatus);
    TInt RestoreStock(TUint32& aStatus);
    // Restores the replaced DoControl(1) body to its original form. Called from the
    // destructor, but an explicit call before exiting is more reliable.
    TInt RestoreKernelCode();
    // Returns percentage of nominal from the STOCK function. A non-zero value
    // means the rollback succeeded and the original code is in the kernel again.
    TInt VerifyRestored();
    // Frees the shadow page created by the patcher for the DoControl(1) body.
    // Without this, each run permanently leaves an allocated RAM page in the kernel
    // over the ROM code page.
    TInt UnshadowKernelPage();
    // Complete patch removal: bytes restored + unshadow. Unshadow is precisely what makes
    // this reliable, because rewriting bytes does not invalidate the I-cache.
    TInt EndPatchSession();

    // Bisection of the reboot cause. Each level leaves behind exactly one
    // more thing than the previous:
    //   0 - loaded patcher.ldd, channel opened and closed;
    //   1 - + func 105 write into OWN buffer (kernel is not modified);
    //   2 - + READ patch, one read, rollback with verification (ROM-shadow page);
    //   3 - full Prepare (everything as in a production run).
    // The first level after which the device crashes at idle points to the culprit.
    TInt SelfTestLevel(TInt aLevel, TInt& aVerify, TUint32& aReadValue);
    TBool IsOverclocked() const { return iApplied; }
    // By default, the destructor restores stock frequency: the process terminates while
    // the frequency remains, and silently leaving the device overclocked would be improper.
    // But when overclocking is applied SPECIFICALLY to keep using the device afterward,
    // this must be disabled explicitly. The kernel patch is still removed regardless - it
    // is dangerous for third-party calls (findings §20), whereas the frequency is not.
    void KeepOnExit(TBool aKeep) { iKeep = aKeep; }
    TInt AppliedMult() const { return iAppliedMult; }

private:
    COcEngine();
    void ConstructL();
    TInt Poke(TUint32 aAddr, const TDesC8& aBytes);
    TInt PokeWords(TUint32 aAddr, const TUint32* aWords, TInt aCount);
    TInt ApplyReadPatch();
    TInt ApplyExecPatch();
    TInt ReopenDvfs();
    TInt WriteRelockBlob(TUint32 aClkSel1Value, TUint32 aCoreValue);

private:
    RPatcherChannel iPatcher;
    RDvfsChannel    iDvfs;
    TBool    iPatcherOpen;
    TBool    iDvfsOpen;
    TBool    iReady;
    TBool    iExecMode;        // which patch is currently applied
    TBool    iPatched;         // whether the DoControl(1) body is currently replaced
    TBool    iShadowed;        // whether a ROM-shadow page was created by the patcher
    TBool    iApplied;
    TBool    iKeep;            // do not restore frequency in destructor
    TInt     iAppliedMult;
    TUint32  iSram;
    TUint32  iStockClkSel1;
    TUint32  iStockCore;       // CM_CLKSEL1_CORE as it was at start
    TOcClocks iStockClocks;
    TOcStep  iStep;
    };

#endif
