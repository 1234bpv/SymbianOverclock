// Overclock engine: shared between the console probe (ocprobe) and the
// on-device application (ocgui). Keeping it in one place is critical - the
// DPLL relock sequence resides here, and two divergent copies would mean
// hanging the device.
//
// Full analysis - docs/findings.md, sections 17 and 18. Briefly, three primitives:
//   1. READ  - patch of the dvfs_interface DoControl(1) body @0x80148410 to "ldr r0,[r1]";
//   2. EXEC  - the same patch to "bx r1": call arbitrary kernel address;
//   3. SRAM  - staging area outside SDRAM: chunk mapped by power.dll at boot.
//
// Everything is runtime-only. A reboot returns the device to its original state;
// nothing is written to flash.

#ifndef OCENGINE_H
#define OCENGINE_H

#include <e32std.h>
#include <e32base.h>

// DoCreate/DoControl in RBusLogicalChannel are protected, so we wrap the channels.
// Patcher channel name is from the driver disassembler itself, not from the file name.
class RPatcherChannel : public RBusLogicalChannel
    {
public:
    TInt Open();
    // func 105: a1 -> target address, a2 -> descriptor with bytes.
    TInt WriteBytes(TUint32& aAddress, const TDesC8& aBytes);
    // func 100: a1 -> address, a2 -> length. Frees ROM-shadow pages across
    // the range (0x848c -> ekern ordinal 238 FreeShadowPage page by page),
    // meaning it restores stock mapping directly to ROM for the region.
    TInt FreeShadow(TUint32& aAddress, TUint32& aLength);
    };

// Host of the READ/EXEC primitive. On RM-320 this is dvfs_interface, on E90 - Usbc
// (there is no dvfs in E90 firmware, findings §27/§32). Only the channel name,
// DoControl opcode, and register passing the address to the body differ; the patch
// mechanics itself is shared, so the channel is parameterized rather than duplicated.
enum TOcHost
    {
    EHostDvfs = 0,   // dvfs_interface, DoControl(1, a1), address in r1
    EHostUsbc        // Usbc (DLddUsbcChannel), DoControl(69, a1), address in r2
    };

class RDvfsChannel : public RBusLogicalChannel
    {
public:
    RDvfsChannel() : iHost(EHostDvfs) {}
    void SetHost(TOcHost aHost) { iHost = aHost; }
    TInt Open();
    // After READ patch - reads at the address; after EXEC patch - calls it.
    TInt ReadOrCall(TUint32 aAddress);
private:
    TOcHost iHost;
    };

// Slider bounds. These are bounds of the SLIDER specifically, not the device: multiplier
// field in CM_CLKSEL1_PLL has ten bits [21:12], so it fits up to 1023, and one step
// equals 3.84 MHz (sys_clk 19.2 MHz, DPLL divider /5).
//
// Until 2026-09-10 the ceiling was 129 - exactly +50% over stock 86, a value convenient
// because 129/86 = 1.5 and all accompanying dividers scale by an integer.
// Raised to 255: former N95 ceiling of 111 turned out to be the bus, not the core
// (findings §38.4), so the true silicon limit is unknown and needs to be measured.
// Above 129 nothing has been verified. The DPLL might fail to lock - then the blob returns
// non-zero status and rolls back on its own; divider compensation becomes approximate;
// we do not compensate the SDRAM autorefresh window at all.
// Stock value is read from the device, not assumed.
const TInt KOcMultMin = 43;
const TInt KOcMultMax = 255;

// There are no measurements above this multiplier - both GUI and probe state this explicitly.
const TInt KOcMultTested = 129;

// The STOCK clock state of THIS device consists of CONSTANTS, not what was read
// at process start. The difference is fundamental.
//
// Previously the reference point was sampled from live registers in Prepare(). As long as the device
// is stock, there is no difference; but once it is already overclocked, the reference point drifts, and
// CompensatedCore() begins targeting a foreign frequency. A neighboring project caught this
// in its logs: the cycle "hold 107 -> hold 86" left dssdiv=5 instead of 6, i.e.
// pixel clock of 132.1 MHz instead of stock 110.08, and the line "vs stock: DSS1
// -3.5%" still reported that display was barely touched. Each subsequent cycle
// shifted the divider by another step and never reverted - a ratchet unnoticed in logs
// precisely because the logs measured drift from an already drifted point.
//
// Values sampled from the device after a cold boot
// (PS1Emulator/plans/runs/oc107_apply.log:15-17).
const TUint32 KStockClkSel1Core = 0x04360644u;   // l3div=4, dss1div=6
const TUint32 KStockClkSelMpu   = 0x00000002u;   // mpu_div=2
const TInt    KStockMult        = 86;            // DPLL 330.24 -> CORE 660.48

// Strategy for reading current frequency - depends on what is present in firmware.
enum TOcReadStrategy
    {
    // Patch of host DoControl body. Which host specifically is specified by iHost: dvfs_interface
    // on RM-320, Usbc on E90. The name remains historical.
    EReadDvfs = 0,
    EReadPowerHelper,  // call of register reading helper from power.dll (where dvfs is absent)
    EReadMeasureOnly   // do not read register at all, measure work (most portable)
    };

// Device/firmware profile: EXACTLY those values that change between ROMs. PRCM
// registers, field masks, and allowed values are NOT included here - this is OMAP2420 itself,
// identical on all these devices. Populated offline from unpacked z:
// (findings §29); numbers are taken only from ROM or from device, never guessed.
struct TOcProfile
    {
    const char*     iId;              // firmware identifier, e.g. "RM-320 31.0.015"
    // HAL EMachineUid of this device. Used to select the profile AUTOMATICALLY -
    // the application has no command line, and picking a foreign profile means
    // patching foreign addresses into a foreign kernel.
    TUint32         iMachineUid;
    TOcReadStrategy iRead;
    TOcHost         iHost;            // primitive host (see TOcHost)
    TUint32         iDvfsBody;        // host DoControl body; 0 if host is absent
    TUint32         iRomSramRoutine;  // power.dll idle/relock routine (for DPLL bypass)
    // Stock DSP divider setter in power.dll. ZERO means "not measured for this
    // device", and ApplyDspDiv on such a profile refuses: zero here is a branch
    // address, not a "skip" flag.
    TUint32         iDspSetter;
    TUint32         iPwrDownGlobal;   // power-down global (SRAM address resolution)
    // Stock clocking layout of SPECIFICALLY this device: panels differ, so DSS/L3
    // vary across models, and a shared reference point cannot be used.
    TUint32         iStockClkSel1Core;
    TUint32         iStockClkSelMpu;
    TInt            iStockMult;
    // Firmware version for which the addresses above were derived, as it appears in string
    // from SysUtil::GetSWVersion (e.g. "31.0.015"). Addresses depend on build,
    // not just model, so a matching machine uid alone guarantees
    // nothing: on another build the same patch will hit the wrong code. An empty
    // string disables the check - leaving room for a profile whose version
    // has not been verified yet, but then this must be visible in documentation, not silent.
    const char*     iFwVersion;
    // Profile is NOT proven on device: addresses are derived from firmware image, but
    // verification was offline only. Such a profile is never selected
    // automatically - it can only be selected manually, deliberately.
    TBool           iExperimental;
    // Hint for searching the power-down global when iPwrDownGlobal == 0.
    // Taken from the literal pool of the getter/setter pair in ROM.
    TUint32         iPwrDownHint;
    // First eight words of the relock routine as they appear in the firmware image from which
    // addresses were derived. After applying READ patch, engine reads them from device and
    // verifies them. This accomplishes two things at once: proves that the patch is ALIVE (otherwise
    // reading would return foreign data), and proves that iRomSramRoutine
    // points specifically to the relock routine of THIS EXACT build. NULL disables verification.
    const TUint32*  iSramSignature;
    // Profile WITHOUT addresses: device discovers them itself by signatures in live XIP.
    //
    // For devices whose extracted firmware image is merely a fragment of full XIP
    // (E90, N93, N93i), offline addresses are invalid in principle: file offset does
    // not equal address (findings 47.1). The only proven path for them is scanning
    // live ROM from user mode. It only reads, and without found
    // signatures the engine refuses and writes nothing to the kernel.
    TBool           iDiscover;
    };

// Eight words of power.dll relock routine prologue. Identical across all parsed
// OMAP2420 images (RM-320, RM-159, N82), so this is a signature of the function itself, not
// of a specific build - and it is used here precisely as a function signature.
extern const TUint32 KSramRoutineSignature[8];

// Half a kilobyte in both directions from the hint. The window is intentionally small: each probe
// is a read at an arbitrary address IN KERNEL MODE, and an unmapped page costs
// a reboot, not an error.
const TUint32 KPwrDownSearchSpan = 0x200;
// Kernel data bounds. Everything the search reads must lie within them.
const TUint32 KKernDataLo = 0xc0000000u;
const TUint32 KKernDataHi = 0xf0000000u;
// XIP ROM bounds: everything we verify with bytes resides here.
const TUint32 KRomLo = 0x80000000u;
const TUint32 KRomHi = 0x90000000u;

// Reference profile proven on device (N95 8GB RM-320, firmware 31.0.015).
extern const TOcProfile KProfileRM320;

// Profile selection by HAL EMachineUid. Returns NULL if the device is unknown, and this is
// the ONLY correct answer in such case: silent fallback to the reference
// profile would mean patching RM-320 addresses into a foreign kernel.
const TOcProfile* ProfileForThisDevice();

// Test profiles: derived offline, not verified on hardware, NOT selected
// automatically. Only the user selects them, and only deliberately.
TInt TestProfileCount();
const TOcProfile* TestProfile(TInt aIndex);

// Nokia E90 profile (RA-6, 400.34.93). Four addresses derived offline from XIP
// (findings §31), dvfs is absent in firmware -> iDvfsBody=0, strategy EReadMeasureOnly.
// NO LONGER THE CASE (2026-09-09): overclocking on E90 works, Usbc (EHostUsbc) serves
// as host for READ/EXEC primitive, and device completes full cycle. Only
// iDspSetter remains zero - DSP divider on E90 was not measured, and ApplyDspDiv refuses because of this.
extern const TOcProfile KProfileE90;

// Registers (virtual addresses; physical = minus 0x7E000000 for 0x48xxxxxx window).
const TUint32 KRegClkSel1Pll = 0xc6008540u;
const TUint32 KRegClkSel2Pll = 0xc6008544u;
const TUint32 KRegClkSelMpu  = 0xc6008140u;
const TUint32 KRegClkEnPll   = 0xc6008500u;
const TUint32 KRegIdlestPll  = 0xc6008520u;
const TUint32 KRegClkSel1Core= 0xc6008240u;
const TUint32 KRegSdrcRfr0   = 0xc61290a4u;
const TUint32 KRegSdrcActimA = 0xc612909cu;

// DSP domain (C55x). The same 4 KB PRCM page as the registers above, so
// READ patch reaches them without any changes in mechanics. Address CM_CLKSEL_DSP is not
// a guess: exactly this address is loaded by power.dll @0x800B16F4 when calculating DSP frequency.
const TUint32 KRegFclkenDsp  = 0xc6008800u;   // CM_FCLKEN_DSP
const TUint32 KRegIclkenDsp  = 0xc6008810u;   // CM_ICLKEN_DSP
const TUint32 KRegIdlestDsp  = 0xc6008820u;   // CM_IDLEST_DSP
const TUint32 KRegClkSelDsp  = 0xc6008840u;   // CM_CLKSEL_DSP

// CM_CLKSEL_DSP fields. Derived from ehwaomap.pdd setters @0x801AE900 and
// @0x801AE960 (each is bic/orr/str into [r0,#0x840]) and matches the clksel
// layout for OMAP2420 in Linux kernel - two independent sources.
//   [3:0]   divider from which the firmware ITSELF calculates DSP frequency: power.dll
//           ord60 @0x800B1644 masks exactly 0xF and divides CORE by it.
//           Datasheet gives the field 5 bits [4:0]; PDD setter does not touch it at all,
//           meaning at runtime the firmware does not change the low field.
//   [6:5]   CLKSEL_DSP_IF - interface clock derived from DSP divider.
//   [7]     field 0 of setter, purpose unknown.
//   [12:8]  CLKSEL_IVA.
//   [13]    field of second setter, purpose unknown.
const TUint32 KDspDivMask   = 0x0000001fu;   // [4:0] per datasheet
const TUint32 KDspFwDivMask = 0x0000000fu;   // [3:0] - this is how much ord60 sees
const TUint32 KDspIfMask    = 0x00000060u;   // [6:5]
const TUint32 KDspBit7Mask  = 0x00000080u;   // [7]
const TUint32 KDspIvaMask   = 0x00001f00u;   // [12:8]
const TUint32 KDspBit13Mask = 0x00002000u;   // [13]

// STOCK DSP divider setter in power.dll. Found as follows: literal 0xC6008840
// occurs in entire ROM exactly twice, both times in power.dll, and one of
// the occurrences - in constant pool next to CM_CLKSEL_MPU and CM_CLKSEL1_CORE, i.e. in
// the zone of validated PRCM setters (@0x800AC410..0x800AC698). Body:
//   cmp r1,#9 / #16 / #18 / #0 / #5 -> mvn r0,#5 (KErrArgument)
//   otherwise Modify32(0xC6008840, mask 0x1F, r1); return 0
const TUint32 KDspSetter = 0x800ac59cu;
// Values that the firmware ITSELF considers valid: all 1..31, except 0, 5, 9, 16, 18.
// This is not our choice - this is the list from setter validation.
// = ~((1<<0)|(1<<5)|(1<<9)|(1<<16)|(1<<18))
const TUint32 KDspDivAllowed = 0xfffafddeu;
TInt DspDivStep(TInt aValue, TInt aDir);

// Clocking layout parsed from registers. Formula - from power.dll code:
//   DPLL = sys_clk * mult / (div + 1)      @0x800B1544
//   CORE = DPLL * CM_CLKSEL2_PLL[1:0]      @0x800B15B0
//   MPU  = CORE / CM_CLKSEL_MPU[3:0]       @0x800B15DC
//   L3   = CORE / CM_CLKSEL1_CORE[4:0]     (SDRAM is also clocked from here)
// CM_CLKSEL1_CORE fields. Derived from power.dll setters @0x800AC458..0x800AC598:
// each validates value, shifts, and calls Modify32 with its own mask. Layout
// matches Linux headers for OMAP2420 - two independent sources.
const TUint32 KCoreL3Mask   = 0x0000001fu;   // [4:0]
const TUint32 KCoreL4Mask   = 0x00000060u;   // [6:5]
const TUint32 KCoreDss1Mask = 0x00001f00u;   // [12:8]
const TInt    KCoreDss1Shift = 8;
const TInt    KCoreL3Shift   = 0;

// All CM_CLKSEL1_CORE fields with ALLOWED values taken from validation in
// power.dll setters @0x800AC458..0x800AC598 (findings §21). Allowed values
// are not our invention, but what the firmware itself considers valid for this SoC,
// so they serve as guardrails for manual mode.
enum TCoreFieldId
    {
    ECoreL3 = 0,      // [4:0]   L3 = CORE / n   (SDRAM controller resides on L3)
    ECoreL4,          // [6:5]   L4 = L3 / n     (slow peripheral bus)
    ECoreDss1,        // [12:8]  DSS1 = CORE / n (display)
    ECoreDss2,        // [13]    DSS2 source selector, not a divider
    ECoreVlynq,       // [19:15] VLYNQ = CORE / n
    ECoreSsi,         // [24:20] SSI = CORE / n  (in Nokia OMAPs - channel to modem)
    ECoreUsb,         // [27:25] USB; source not 100% established, so raw value only
    ECoreFieldCount
    };

struct TCoreField
    {
    const char* iName;
    TUint32     iMask;
    TInt        iShift;
    TUint32     iAllowed;    // bitmask of allowed values 0..31
    TInt        iBase;       // 0 = CORE, 1 = L3 (for L4), -1 = not a divider
    };

// MPU divider resides in a SEPARATE register CM_CLKSEL_MPU, not in CLKSEL1_CORE.
// Field [4:0] (mask 0x1F in power.dll setter @0x800AC410), and allowed values
// are 1, 2, 4, 6, 8: transition table @0x800AC3D0 rejects 0, 3, 5, 7. That is,
// divider 3 does not exist on this SoC, although the field could accommodate it.
const TUint32 KMpuDivMask = 0x0000001fu;
const TUint32 KMpuAllowed = (1u<<1)|(1u<<2)|(1u<<4)|(1u<<6)|(1u<<8);
TInt MpuDivStep(TInt aValue, TInt aDir);

// Narrower divider sets for AUTOMATIC selection. Reason - in comment at
// definition: hardware set is narrower than what firmware validates, and an invalid
// value in a SINGLE field rejects the ENTIRE write to CM_CLKSEL1_CORE.
extern const TUint32 KAutoDivAllowed;   // DSS1: 1, 2, 4, 5, 6, 8, 12, 16
extern const TUint32 KAutoL3Allowed;    // L3: same without 5 (validator rejects it)

const TCoreField& CoreField(TInt aId);
TInt CoreFieldValue(TUint32 aCoreReg, TInt aId);
TUint32 CoreFieldSet(TUint32 aCoreReg, TInt aId, TInt aValue);
// Next/previous ALLOWED field value. Returns old value if there is none further.
TInt CoreFieldStep(TInt aId, TInt aValue, TInt aDir);

class TOcClocks
    {
public:
    void Decode(TUint32 aClkSel1, TUint32 aClkSel2, TUint32 aMpuDiv, TUint32 aCoreDiv);
    TUint32 DpllKhz() const;
    TUint32 CoreKhz() const;
    TUint32 MpuKhz() const;
    TUint32 L3Khz() const;

    TUint32 iSysKhz;      // 19200, 13000 or 12000 - by field [25:23]
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
    EOcStepUnsupported,       // profile does not support EReadDvfs path (e.g. E90)
    EOcStepVerifyBody,        // bytes at patch address are not what we expected
    EOcStepFirmware,          // device firmware is not the one addresses were derived for
    EOcStepDiscover,          // scanning live XIP found no signatures
    EOcStepReady
    };

class COcEngine : public CBase
    {
public:
    static COcEngine* NewL();
    ~COcEngine();

    // Select device profile BEFORE Prepare(). Defaults to KProfileRM320.
    // Ignored after engine is already ready.
    void SetProfile(const TOcProfile& aProfile) { if (!iReady) iProfile = &aProfile; }
    const TOcProfile& Profile() const { return *iProfile; }

    // Does everything up to readiness. KErrNone = ready to overclock.
    // Works only for profile with EReadDvfs strategy (RM-320 and kin). For
    // a profile without dvfs (E90, iDvfsBody=0) deliberately refuses with EOcStepUnsupported,
    // rather than patching foreign kernel body (findings §27).
    TInt Prepare();
    TBool IsReady() const { return iReady; }
    TOcStep FailedAt() const { return iStep; }
    static const TDesC& StepName(TOcStep aStep);

    TUint32 SramAddress() const { return iSram; }
    TInt StockMult() const { return iStockClocks.iMult; }
    const TOcClocks& StockClocks() const { return iStockClocks; }
    // Layout for hypothetical multiplier - to show "what will be", before application.
    void ClocksFor(TInt aMult, TOcClocks& aOut) const;
    // Selects L3 and DSS1 dividers so their frequencies remain as close as possible
    // to stock at new CORE. Returns ready CM_CLKSEL1_CORE value.
    TUint32 CompensatedCore(TInt aMult) const;
    TUint32 StockCore() const { return iStockCore; }
    TUint32 StockMpu() const { return iStockMpu; }

    // State in which Prepare() FOUND the device. It no longer serves as reference
    // point (see KStockClkSel1Core), but it cannot be kept silent: discrepancy
    // with stock means previous run left behind a non-stock layout.
    TUint32 BootCore() const { return iBootCore; }
    TUint32 BootMpu() const { return iBootMpu; }
    TInt BootMult() const { return iBootMult; }
    // Compared against reference point of THIS device, not reference one. Otherwise on
    // E90, which has stock l3div=6 vs 4 on RM-320, every run would falsely report
    // "NOT STOCK AT START" on a completely stock device.
    // We take RESOLVED reference point (iStock*), not profile fields: in test
    // profiles those fields are zero, and direct comparison would yield "NON-STOCK" on
    // a completely stock device simply because profile does not populate them.
    TBool BootedNonStock() const;

    TInt ReadReg(TUint32 aAddr, TUint32& aOut);
    // Multiple addresses per single application of READ patch. Fewer windows where
    // foreign DoControl(1) can land - see comment at implementation.
    TInt ReadRegs(const TUint32* aAddr, TUint32* aOut, TInt aCount);

    // --- DSP domain (C55x) --------------------------------------------------
    // Divider is changed by calling STOCK power.dll setter via EXEC patch, not
    // by own MMIO write: setter validates value itself and preserves rest of
    // register itself. Neither CORE, nor SDRAM, nor display is affected.
    // aSetterRc - result of setter itself: 0 or KErrArgument.
    TInt ApplyDspDiv(TInt aDiv, TInt& aSetterRc);
    TInt RestoreDspDiv(TInt& aSetterRc);
    TInt LiveDspDiv(TInt& aOut);
    TInt StockDspDiv() const { return iStockDspDiv; }
    TInt DspDiv() const { return iDspDiv; }
    TBool IsDspChanged() const { return iDspDiv != iStockDspDiv; }
    // DSP frequency by firmware's own formula: CORE / divider.
    TUint32 DspKhz(TInt aDiv) const;
    // Rereads CM_CLKSEL1_PLL from device and returns actual multiplier.
    TInt LiveMult(TInt& aOut);
    // Live CM_CLKSEL1_CORE. Verify against what was intended to be written: invalid
    // value in a SINGLE field rejects entire write, and silently (findings §26).
    TInt LiveCore(TUint32& aOut);

    // Performs relock. aStatus: 0 = clean, bit0 = bypass timeout (multiplier NOT
    // written), bit1 = CLKCFG timeout, bit2 = lock timeout.
    TInt ApplyMult(TInt aMult, TUint32& aStatus);
    // Manual mode: multiplier AND dividers are specified by caller. Auto-selection does not work.
    // aMpuReg == 0 -> keep stock MPU divider.
    TInt ApplyMultAndCore(TInt aMult, TUint32 aCoreReg, TUint32 aMpuReg,
                          TUint32& aStatus);
    // Same result, but STEPWISE by one multiplier step at a time, with settle pause
    // between them. Reason: +1 step is stable even with live
    // display, while +2 and more hangs device (findings 20) - bypass window is longer,
    // and display controller does not survive L3 drop. One large jump is no
    // better than several small ones, so we do only small ones.
    TInt ApplyMultRamp(TInt aTarget, TUint32& aStatus);
    // Same, but with explicit dividers at each step.
    TInt ApplyRampWithCore(TInt aTarget, TUint32 aCoreReg, TUint32 aMpuReg,
                           TUint32& aStatus);
    // Rewrite ONLY dividers (CM_CLKSEL1_CORE and CM_CLKSEL_MPU) with latch
    // PRCM_CLKCFG_CTRL, without stopping DPLL. Needed to hold
    // layout while measured process runs in the same window: full relock
    // in the middle of such run is dangerous (findings §20), and this is the same
    // operation governor itself does on every OPP transition.
    // aRc: 0 = latch freed, 1 = timeout.
    TInt ReassertDividers(TUint32 aCoreReg, TUint32 aMpuReg, TInt& aRc);
    // Executes arbitrary ARM code blob in kernel mode via SRAM slot.
    // aBlob - ready code bytes, up to 2048 bytes. aResult - what blob
    // returned in r0.
    TInt ExecCustomBlob(const TDesC8 &aBlob, TInt &aResult);
    TInt RestoreStock(TUint32& aStatus);
    // Restores substituted DoControl(1) body to original state. Called from
    // destructor, but explicit call before exit is more reliable.
    TInt RestoreKernelCode();
    // Returns percentage of nominal from STOCK function. Non-zero value
    // means rollback took effect and kernel has original code again.
    TInt VerifyRestored();
    // Frees shadow page created by patcher for DoControl(1) body.
    // Without this each run leaves permanently allocated RAM page in kernel
    // on top of ROM code page.
    TInt UnshadowKernelPage();
    // Full removal of patch: bytes back + unshadow. Unshadow specifically makes this
    // reliable, because rewriting bytes does not invalidate I-cache.
    TInt EndPatchSession();

    // Bisection of reboot cause. Each level leaves behind exactly one
    // more thing than the previous:
    //   0 - loaded patcher.ldd, channel opened and closed;
    //   1 - + func 105 write into OWN buffer (kernel does not change);
    //   2 - + READ patch, one read, rollback with verification (ROM-shadow page);
    //   3 - full Prepare (everything as in working run).
    // The first level after which device crashes in idle points to the culprit.
    TInt SelfTestLevel(TInt aLevel, TInt& aVerify, TUint32& aReadValue);
    TBool IsOverclocked() const { return iApplied; }
    // By default destructor restores stock frequency: process dies, but
    // frequency remains, and silently releasing device overclocked would be improper.
    // But when overclock is applied SPECIFICALLY to use the device afterwards, this
    // must be disabled explicitly. Kernel patch is removed anyway - it is
    // dangerous for foreign calls (findings §20), while frequency is not.
    void KeepOnExit(TBool aKeep) { iKeep = aKeep; }
    TInt AppliedMult() const { return iAppliedMult; }
    // Layout that was actually submitted to hardware on last write. May
    // differ from requested: l3 field is clamped to live value because
    // bus increase is fatal (findings §38.2).
    TUint32 LastCoreWritten() const { return iLastCoreWritten; }
    // ETrue if bus is left reduced intentionally (refusal in RestoreStock or
    // clamp in ApplyMultAndCore). Reboot restores stock clocking.
    TBool RestoreNeedsReboot() const { return iRestoreNeedsReboot; }

    // Bus raise guard. Enabled by default: smaller l3div vs live is
    // a bus increase, and it killed the device four out of four times (findings §38.2).
    // Can ONLY be disabled explicitly, following the same principle "everything risky behind explicit
    // argument" by which rest of dangerous paths lives here. Disabling breaks nothing
    // permanently: nothing is written to flash, worst case is immediate reboot.

    // Firmware version READ from device (first line of SysUtil::GetSWVersion).
    // Empty until Prepare() is called.
    const TDesC& Firmware() const { return iFirmware; }
    // Global address found by search (0 if profile knew it itself).
    // Words captured from live ROM. Needed only for printing in probe: to see
    // that verification indeed matched, rather than falling back to constant branch.
    TUint32 OrigWord(TInt aI) const
        { return (aI >= 0 && aI < iOrigCount) ? iOrigWords[aI] : 0; }
    TInt OrigCount() const { return iOrigCount; }
    TUint32 FoundGlobal() const { return iFoundGlobal; }
    // Addresses found by scanning live XIP. Zero means "take from
    // profile": all proven devices behave this way.
    TUint32 HostBody() const
        { return iFoundBody ? iFoundBody : iProfile->iDvfsBody; }
    TUint32 SramRoutine() const
        { return iFoundSram ? iFoundSram : iProfile->iRomSramRoutine; }
    TUint32 FoundBody() const { return iFoundBody; }
    TUint32 FoundSramRoutine() const { return iFoundSram; }
    // Verification of search mechanism on device with known answer.
    TInt TryDiscoverGlobal(TUint32& aGlobal, TUint32& aSram);
    // Same for XIP scanning: runs search and returns found addresses without patching
    // anything. Needed to verify method on device where correct
    // answer is already known independently.
    TInt TryDiscoverRom(TUint32& aBody, TUint32& aSram);

private:
    COcEngine();
    void ConstructL();
    TInt CheckFirmware();
    TInt CaptureOriginal();
    TInt DiscoverRomAddresses();
    TInt ChaseSram(TUint32 aGlobal, TUint32& aSram);
    TInt DiscoverPwrDownGlobal(TUint32& aGlobal, TUint32& aSram);
    TInt Poke(TUint32 aAddr, const TDesC8& aBytes);
    TInt PokeWords(TUint32 aAddr, const TUint32* aWords, TInt aCount);
    TInt ApplyReadPatch();
    TInt ApplyExecPatch();
    // Prepare body AFTER applying READ patch. Placed separately on purpose: this way
    // Prepare removes patch (EndPatchSession) on EVERY exit from probe, both success
    // and any error. Otherwise abnormal exit would leave patched kernel function
    // permanently - fatal on a device with different addresses (findings §27).
    TInt PrepareProbe();
    TInt ReopenDvfs();
    TInt WriteRelockBlob(TUint32 aClkSel1Value, TUint32 aCoreValue,
                         TUint32 aMpuValue);
    TInt WriteDspBlob(TInt aDiv);
    TInt WriteDivBlob(TUint32 aCoreReg, TUint32 aMpuReg);

private:
    // Active device profile. All firmware-dependent addresses are taken from here,
    // not from constants, so porting to another ROM is populating a profile, not
    // editing code (findings §29). For now always &KProfileRM320.
    const TOcProfile* iProfile;
    RPatcherChannel iPatcher;
    RDvfsChannel    iDvfs;
    // Stock DoControl body words CAPTURED FROM LIVE ROM of this device. A constant
    // will not do here: it contains bl/ldr/b offsets that vary per build,
    // so rolling back with a constant on a foreign build would corrupt the driver until reboot.
    TUint32  iOrigWords[6];
    TInt     iOrigCount;
    TBool    iPatcherOpen;
    TBool    iDvfsOpen;
    TBool    iReady;
    TBool    iExecMode;        // which patch is currently applied
    TBool    iPatched;         // whether DoControl(1) body is currently substituted
    TBool    iShadowed;        // whether ROM-shadow page was created by patcher
    TBool    iApplied;
    TBool    iKeep;            // do not restore frequency in destructor
    // Raising bus divider back to stock value = 4 deaths out of 4 on two
    // devices (findings 38.2). Therefore RestoreStock refuses to do this
    // and leaves device on reduced bus, telling user to reboot.
    TBool    iRestoreNeedsReboot;
    TBuf<64> iFirmware;
    TUint32  iFoundGlobal;     // what search found; 0 = profile knew address itself
    TUint32  iFoundBody;       // host DoControl body found by scanning XIP
    TUint32  iFoundSram;       // relock routine found in the same place
    TUint32  iLastCoreWritten;
    TInt     iAppliedMult;
    TUint32  iSram;
    TUint32  iStockClkSel1;
    TUint32  iStockCore;       // STOCK CM_CLKSEL1_CORE (constant)
    TUint32  iStockMpu;        // STOCK CM_CLKSEL_MPU (constant)
    TUint32  iBootCore;        // what was actually set at process start
    TUint32  iBootMpu;
    TInt     iBootMult;
    TInt     iStockDspDiv;     // CM_CLKSEL_DSP[3:0] at start
    TInt     iDspDiv;          // what is set currently
    TOcClocks iStockClocks;
    TOcStep  iStep;
    };

#endif
