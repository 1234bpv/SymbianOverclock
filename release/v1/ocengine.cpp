// Overclock engine implementation. See ocengine.h and docs/findings.md 17-18.

#include "ocengine.h"
#include <e32svr.h>

_LIT(KPatcherFile, "patcher.ldd");
_LIT(KPatcherName, "RomPatcherPlus.Driver");
_LIT(KDvfsFile, "dvfs_interface.ldd");
_LIT(KDvfsName, "dvfs_interface");

// Body of dvfs_interface DoControl(1): exactly 6 words, 0x80148410..0x80148427.
// The dispatcher starts at 0x80148428 and remains untouched.
const TUint32 KDvfsBody = 0x80148410u;
// power.dll global: [+4] -> power-down object, [obj+16] -> linear SRAM address.
const TUint32 KPwrDownGlobal = 0xc80030c8u;
// Source of copy in ROM: used to verify that we found the executable copy specifically.
const TUint32 KRomSramRoutine = 0x800a7f6cu;
// Firmware occupies the first 1024 bytes of the chunk; writing at offset 2048 of 4096.
const TUint32 KSramSlot = 0x800u;

// --- code executed from SRAM ------------------------------------------------
//
// Requirements are strict and non-obvious:
//   * no external bl - after copying, relative jumps would lead
//     nowhere. Constants come from own pool (.ltorg at the end), so ldr [pc,#off]
//     remains correct at any address;
//   * no access to SDRAM during the critical window: while DPLL is in bypass,
//     the SDRAM controller is clocked at a tiny frequency and refresh starves. The stack
//     is touched only before and after the window;
//   * EN_DPLL is BITS [1:0] of CM_CLKEN_PLL, not [2:0]. Bits [3:2] are APLL
//     54 MHz; mask #7 would disable it along with DPLL. Mask #3 was taken from
//     power.dll code itself (and r2,r2,#0xfc);
//   * all waits are counter-bounded: interrupts are disabled, so an
//     infinite loop would mean pulling the battery.
//
// Order follows omap242x_sram_set_prcm and power.dll idle routine @0x800A7F6C.
__asm__(
"   .text                       \n"
"   .align 2                    \n"
"   .code 32                    \n"
"   .global OcReadBlobStart     \n"
"   .global OcReadBlobEnd       \n"
"OcReadBlobStart:               \n"
"   mrs  r1, cpsr               \n"
"   orr  r2, r1, #0xC0          \n"
"   msr  cpsr_c, r2             \n"
"   ldr  r3, =0xC6008000        \n"
"   ldr  r0, [r3, #0x540]       \n"
"   msr  cpsr_c, r1             \n"
"   bx   lr                     \n"
"   .ltorg                      \n"
"OcReadBlobEnd:                 \n"
"                               \n"
// EXEC patch of the DoControl(1) body. Critical: dvfs_interface channel is opened
// not only by us (§15 - mmrmserver.dll, fmradiotunercontrol.dll), and in an app with
// a live UI these clients are active. A simple "bx r1" would mean any
// of their calls to DoControl(1, a1) jumps in the kernel to a THIRD-PARTY address - hence
// a freeze, and the more such windows we open, the more certain it is.
// Therefore we jump ONLY to one expected address (our SRAM slot), and return 0
// to all others - harmlessly, just like the READ patch with a1=0.
// Exactly 6 words = 24 bytes, which is the exact size of the original body.
"   .global OcExecPatchStart    \n"
"   .global OcExecPatchEnd      \n"
"OcExecPatchStart:              \n"
"   ldr  r0, [pc, #12]          \n"   // -> literal below
"   cmp  r1, r0                 \n"
"   bxeq r1                     \n"
"   mov  r0, #0                 \n"
"   bx   lr                     \n"
"   .word 0xDEADBEEF            \n"   // allowed address; patched externally
"OcExecPatchEnd:                \n"
"                               \n"
"   .global OcRelockBlobStart   \n"
"   .global OcRelockBlobEnd     \n"
"OcRelockBlobStart:             \n"
"   stmfd sp!, {r4-r11, lr}     \n"
"   ldr  r4, =0xC6008000        \n"
"   ldr  r5, =0xC6129000        \n"
"   ldr  r6, =0xDEADBEEF        \n"   // new CM_CLKSEL1_PLL; patched externally
"   ldr  r11, =0xFEEDFACE       \n"   // initial, for hardware rollback
"   ldr  r2, =0xCAFEBABE        \n"   // new CM_CLKSEL1_CORE (dividers)
"   ldr  r3, =0xBAADF00D        \n"   // initial CM_CLKSEL1_CORE
"   mov  r7, #0                 \n"
"   mrs  r8, cpsr               \n"
"   orr  r9, r8, #0xC0          \n"
"   msr  cpsr_c, r9             \n"
// SDRAM into self-refresh BEFORE the window. Without this step, the memory controller
// remains in normal mode while CORE (and with it L3 and SDRAM) drops to sys_clk for the
// entire bypass+relock duration. A headless run survived this because memory traffic
// was near zero; with an active display continuously reading the framebuffer,
// the device froze completely, and more reliably so the larger the multiplier step (longer
// DPLL lock = longer window). The value 0x48 and this exact spot in the
// sequence come from the idle routine of power.dll @0x800A7F6C (findings 17), i.e.,
// vendor code for this exact SoC.
"   ldr  r0, [r5, #0x70]        \n"
"   orr  r0, r0, #0x48          \n"
"   str  r0, [r5, #0x70]        \n"
"   ldr  r0, [r5, #0x70]        \n"   // readback: the write must post
"   ldr  r0, [r4, #0x500]       \n"
"   bic  r0, r0, #3             \n"
"   orr  r0, r0, #2             \n"
"   str  r0, [r4, #0x500]       \n"
"   mov  r10, #0x10000          \n"
"1: ldr  r0, [r4, #0x520]       \n"
"   and  r0, r0, #3             \n"
"   cmp  r0, #1                 \n"
"   beq  2f                     \n"
"   subs r10, r10, #1           \n"
"   bne  1b                     \n"
"   orr  r7, r7, #1             \n"
"   b    3f                     \n"
"2: str  r6, [r4, #0x540]       \n"
// CORE domain dividers are written HERE, together with the multiplier: a single latch
// via CLKCFG_CTRL latches both PLL and CLKSEL1_CORE, so L3/SDRAM and DSS never
// see the new CORE with the old divider. That was precisely what was missing: the display
// is clocked by CORE/DSS1_div, and any CORE change without divider compensation
// shifted its pixel clock - hence rendering hangs, symmetrically even when
// underclocking (findings §21).
"   str  r2, [r4, #0x240]       \n"
"   mov  r0, #1                 \n"
"   str  r0, [r4, #0x80]        \n"
"   mov  r10, #0x10000          \n"
"4: ldr  r0, [r4, #0x80]        \n"
"   tst  r0, #1                 \n"
"   beq  3f                     \n"
"   subs r10, r10, #1           \n"
"   bne  4b                     \n"
"   orr  r7, r7, #2             \n"
"3: ldr  r0, [r4, #0x500]       \n"
"   bic  r0, r0, #3             \n"
"   orr  r0, r0, #3             \n"
"   str  r0, [r4, #0x500]       \n"
"   mov  r10, #0x10000          \n"
"5: ldr  r0, [r4, #0x520]       \n"
"   and  r0, r0, #3             \n"
"   cmp  r0, #2                 \n"
"   beq  6f                     \n"
"   subs r10, r10, #1           \n"
"   bne  5b                     \n"
"   orr  r7, r7, #4             \n"
// HARDWARE ROLLBACK. If DPLL failed to lock on the new multiplier, it cannot be left
// in bypass: CORE then equals sys_clk, MPU drops to ~9.6 MHz, and the device looks
// completely frozen, although formally alive. We return to bypass, write the
// INITIAL multiplier and lock onto it - all within the same critical section
// while interrupts are still disabled.
"   ldr  r0, [r4, #0x500]       \n"
"   bic  r0, r0, #3             \n"
"   orr  r0, r0, #2             \n"
"   str  r0, [r4, #0x500]       \n"
"   mov  r10, #0x10000          \n"
"8: ldr  r0, [r4, #0x520]       \n"
"   and  r0, r0, #3             \n"
"   cmp  r0, #1                 \n"
"   beq  9f                     \n"
"   subs r10, r10, #1           \n"
"   bne  8b                     \n"
"9: str  r11, [r4, #0x540]      \n"   // initial CM_CLKSEL1_PLL
"   str  r3, [r4, #0x240]       \n"   // and initial dividers
"   mov  r0, #1                 \n"
"   str  r0, [r4, #0x80]        \n"
"   ldr  r0, [r4, #0x500]       \n"
"   bic  r0, r0, #3             \n"
"   orr  r0, r0, #3             \n"
"   str  r0, [r4, #0x500]       \n"
"   mov  r10, #0x10000          \n"
"0: ldr  r0, [r4, #0x520]       \n"
"   and  r0, r0, #3             \n"
"   cmp  r0, #2                 \n"
"   beq  6f                     \n"
"   subs r10, r10, #1           \n"
"   bne  0b                     \n"
"   orr  r7, r7, #8             \n"   // rollback failed as well
"6: ldr  r0, [r5, #0x64]        \n"
"   and  r0, r0, #0xFF00        \n"
"   ldr  r1, [r5, #0x60]        \n"
"   and  r1, r1, #0x0F          \n"
"   orr  r1, r1, #4             \n"
"   orr  r0, r0, r1             \n"
"   str  r0, [r5, #0x60]        \n"
"   ldr  r0, [r5, #0x6C]        \n"
"   and  r0, r0, #0xFF00        \n"
"   ldr  r1, [r5, #0x68]        \n"
"   and  r1, r1, #0x0F          \n"
"   orr  r1, r1, #4             \n"
"   orr  r0, r0, r1             \n"
"   str  r0, [r5, #0x68]        \n"
"   mov  r10, #0x400            \n"
"7: subs r10, r10, #1           \n"
"   bne  7b                     \n"
"   msr  cpsr_c, r8             \n"
"   ldr  r0, [r4, #0x540]       \n"
"   bic  r0, r0, #0xF0000000    \n"
"   orr  r0, r0, r7, lsl #28    \n"
"   ldmfd sp!, {r4-r11, pc}     \n"
"   .ltorg                      \n"
"OcRelockBlobEnd:               \n"
);

extern "C" void OcRelockBlobStart(void);
extern "C" void OcRelockBlobEnd(void);
extern "C" void OcExecPatchStart(void);
extern "C" void OcExecPatchEnd(void);

// Original 6 words of the DoControl(1) body, taken from ROM disassembly
// (out/asm/dvfs_if.asm, 0x80148410..0x80148427). Keeping them here is mandatory:
// without rollback our patch remains in ROM-shadow TO LIVE AFTER PROCESS EXIT, meaning
// a replaced kernel function hangs in the system permanently (until reboot). If it is
// ever called by non-our code and not with zero in a1, we dereference an arbitrary
// address in kernel mode - the device reboots.
static const TUint32 KDvfsOriginal[6] =
    { 0xe92d4010u,   // stmdb sp!, {r4, lr}
      0xe3a00001u,   // mov   r0, #1
      0xeb00014cu,   // bl    0x80148950
      0xe59f11d0u,   // ldr   r1, [pc, #464]
      0xe8bd4010u,   // ldmia sp!, {r4, lr}
      0xea000135u }; // b     0x80148900

// READ patch: cmp r1,#0 ; ldrne r0,[r1] ; moveq r0,#0 ; bx lr
static const TUint32 KReadPatch[4] =
    { 0xe3510000u, 0x15910000u, 0x03a00000u, 0xe12fff1eu };


// --- channels ---------------------------------------------------------------

TInt RPatcherChannel::Open()
    {
    return DoCreate(KPatcherName, TVersion(0,0,0), KNullUnit, NULL, NULL,
                    EOwnerProcess);
    }

TInt RPatcherChannel::WriteBytes(TUint32& aAddress, const TDesC8& aBytes)
    {
    return DoControl(105, (TAny*)&aAddress, (TAny*)&aBytes);
    }

TInt RPatcherChannel::FreeShadow(TUint32& aAddress, TUint32& aLength)
    {
    return DoControl(100, (TAny*)&aAddress, (TAny*)&aLength);
    }

TInt RDvfsChannel::Open()
    {
    return DoCreate(KDvfsName, TVersion(0,0,0), KNullUnit, NULL, NULL,
                    EOwnerProcess);
    }

TInt RDvfsChannel::ReadOrCall(TUint32 aAddress)
    {
    return DoControl(1, (TAny*)aAddress);
    }

// --- clock layout -----------------------------------------------------------

void TOcClocks::Decode(TUint32 aClkSel1, TUint32 aClkSel2, TUint32 aMpuDiv,
                       TUint32 aCoreDiv)
    {
    TUint32 sysSel = (aClkSel1 >> 23) & 7;
    iSysKhz = (sysSel == 0) ? 19200u : ((sysSel == 2) ? 13000u : 12000u);
    iMult = (TInt)((aClkSel1 >> 12) & 0x3ff);
    iDiv = (TInt)((aClkSel1 >> 8) & 0xf);
    iCoreSel = (TInt)(aClkSel2 & 3);
    iMpuDiv = (TInt)(aMpuDiv & 0xf);
    iL3Div = (TInt)(aCoreDiv & 0x1f);
    iDss1Div = (TInt)((aCoreDiv & KCoreDss1Mask) >> KCoreDss1Shift);
    }

TUint32 TOcClocks::Dss1Khz() const
    {
    return iDss1Div ? CoreKhz() / (TUint32)iDss1Div : 0;
    }

TUint32 TOcClocks::DpllKhz() const
    {
    return (iDiv + 1) ? (iSysKhz * (TUint32)iMult) / (TUint32)(iDiv + 1) : 0;
    }

TUint32 TOcClocks::CoreKhz() const { return DpllKhz() * (TUint32)iCoreSel; }

TUint32 TOcClocks::MpuKhz() const
    {
    return iMpuDiv ? CoreKhz() / (TUint32)iMpuDiv : 0;
    }

TUint32 TOcClocks::L3Khz() const
    {
    return iL3Div ? CoreKhz() / (TUint32)iL3Div : 0;
    }

// --- engine -----------------------------------------------------------------

COcEngine::COcEngine()
  : iPatcherOpen(EFalse), iDvfsOpen(EFalse), iReady(EFalse), iExecMode(EFalse), iPatched(EFalse), iShadowed(EFalse),
    iApplied(EFalse), iKeep(EFalse), iAppliedMult(0), iSram(0), iStockClkSel1(0), iStockCore(0),
    iStep(EOcStepIdle)
    {
    }

COcEngine* COcEngine::NewL()
    {
    COcEngine* self = new (ELeave) COcEngine();
    CleanupStack::PushL(self);
    self->ConstructL();
    CleanupStack::Pop(self);
    return self;
    }

void COcEngine::ConstructL() {}

COcEngine::~COcEngine()
    {
    // Order is mandatory and strictly as follows:
    //   1. remove overclock - frequency survives the process;
    //   2. restore original kernel bytes - the patch also survives the process, and
    //      leaving a replaced function in the system means setting a trap for later;
    //   3. only then close the channels (without the patcher, step 2 is impossible).
    if (iApplied && !iKeep && iSram)
        {
        TUint32 st = 0;
        RestoreStock(st);
        }
    RestoreKernelCode();
    // Last - remove shadow. After this, neither our bytes nor the page
    // allocated for them remain in the kernel.
    UnshadowKernelPage();
    if (iDvfsOpen)
        iDvfs.Close();
    if (iPatcherOpen)
        iPatcher.Close();
    }

// Restores the DoControl(1) body to its original form. Safe to call repeatedly.
//
// We write all 6 words: the EXEC patch occupies exactly that much. Previously 4 words stood here
// with the justification "16 bytes is the proven memmove path"; that justification was
// a misdiagnosis - the real reason for the failed rollback was the I-cache, not the
// write length (findings §19).
TInt COcEngine::RestoreKernelCode()
    {
    if (!iPatcherOpen || !iPatched)
        return KErrNone;
    TInt r = PokeWords(KDvfsBody, KDvfsOriginal, 6);
    if (r == KErrNone)
        {
        iPatched = EFalse;
        iExecMode = EFalse;
        }
    return r;
    }

// Removes ROM-shadow from the DoControl(1) body: the page is restored to stock
// mapping directly to ROM, and the RAM page allocated by the patcher is freed.
// Done ONCE, on termination: we already restore the contents after each
// operation anyway, whereas kernel page alloc/free on every read is unnecessary churn.
TInt COcEngine::UnshadowKernelPage()
    {
    if (!iPatcherOpen || !iShadowed)
        return KErrNone;
    TUint32 addr = KDvfsBody;
    TUint32 len = 16;
    TInt r = iPatcher.FreeShadow(addr, len);
    if (r == KErrNone)
        iShadowed = EFalse;
    return r;
    }

// Rollback verification. There is nothing to read the restored bytes with - the read primitive
// IS those bytes. But the original function has known behavior: DoControl(1, 0)
// returns percentage of nominal (findings 3), i.e., 50 or 100. Our READ patch
// with a1=0 returns exactly 0, and the EXEC patch would jump to a null address.
// Therefore a non-zero percentage here is proof that stock code is in the kernel again.
TInt COcEngine::VerifyRestored()
    {
    if (!iDvfsOpen || iPatched)
        return KErrNotReady;
    return iDvfs.ReadOrCall(0);
    }

_LIT(KStepIdle,        "idle");
_LIT(KStepLoadDriver,  "loading patcher.ldd");
_LIT(KStepOpenPatcher, "opening RomPatcherPlus.Driver");
_LIT(KStepSelfTest,    "patcher self-test");
_LIT(KStepReadPatch,   "applying READ patch");
_LIT(KStepOpenDvfs,    "opening dvfs_interface");
_LIT(KStepResolveSram, "resolving SRAM chunk");
_LIT(KStepVerifySram,  "verifying SRAM copy");
_LIT(KStepReadRegs,    "reading PRCM registers");
_LIT(KStepReady,       "ready");

const TDesC& COcEngine::StepName(TOcStep aStep)
    {
    switch (aStep)
        {
        case EOcStepLoadDriver:  return KStepLoadDriver;
        case EOcStepOpenPatcher: return KStepOpenPatcher;
        case EOcStepSelfTest:    return KStepSelfTest;
        case EOcStepReadPatch:   return KStepReadPatch;
        case EOcStepOpenDvfs:    return KStepOpenDvfs;
        case EOcStepResolveSram: return KStepResolveSram;
        case EOcStepVerifySram:  return KStepVerifySram;
        case EOcStepReadRegs:    return KStepReadRegs;
        case EOcStepReady:       return KStepReady;
        default:                 return KStepIdle;
        }
    }

TInt COcEngine::Poke(TUint32 aAddr, const TDesC8& aBytes)
    {
    if (!iPatcherOpen)
        return KErrNotReady;
    TUint32 addr = aAddr;
    return iPatcher.WriteBytes(addr, aBytes);
    }

TInt COcEngine::PokeWords(TUint32 aAddr, const TUint32* aWords, TInt aCount)
    {
    TBuf8<512> b;
    if (aCount * 4 > b.MaxLength())
        return KErrOverflow;
    b.SetLength(aCount * 4);
    for (TInt i = 0; i < aCount; ++i)
        {
        b[i*4+0] = (TUint8)(aWords[i]);
        b[i*4+1] = (TUint8)(aWords[i] >> 8);
        b[i*4+2] = (TUint8)(aWords[i] >> 16);
        b[i*4+3] = (TUint8)(aWords[i] >> 24);
        }
    return Poke(aAddr, b);
    }

// The channel has to be reopened after each patch change: the driver holds
// state, and we need to guarantee that the next DoControl(1) goes into the fresh body.
TInt COcEngine::ReopenDvfs()
    {
    if (iDvfsOpen)
        {
        iDvfs.Close();
        iDvfsOpen = EFalse;
        }
    TInt r = iDvfs.Open();
    if (r == KErrNone)
        iDvfsOpen = ETrue;
    return r;
    }

// We do NOT reopen the channel. The verified probe run opened it exactly
// once, and reopening here accomplishes nothing: the driver dispatcher calls
// the patched body via "bl 0x80148410" directly, caching nothing. Unnecessary
// close/open cycles mean extra operations on the kernel driver for every read,
// while the device was stable prior to this work, so there is no reason to introduce new churn.
TInt COcEngine::ApplyReadPatch()
    {
    TInt r = PokeWords(KDvfsBody, KReadPatch, 4);
    if (r != KErrNone)
        return r;
    iExecMode = EFalse;
    iPatched = ETrue;
    iShadowed = ETrue;
    return KErrNone;
    }

TInt COcEngine::ApplyExecPatch()
    {
    // We take bytes from the assembly template, not from an array of constants: instruction
    // encoding is computed by the assembler, not manually by me.
    const TUint8* p = (const TUint8*)((TUint32)&OcExecPatchStart & ~1u);
    const TUint8* q = (const TUint8*)((TUint32)&OcExecPatchEnd & ~1u);
    TInt len = (TInt)(q - p);
    if (len != 24)
        return KErrCorrupt;
    TBuf8<24> patch;
    patch.Copy(p, len);

    // Substitute the single address to which jumping is allowed.
    const TUint32 allowed = iSram + KSramSlot;
    TBool found = EFalse;
    for (TInt i = 0; i + 4 <= len; i += 4)
        {
        TUint32 w = (TUint32)patch[i] | ((TUint32)patch[i+1] << 8) |
                    ((TUint32)patch[i+2] << 16) | ((TUint32)patch[i+3] << 24);
        if (w != 0xdeadbeefu)
            continue;
        patch[i+0] = (TUint8)(allowed);
        patch[i+1] = (TUint8)(allowed >> 8);
        patch[i+2] = (TUint8)(allowed >> 16);
        patch[i+3] = (TUint8)(allowed >> 24);
        found = ETrue;
        break;
        }
    if (!found)
        return KErrNotFound;

    TInt r = Poke(KDvfsBody, patch);
    if (r != KErrNone)
        return r;
    iExecMode = ETrue;
    iPatched = ETrue;
    iShadowed = ETrue;
    return KErrNone;
    }

// Removes the patch RELIABLY. Rewriting bytes is NOT sufficient for this: no patcher
// path executes Cache::IMB (findings §14), and the patched function has already
// executed, so its instructions remain in the I-cache. Observed directly:
// after restoring bytes, DoControl(1,0) returned 0, meaning the patch was alive.
// FreeShadowPage instead discards the RAM page and restores mapping to
// real ROM - along with proper cache maintenance.
// Bytes are still rewritten first: if unshadow ever fails, page contents
// must still be stock.
TInt COcEngine::EndPatchSession()
    {
    RestoreKernelCode();
    return UnshadowKernelPage();
    }

TInt COcEngine::ReadReg(TUint32 aAddr, TUint32& aOut)
    {
    if (!iPatcherOpen)
        return KErrNotReady;
    TInt r = ApplyReadPatch();
    if (r != KErrNone)
        return r;
    aOut = (TUint32)iDvfs.ReadOrCall(aAddr);
    EndPatchSession();
    return KErrNone;
    }

TInt COcEngine::SelfTestLevel(TInt aLevel, TInt& aVerify, TUint32& aReadValue)
    {
    aVerify = -1;
    aReadValue = 0;

    iStep = EOcStepLoadDriver;
    TInt r = User::LoadLogicalDevice(KPatcherFile);
    if (r != KErrNone && r != KErrAlreadyExists)
        return r;

    iStep = EOcStepOpenPatcher;
    r = iPatcher.Open();
    if (r != KErrNone)
        return r;
    iPatcherOpen = ETrue;
    if (aLevel < 1)
        return KErrNone;

    // Level 1: func 105 contract on own buffer. Kernel is not touched.
    iStep = EOcStepSelfTest;
    TBuf8<4> target;
    TBuf8<4> bytes;
    target.SetLength(4);
    bytes.SetLength(4);
    target[0] = target[1] = target[2] = target[3] = 0;
    bytes[0] = 0x51; bytes[1] = 0xa7; bytes[2] = 0x3c; bytes[3] = 0xd2;
    TUint32 selfAddr = (TUint32)target.Ptr();
    r = iPatcher.WriteBytes(selfAddr, bytes);
    if (r != KErrNone || target.Compare(bytes) != 0)
        return (r != KErrNone) ? r : KErrCorrupt;
    if (aLevel < 2)
        return KErrNone;

    // Level 2: first touch of kernel code. This is where the ROM-shadow page arises,
    // and it remains until reboot even after contents are restored.
    iStep = EOcStepOpenDvfs;
    User::LoadLogicalDevice(KDvfsFile);
    r = ReopenDvfs();
    if (r != KErrNone)
        return r;

    iStep = EOcStepReadPatch;
    r = ApplyReadPatch();
    if (r != KErrNone)
        return r;
    aReadValue = (TUint32)iDvfs.ReadOrCall(KRegClkSel1Pll);
    // Verify AFTER unshadow, not after rewriting bytes: unshadow specifically
    // is the reliable patch removal.
    EndPatchSession();
    aVerify = VerifyRestored();
    if (aLevel < 3)
        return KErrNone;

    return Prepare();
    }

TInt COcEngine::Prepare()
    {
    iStep = EOcStepLoadDriver;
    TInt r = User::LoadLogicalDevice(KPatcherFile);
    if (r != KErrNone && r != KErrAlreadyExists)
        return r;
    // dvfs_interface is already in ROM, but an attempt won't hurt.
    User::LoadLogicalDevice(KDvfsFile);

    iStep = EOcStepOpenPatcher;
    r = iPatcher.Open();
    if (r != KErrNone)
        return r;
    iPatcherOpen = ETrue;

    // Self-test of func 105 contract on own buffer: kernel is not touched.
    iStep = EOcStepSelfTest;
    TBuf8<4> target;
    TBuf8<4> bytes;
    target.SetLength(4);
    bytes.SetLength(4);
    target[0] = target[1] = target[2] = target[3] = 0;
    bytes[0] = 0x51; bytes[1] = 0xa7; bytes[2] = 0x3c; bytes[3] = 0xd2;
    TUint32 selfAddr = (TUint32)target.Ptr();
    r = iPatcher.WriteBytes(selfAddr, bytes);
    if (r != KErrNone || target.Compare(bytes) != 0)
        return (r != KErrNone) ? r : KErrCorrupt;

    iStep = EOcStepReadPatch;
    r = PokeWords(KDvfsBody, KReadPatch, 4);
    if (r != KErrNone)
        return r;
    iExecMode = EFalse;
    iPatched = ETrue;

    iStep = EOcStepOpenDvfs;
    r = ReopenDvfs();
    if (r != KErrNone)
        return r;

    iStep = EOcStepResolveSram;
    TUint32 obj = (TUint32)iDvfs.ReadOrCall(KPwrDownGlobal + 4);
    if (obj < 0xc0000000u || obj >= 0xf0000000u || (obj & 3))
        return KErrNotFound;
    TUint32 sram = (TUint32)iDvfs.ReadOrCall(obj + 16);
    if (sram < 0xc0000000u || (sram & 3))
        return KErrNotFound;

    // We do not trust the address without verification: first word of the copy must equal
    // first word of the original in ROM. Otherwise we would be writing to random kernel memory.
    iStep = EOcStepVerifySram;
    TUint32 w0 = (TUint32)iDvfs.ReadOrCall(sram);
    TUint32 romw0 = (TUint32)iDvfs.ReadOrCall(KRomSramRoutine);
    if (w0 != romw0)
        return KErrCorrupt;
    iSram = sram;

    iStep = EOcStepReadRegs;
    TUint32 sel1 = (TUint32)iDvfs.ReadOrCall(KRegClkSel1Pll);
    TUint32 sel2 = (TUint32)iDvfs.ReadOrCall(KRegClkSel2Pll);
    TUint32 mpud = (TUint32)iDvfs.ReadOrCall(KRegClkSelMpu);
    TUint32 core = (TUint32)iDvfs.ReadOrCall(KRegClkSel1Core);
    if (((sel1 >> 12) & 0x3ff) == 0)
        return KErrCorrupt;
    iStockClkSel1 = sel1;
    iStockCore = core;
    // core_sel is read sometimes as 1, sometimes as 2: the DVFS governor moves it at
    // runtime (findings 12, 18). An instantaneous reading here is misleading - catching
    // the device at a lower OPP, we would display 165 MHz instead of 330. So we take
    // a series UNDER LOAD and take the maximum: the governor raises OPP only
    // when there is work, and interference can slow down a reading, never inflate it.
    TUint32 bestSel2 = sel2 & 3;
    for (TInt k = 0; k < 8; ++k)
        {
        volatile TUint32 x = 1;
        for (TInt i = 0; i < 150000; ++i)
            x = x * 1664525u + 1013904223u;
        TUint32 v = (TUint32)iDvfs.ReadOrCall(KRegClkSel2Pll) & 3;
        if (v > bestSel2)
            bestSel2 = v;
        }
    sel2 = bestSel2 ? bestSel2 : 2;
    iStockClocks.Decode(sel1, sel2, mpud, core);
    iAppliedMult = iStockClocks.iMult;

    // Read batch completed - removing patch completely. Going forward, each operation
    // applies it on its own, strictly for the duration of its execution.
    EndPatchSession();

    iStep = EOcStepReady;
    iReady = ETrue;
    return KErrNone;
    }

void COcEngine::ClocksFor(TInt aMult, TOcClocks& aOut) const
    {
    aOut = iStockClocks;
    aOut.iMult = aMult;
    }

// Selection of dividers for the new CORE. Goal is to keep L3 and DSS1 as close to their
// STOCK frequencies as possible: their shift was what broke the device, not the core frequency itself.
// Dividers are integers, so exact compensation is not always possible; at equal
// error we prefer LOWER frequency - it is safer both for the panel
// and for memory (refresh is counted in L3 cycles, so a slower bus means
// more frequent refresh, not less frequent).
static TInt PickDivider(TUint32 aNewCoreKhz, TUint32 aWantKhz, TInt aMin, TInt aMax)
    {
    if (!aWantKhz)
        return aMin;
    TInt best = aMin;
    TUint32 bestErr = 0xffffffffu;
    for (TInt d = aMin; d <= aMax; ++d)
        {
        TUint32 got = aNewCoreKhz / (TUint32)d;
        TUint32 err = (got > aWantKhz) ? (got - aWantKhz) : (aWantKhz - got);
        // Strictly less: at equal error, the LARGER divider remains, i.e.,
        // lower frequency.
        if (err < bestErr)
            {
            bestErr = err;
            best = d;
            }
        }
    return best;
    }

TUint32 COcEngine::CompensatedCore(TInt aMult) const
    {
    TOcClocks want;
    ClocksFor(aMult, want);
    const TUint32 newCore = want.CoreKhz();
    if (!newCore)
        return iStockCore;

    // DSS1 - exact compensation to the nearest. This is the frequency whose shift broke
    // rendering, and we do not need it higher: the display does not care about speed,
    // it needs ITS OWN frequency.
    const TInt dss = PickDivider(newCore, iStockClocks.Dss1Khz(), 1, 31);

    // L3/SDRAM - on the contrary, we keep AS HIGH AS POSSIBLE within tolerance. Compensating
    // it "to nearest" would be a mistake: at mult 96 this would drop memory
    // below stock, whereas benchmarking showed that it comfortably holds +11.5% with
    // zero errors. So we take the smallest divider under which L3 does not
    // exceed stock by more than 15%.
    const TUint32 l3Cap = iStockClocks.L3Khz() + iStockClocks.L3Khz() / 7u;
    TInt l3 = iStockClocks.iL3Div ? iStockClocks.iL3Div : 4;
    for (TInt d = 1; d <= 31; ++d)
        {
        if (newCore / (TUint32)d <= l3Cap)
            {
            l3 = d;
            break;
            }
        }
    if (l3 < iStockClocks.iL3Div)
        l3 = iStockClocks.iL3Div;      // never overclock the bus higher than it would end up naturally

    TUint32 v = iStockCore;
    v = (v & ~KCoreL3Mask) | (((TUint32)l3 << KCoreL3Shift) & KCoreL3Mask);
    v = (v & ~KCoreDss1Mask) | (((TUint32)dss << KCoreDss1Shift) & KCoreDss1Mask);
    return v;
    }

TInt COcEngine::LiveMult(TInt& aOut)
    {
    TUint32 v = 0;
    TInt r = ReadReg(KRegClkSel1Pll, v);
    if (r != KErrNone)
        return r;
    aOut = (TInt)((v >> 12) & 0x3ff);
    return KErrNone;
    }

TInt COcEngine::WriteRelockBlob(TUint32 aClkSel1Value, TUint32 aCoreValue)
    {
    const TUint8* p = (const TUint8*)((TUint32)&OcRelockBlobStart & ~1u);
    const TUint8* q = (const TUint8*)((TUint32)&OcRelockBlobEnd & ~1u);
    TInt len = (TInt)(q - p);
    if (len <= 0 || len > 480)
        return KErrCorrupt;
    TBuf8<512> blob;
    blob.Copy(p, len);

    // Two stub words: target value and initial value (for hardware rollback
    // inside the critical section). Both must be found, otherwise the blob is not what
    // we expect, and executing it is unsafe.
    TInt patched = 0;
    for (TInt i = 0; i + 4 <= len; i += 4)
        {
        TUint32 w = (TUint32)blob[i] | ((TUint32)blob[i+1] << 8) |
                    ((TUint32)blob[i+2] << 16) | ((TUint32)blob[i+3] << 24);
        TUint32 v = 0;
        if (w == 0xdeadbeefu)
            v = aClkSel1Value;
        else if (w == 0xfeedfaceu)
            v = iStockClkSel1;
        else if (w == 0xcafebabeu)
            v = aCoreValue;
        else if (w == 0xbaadf00du)
            v = iStockCore;
        else
            continue;
        blob[i+0] = (TUint8)(v);
        blob[i+1] = (TUint8)(v >> 8);
        blob[i+2] = (TUint8)(v >> 16);
        blob[i+3] = (TUint8)(v >> 24);
        ++patched;
        }
    if (patched != 4)
        return KErrNotFound;
    return Poke(iSram + KSramSlot, blob);
    }

TInt COcEngine::ApplyMult(TInt aMult, TUint32& aStatus)
    {
    aStatus = 0;
    if (!iReady)
        return KErrNotReady;
    if (aMult < KOcMultMin || aMult > KOcMultMax)
        return KErrArgument;

    // Change exactly bits [21:12], preserve the rest of the register.
    TUint32 target = (iStockClkSel1 & ~0x003ff000u) |
                     (((TUint32)aMult & 0x3ff) << 12);

    TInt r = WriteRelockBlob(target, CompensatedCore(aMult));
    if (r != KErrNone)
        return r;
    r = ApplyExecPatch();
    if (r != KErrNone)
        return r;

    TUint32 rc = (TUint32)iDvfs.ReadOrCall(iSram + KSramSlot);
    aStatus = rc >> 28;

    // Remove the patch IMMEDIATELY and completely: measurements follow, and keeping the
    // replaced kernel function body all that time is prohibited.
    EndPatchSession();

    iAppliedMult = aMult;
    iApplied = (aMult != iStockClocks.iMult);
    return KErrNone;
    }

TInt COcEngine::ApplyMultRamp(TInt aTarget, TUint32& aStatus)
    {
    aStatus = 0;
    if (!iReady)
        return KErrNotReady;
    if (aTarget < KOcMultMin || aTarget > KOcMultMax)
        return KErrArgument;

    // We step from the ACTUAL multiplier, not the remembered one: if the previous
    // attempt rolled back on its own, hardware state may not match ours.
    TInt cur = 0;
    TInt r = LiveMult(cur);
    if (r != KErrNone)
        return r;
    if (cur < KOcMultMin || cur > KOcMultMax)
        return KErrCorrupt;

    while (cur != aTarget)
        {
        cur += (aTarget > cur) ? 1 : -1;
        TUint32 st = 0;
        r = ApplyMult(cur, st);
        aStatus |= st;
        if (r != KErrNone)
            return r;
        // Non-zero status means the blob already rolled back on its own to the previous
        // value. We go no further: the next step would only repeat the failure.
        if (st != 0)
            return KErrAbort;
        User::After(20000);            // 20 ms for subsystems to settle
        }
    return KErrNone;
    }

TInt COcEngine::RestoreStock(TUint32& aStatus)
    {
    TInt r = ApplyMultRamp(iStockClocks.iMult, aStatus);
    if (r == KErrNone)
        iApplied = EFalse;
    return r;
    }
