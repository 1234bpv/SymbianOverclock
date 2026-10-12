// Overclock engine implementation. See ocengine.h and docs/findings.md 17-18.

#include "ocengine.h"
#include <e32svr.h>
#include <hal.h>
#include <sysutil.h>

_LIT(KPatcherFile, "patcher.ldd");
_LIT(KPatcherName, "RomPatcherPlus.Driver");
_LIT(KDvfsFile, "dvfs_interface.ldd");
_LIT(KDvfsName, "dvfs_interface");
// Primitive host where dvfs is absent: standard USB client driver.
_LIT(KUsbcName, "Usbc");

// dvfs_interface DoControl(1) body: exactly 6 words, 0x80148410..0x80148427.
// The dispatcher starts at 0x80148428 and remains untouched.
const TUint32 KDvfsBody = 0x80148410u;
// power.dll global: [+4] -> power-down object, [obj+16] -> SRAM linear address.
const TUint32 KPwrDownGlobal = 0xc80030c8u;
// ROM copy source: used for verification that we found the executable copy.
const TUint32 KRomSramRoutine = 0x800a7f6cu;

// Reference profile. Values are the same constants proven on the device, just
// gathered in one place. RM-320 behavior does not change by a single bit from this;
// the point is that ANOTHER device is now added as a second such entry, not
// by modifying code. Numbers for a new profile are taken from unpacked z: (§29):
// dvfsBody - DoControl(1) body offset; romSram - power.dll idle routine; dspSetter -
// DSP divider setter; pwrDown - power-down global. Where dvfs is absent in firmware
// (E90 etc.) - iRead is different, and iDvfsBody = 0.
const TUint32 KSramRoutineSignature[8] =
    { 0xe92d40feu,   // stmdb sp!, {r1-r7, lr}
      0xe59f1128u,   // ldr   r1, [pc, #296]
      0xe59f5128u,   // ldr   r5, [pc, #296]
      0xe3a03000u,   // mov   r3, #0
      0xee073f9au,   // mcr   p15, 0, r3, c7, c10, 4   write barrier
      0xe5914070u,   // ldr   r4, [r1, #112]           SDRC
      0xe3844048u,   // orr   r4, r4, #72
      0xe5814070u }; // str   r4, [r1, #112]

const TOcProfile KProfileRM320 =
    {
    "RM-320 35.0.001/31.0.015",
    0x20002d84u,            // HAL EMachineUid, retrieved from device
    EReadDvfs,
    EHostDvfs,
    KDvfsBody,
    KRomSramRoutine,
    KDspSetter,
    KPwrDownGlobal,
    KStockClkSel1Core,
    KStockClkSelMpu,
    KStockMult,
    // Two builds, and reasons for them are DIFFERENT. 35.0.001 is what is installed on the device
    // on which the entire overclock was proven (verified on 2026-09-11 by gate: the device
    // reports "V 35.0.001"). 31.0.015 is the image from which addresses were derived
    // offline (out/roms/rm320.rom), and profile name until now mentioned only it.
    // The fact that the same addresses work on both is proof that between these builds
    // code did not shift.
    "35.0.001;31.0.015",
    EFalse,                 // proven on device
    0xc80030ecu,            // hint: needed only to VERIFY search
    KSramRoutineSignature,
    EFalse
    };

// --- Nokia E90 (RA-6, 400.34.93) --------------------------------------------
//
// FIXED 2026-09-09 (findings §34). Three addresses that were here previously
// (0x80056874, 0x80059aa4, 0xc8004194) were derived from out/roms/e90.rom under
// the assumption that offset in the file linearly translates to virtual address. The device
// DISPROVED this assumption: reading live XIP from user mode yielded different code
// at those addresses, and the ROM itself on device is 19776 KB vs 1.7 MB in the extracted
// image. Therefore the addresses are zeroed - an incorrect number is more dangerous than an absent one. A method
// to get correct ones already exists and is tested: signature scan of live ROM
// (ocprobe romscan / romdump), which already measured Usbc host 0x800bf874.
// dvfs is NOT present in E90 firmware (verified: "dvfs" in XIP relates to the display), so
// Usbc acts as the READ/EXEC primitive host here: iHost = EHostUsbc, and iDvfsBody is
// the address of DLddUsbcChannel::DoControl body. Patch mechanics are shared with RM-320,
// differing only in opcode, argument register, and rollback words (findings §32, §34).
//
// The three addresses below were MEASURED on a live device on 2026-09-09 via signature scan
// (romscan -> romdump -> objdump), rather than derived from an offline image:
//   iDvfsBody       0x800bf874  DoControl body; prologue verified, dispatcher at
//                               0x800c09a4 calls specifically it and allows opcode 69;
//   iRomSramRoutine 0x80058da4  relock routine: DSB, SDRC+0x70 |= 0x48, silicon
//                               revision check, bypass, WFI, relock, DLL 0x770E -
//                               structurally the same as on RM-320, and it is its
//                               address that the constructor passes to memmove to SRAM;
//   iPwrDownGlobal  0xc8003068  global into which the power-down object is stored
//                               (str r0,[r5,#4] @0x80059448).
// DSP divider setter is not yet measured - it is not needed for overclock, it is used
// only by the secondary DSP branch, so it remains 0 and the branch disables itself.
//
// STOCK clocking layout was MEASURED on the device on 2026-09-09 via the Usbc primitive
// (findings §34), not derived offline and not guessed:
//   CM_CLKSEL1_PLL  = 0x00056400  mult=86 div=4 sys_clk=19.2 MHz
//   CM_CLKSEL2_PLL  = 2, CM_CLKSEL_MPU = 2, CM_CLKSEL1_CORE = 0x04360626
//   CM_CLKEN_PLL    = 0x0f (DPLL in lock)
// That is 19.2*86/5 = 330.24 MHz on DPLL, and after x2 / /2 the same on MPU - E90
// runs stock at the same 330.24 MHz as RM-320. Guess "86" is confirmed.
// This is a snapshot of ONE moment; on E90 there is no CPU dvfs (§27), so layout
// should be constant, but on first WRITE run it should be reread.
// mult=97 value from bootstrap (§33) does not apply to OS working state, and §33
// addresses themselves were derived from a layout disproven by the device.
const TUint32 KE90HostBody       = 0x800bf874u;  // DLddUsbcChannel::DoControl
const TUint32 KE90RomSramRoutine = 0x80058da4u;
const TUint32 KE90DspSetter      = 0u;   // not needed for overclock, not measured
const TUint32 KE90PwrDownGlobal  = 0xc8003068u;
const TOcProfile KProfileE90 =
    {
    "RA-6 400.34.93",
    0x20002496u,            // HAL EMachineUid, retrieved from device
    EReadDvfs,              // read via patch of host's DoControl body
    EHostUsbc,              // host is Usbc, because dvfs is absent in firmware
    KE90HostBody,
    KE90RomSramRoutine,
    KE90DspSetter,
    KE90PwrDownGlobal,
    0x04360626,             // iStockClkSel1Core: MEASURED on device 2026-09-09
    2,                      // iStockClkSelMpu:   MEASURED in the same place
    86,                     // iStockMult: MEASURED (CM_CLKSEL1_PLL=0x00056400,
                            //   mult=86 div=4 sys_clk=19.2 -> 330.24 MHz, like RM-320)
    "400.34.93",
    EFalse,                 // proven on device
    0xc80030ecu,            // hint: same, to verify search
    KSramRoutineSignature,
    EFalse                  // addresses are known, nothing to search
    };

// --- TEST profiles ---------------------------------------------------------
//
// Both were derived OFFLINE from firmware images and have not been verified
// on hardware even once. Therefore they are never picked automatically: machine uid
// in them is zero, and ProfileForThisDevice skips them. Such a profile can only be selected
// manually and deliberately.
//
// What specifically is proven, and what is not. Three power.dll addresses were verified
// byte-for-byte against the RM-320 reference directly in images: in RM-159 they match
// one-to-one, in N82 they are shifted by exactly +0x10, and code in those places is identical.
// The iDvfsBody address was found by DoControl(1) body signature - a single match across
// the entire 43-megabyte image, where on reference image the same signature accurately
// reproduces the known address 0x80148410, meaning the method is verified on a known answer.
//
// What is NOT proven. Power-down global could not be found offline: signature of the
// getter/setter pair yields a neighbouring pair on RM-320 (0xc80030ec instead of true
// 0xc80030c8), meaning the method is NOT confirmed. Therefore a hint is placed here,
// and the address itself is searched on the device with SRAM copy verification. Build versions
// are also unknown: there is no version string in resident XIP, so the firmware gate for these
// profiles is empty, and its role is assumed by verifying relock routine signature after patching.
//
// Stock clocking layout is unknown - zeroes mean "take from device at start".
// Because of this, the device before first launch must be freshly rebooted,
// otherwise the reference point will drift.
const TOcProfile KProfileN95Classic =
    {
    "RM-159 N95 classic (TEST)",
    0,                      // machine uid unknown: no auto-selection
    EReadDvfs,
    EHostDvfs,
    0x8014b5f0u,            // iDvfsBody: only signature match in image
    0x800a7f6cu,            // iRomSramRoutine: byte-for-byte like RM-320
    0x800ac59cu,            // iDspSetter: byte-for-byte like RM-320
    0,                      // iPwrDownGlobal: search on device
    0,                      // iStockClkSel1Core: retrieve from device
    0,                      // iStockClkSelMpu: retrieve from device
    0,                      // iStockMult: retrieve from device
    "",                     // build unknown, gate is empty
    ETrue,
    0xc80030ecu,            // global search hint
    KSramRoutineSignature,
    EFalse
    };

const TOcProfile KProfileN82 =
    {
    "RM-313 N82 (TEST)",
    0,
    EReadDvfs,
    EHostDvfs,
    0x801411e0u,            // iDvfsBody: only signature match in image
    0x800a7f7cu,            // iRomSramRoutine: RM-320 + 0x10, code identical
    0x800ac5acu,            // iDspSetter: RM-320 + 0x10, code identical
    0,
    0,
    0,
    0,
    "",
    ETrue,
    0xc80030ecu,
    KSramRoutineSignature,
    EFalse
    };

// --- profiles that discover addresses THEMSELVES ----------------------------
//
// N93 (RM-55) and N93i (RM-156) are the same OMAP 2420, but offline addresses
// cannot be provided for them, and this is not caution, but a measurable fact. XIP
// extracted from firmware in them, as in E90, is a fragment: 1.1 and 1.3 MB vs almost 20 MB
// on device, and file offset does not equal address. For E90 the difference was measured
// directly: DoControl body is in the file at 0x80094ac4, and on the device at
// 0x800bf874 (findings 47.1). A constant derived from such an image would point
// to an alien page.
//
// There is no version of dvfs driver in N93 and N93i firmwares - just like on E90 -
// so Usbc remains the primitive host.
//
// Hence iDiscover: addresses are discovered by the device itself via signature scanning of live
// XIP, reading from user mode. If not found or found twice -
// engine rejects, and nothing is written to the kernel. These profiles are NOT verified
// on hardware even once; they exist specifically so that device owners can
// try and report back the result.
const TOcProfile KProfileN93 =
    {
    "RM-55 N93 (TEST, auto)",
    0,                      // machine uid unknown: no auto-selection
    EReadDvfs,
    EHostUsbc,
    0,                      // DoControl body: discover on device
    0,                      // relock routine: discover on device
    0,                      // DSP setter unknown - ApplyDspDiv will reject
    0,                      // power-down global: search around hint
    0, 0, 0,                // stock layout: retrieve from device
    "",                     // build unknown, gate is empty
    ETrue,
    0xc80030ecu,            // global search hint (as on other OMAP2420)
    KSramRoutineSignature,
    ETrue                   // discover addresses via scanning
    };

const TOcProfile KProfileN93i =
    {
    "RM-156 N93i (TEST, auto)",
    0,
    EReadDvfs,
    EHostUsbc,
    0, 0, 0, 0,
    0, 0, 0,
    "",
    ETrue,
    0xc80030ecu,
    KSramRoutineSignature,
    ETrue
    };

// Test profiles as a separate list - specifically so they cannot be picked accidentally.
static const TOcProfile* const KTestProfiles[] =
    { &KProfileN95Classic, &KProfileN82, &KProfileN93, &KProfileN93i };

TInt TestProfileCount()
    {
    return (TInt)(sizeof(KTestProfiles) / sizeof(KTestProfiles[0]));
    }

const TOcProfile* TestProfile(TInt aIndex)
    {
    if (aIndex < 0 || aIndex >= TestProfileCount())
        return NULL;
    return KTestProfiles[aIndex];
    }

// Profile selection for device. Keep list here, next to profiles themselves.
//
// Deliberately do NOT make a fallback "if not recognized - take reference".
// A profile is four addresses patched into the kernel; on an unrecognized device
// they point nowhere, and the cost of error is a reboot at least.
// An unknown device must get an honest rejection, not a guess.
const TOcProfile* ProfileForThisDevice()
    {
    const TOcProfile* const known[] = { &KProfileRM320, &KProfileE90 };
    const TInt n = sizeof(known) / sizeof(known[0]);

    TInt uid = 0;
    if (HAL::Get(HALData::EMachineUid, uid) != KErrNone)
        return NULL;

    for (TInt i = 0; i < n; ++i)
        if (!known[i]->iExperimental && known[i]->iMachineUid == (TUint32)uid)
            return known[i];
    return NULL;
    }

// Firmware occupies the first 1024 bytes of the chunk; we write at offset 2048 of 4096.
const TUint32 KSramSlot = 0x800u;

// --- code executed from SRAM ------------------------------------------------
//
// Strict and non-obvious requirements:
//   * no external bl - after copying, relative jumps would lead to
//     nowhere. Constants - from own pool (.ltorg at the end), so ldr [pc,#off]
//     remains valid at any address;
//   * no SDRAM access in the critical window: while DPLL is in bypass,
//     SDRAM controller is clocked at a tiny frequency and refresh is starved. Stack
//     is touched only before and after the window;
//   * EN_DPLL is BITS [1:0] of CM_CLKEN_PLL, not [2:0]. Bits [3:2] are APLL
//     54 MHz; mask #7 would disable it along with DPLL. Mask #3 is taken from code
//     of power.dll itself (and r2,r2,#0xfc);
//   * all waits are bounded by counter: interrupts are disabled, so
//     an infinite loop would mean pulling the battery.
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
// EXEC patch of DoControl(1) body. Critical: dvfs_interface channel is opened not
// only by us (§15 - mmrmserver.dll, fmradiotunercontrol.dll), and in an application with
// a live interface these clients are active. A simple "bx r1" would mean that any of
// their DoControl(1, a1) calls jumps in the kernel to an UNEXPECTED address - hence
// hangs, and all the more certain the more such windows we open.
// Therefore we jump ONLY to one expected address (our SRAM slot), and return 0
// to all others - harmlessly, like the READ patch on a1=0.
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
// Same for Usbc host, where address arrives in r2 (findings §34). Caution
// here is the same and no less necessary: Usbc channel is also not our own, so we jump
// only to the single allowed address, returning 0 to the rest.
"   .global OcExecPatchR2Start  \n"
"   .global OcExecPatchR2End    \n"
"OcExecPatchR2Start:            \n"
"   ldr  r0, [pc, #12]          \n"   // -> literal below
"   cmp  r2, r0                 \n"
"   bxeq r2                     \n"
"   mov  r0, #0                 \n"
"   bx   lr                     \n"
"   .word 0xDEADBEEF            \n"   // allowed address; patched externally
"OcExecPatchR2End:              \n"
"                               \n"
// Trampoline to STOCK DSP divider setter in power.dll (@0x800AC59C).
//
// Why we call vendor function instead of writing to CM_CLKSEL_DSP ourselves:
//   * the setter validates values itself (rejects 0, 5, 9, 16, 18) and returns -6,
//     meaning firmware forbids writing what it considers invalid for this SoC -
//     exactly the same guardrails the project already takes for CM_CLKSEL1_CORE;
//   * internally it does Modify32 with mask 0x1F, so the rest of the register
//     is preserved by vendor, verified code, not by our read-modify-write;
//   * we do not introduce a new kernel patch form. WRITE patch of DoControl(1) body
//     ("strne r2,[r1]") would be a worse target than READ for third-party
//     dvfs_interface clients, and moreover it simply does not work: dispatcher @0x80148428
//     destroys a2 (mov r3,#0) and passes r1 == r2 == a1 to the body.
// We call the function DIRECTLY IN ROM without copying: all its own bl calls remain
// valid, in particular bl to Modify32.
//
// Exactly one argument - divider in r1; setter ignores r0 (overwrites it immediately).
// Returns 0 or -6, and this value reaches the caller as the result of
// DoControl(1): EXEC patch does bxeq r1, which is a tail call.
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
// SDRAM into self-refresh BEFORE the window. Without this step, the memory controller remains
// in normal mode while CORE (and with it L3 and SDRAM) drops to sys_clk for the entire
// bypass+relock duration. A headless run survived this because memory traffic
// was almost nonexistent; with a live display continuously reading the framebuffer,
// the device hung hard, all the more certainly the larger the multiplier step (longer
// DPLL lock time = longer window). The 0x48 value and this exact place in the
// sequence are from power.dll idle routine @0x800A7F6C (findings 17), i.e.
// vendor code for this same SoC.
"   ldr  r0, [r5, #0x70]        \n"
"   orr  r0, r0, #0x48          \n"
"   str  r0, [r5, #0x70]        \n"
"   ldr  r0, [r5, #0x70]        \n"   // read back: write must take effect
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
// Write CORE domain dividers HERE, together with multiplier: a single commit
// of CLKCFG_CTRL latches both PLL and CLKSEL1_CORE, so L3/SDRAM and DSS never
// see new CORE with old divider. This is exactly what was missing: display
// is clocked by CORE/DSS1_div, and any CORE change without divider compensation
// shifted its pixel clock - hence rendering freeze, symmetrically,
// even when lowering frequency (findings §21).
"   str  r2, [r4, #0x240]       \n"
// MPU divider - in the same commit. It allows setting target core frequency
// at TWICE lower CORE (div 1 instead of 2), thus testing hypothesis that the limit
// is not in ARM itself, but in CORE as the highest internal frequency.
"   ldr  r0, =0xF00DBEEF        \n"
"   str  r0, [r4, #0x140]       \n"
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
// HARDWARE ROLLBACK. If DPLL failed to lock on the new multiplier, leaving it in
// bypass is unacceptable: CORE then equals sys_clk, MPU drops to ~9.6 MHz, and device
// looks completely frozen, although formally alive. We return to bypass,
// write INITIAL multiplier, and lock on it - all within the same
// critical section while interrupts are still disabled.
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
"   ldr  r0, =0xFEEDC0DE        \n"   // initial CM_CLKSEL_MPU
"   str  r0, [r4, #0x140]       \n"
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
"   orr  r7, r7, #8             \n"   // rollback also failed
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
"                               \n"
// Trampoline to STOCK DSP divider setter in power.dll (@0x800AC59C).
//
// Why we call vendor function instead of writing to CM_CLKSEL_DSP ourselves:
//   * the setter validates values itself (rejects 0, 5, 9, 16, 18) and returns -6,
//     meaning firmware forbids writing what it considers invalid for this SoC -
//     exactly the same guardrails the project already takes for CM_CLKSEL1_CORE;
//   * internally it does Modify32 (@0x800A1C50) - leaf function with IRQ/FIQ off
//     and DMB, without locks or calls, so from our context it is safe, while
//     the rest of the register bits are preserved by vendor verified code;
//   * we do not introduce a new kernel patch form. WRITE patch of DoControl(1) body
//     ("strne r2,[r1]") would be a worse target than READ for third-party
//     dvfs_interface clients, and moreover it simply does not work: dispatcher @0x80148428
//     destroys a2 (mov r3,#0) and passes r1 == r2 == a1 to the body.
// We call the function DIRECTLY IN ROM without copying: all its own bl calls remain
// valid, in particular bl to Modify32.
//
// COMMIT. Writing to CM_CLKSEL_DSP alone is NOT ENOUGH - verified on hardware:
// setter returned 0, but the register retained its old value. The new value
// is latched only by writing 1 to PRCM_CLKCFG_CTRL (+0x80), just as for CLKSEL1_CORE
// and CLKSEL_MPU in the relock blob above. Stock setter does not do this - and that
// is evidently why it is never called anywhere in ROM: in this form it does not
// work. We hold the sequence under disabled interrupts: DVFS governor in
// runtime moves CM_CLKSEL2_PLL itself and commits it with the same latch, so between
// our write and our commit it must not intervene.
//
// Returns: 0 = written, -6 = firmware rejected value, 1 = latch did not
// clear within allocated cycles.
"   .global OcDspBlobStart      \n"
"   .global OcDspBlobEnd        \n"
"OcDspBlobStart:                \n"
"   stmfd sp!, {r4, r5, lr}     \n"
"   mrs  r5, cpsr               \n"
"   orr  r3, r5, #0xC0          \n"
"   msr  cpsr_c, r3             \n"
"   ldr  r1, =0xD5D1D1D1        \n"   // divider; patched externally
"   ldr  r2, =0xD5A0A0A0        \n"   // setter address; patched externally
"   mov  r0, #0                 \n"
"   blx  r2                     \n"
"   movs r4, r0                 \n"
"   bne  13f                    \n"   // setter rejected - nothing to commit
"   ldr  r3, =0xC6008000        \n"
"   mov  r0, #1                 \n"
"   str  r0, [r3, #0x80]        \n"
"   mov  r2, #0x10000           \n"
"11:ldr  r0, [r3, #0x80]        \n"
"   tst  r0, #1                 \n"
"   beq  13f                    \n"
"   subs r2, r2, #1             \n"
"   bne  11b                    \n"
"   mov  r4, #1                 \n"   // latch timeout
"13:msr  cpsr_c, r5             \n"
"   mov  r0, r4                 \n"
"   ldmfd sp!, {r4, r5, pc}     \n"
"   .ltorg                      \n"
"OcDspBlobEnd:                  \n"
"                               \n"
// Rewrite ONLY dividers - CM_CLKSEL1_CORE and CM_CLKSEL_MPU - and commit them
// with PRCM_CLKCFG_CTRL latch. DPLL is not stopped during this.
//
// Why a separate blob when relock can already write the same two registers. Because
// relock puts DPLL into bypass every time for this, and that is the same window that hangs
// the device with an active display (findings §20, §21). Here CORE does not change
// at all, only dividers derived from it change - exactly what the governor
// itself does during OPP transition. So this is not a new class of operation for this SoC, but
// the same one the firmware uses continuously.
//
// This path is needed for one thing: hold layout while measured process runs
// in the same window. Customer observed that written dividers live for seconds
// and return to stock word; rewriting them via full relock during an emulator
// run is impossible, but this way - possible.
//
// Returns 0 if latch cleared, and 1 if not.
//
// NOT VERIFIED ON DEVICE as of 2026-08-15: code written, compiled, but
// never executed. Activation - only via explicit key hold=1.
"   .global OcDivBlobStart      \n"
"   .global OcDivBlobEnd        \n"
"OcDivBlobStart:                \n"
"   mrs  r12, cpsr              \n"
"   orr  r3, r12, #0xC0         \n"
"   msr  cpsr_c, r3             \n"
"   ldr  r3, =0xC6008000        \n"
"   ldr  r0, =0xD1C0DE01        \n"   // new CM_CLKSEL1_CORE; patched externally
"   str  r0, [r3, #0x240]       \n"
"   ldr  r0, =0xD1C0DE02        \n"   // new CM_CLKSEL_MPU; patched externally
"   str  r0, [r3, #0x140]       \n"
"   mov  r1, #1                 \n"
"   str  r1, [r3, #0x80]        \n"
"   mov  r2, #0x10000           \n"
"   mov  r0, #1                 \n"   // until latch clears - this is timeout
"21:ldr  r1, [r3, #0x80]        \n"
"   tst  r1, #1                 \n"
"   moveq r0, #0                \n"
"   beq  23f                    \n"
"   subs r2, r2, #1             \n"
"   bne  21b                    \n"
"23:msr  cpsr_c, r12            \n"
"   bx   lr                     \n"
"   .ltorg                      \n"
"OcDivBlobEnd:                  \n"
);

extern "C" void OcRelockBlobStart(void);
extern "C" void OcRelockBlobEnd(void);
extern "C" void OcExecPatchStart(void);
extern "C" void OcExecPatchEnd(void);
extern "C" void OcExecPatchR2Start(void);
extern "C" void OcExecPatchR2End(void);
extern "C" void OcDspBlobStart(void);
extern "C" void OcDspBlobEnd(void);
extern "C" void OcDivBlobStart(void);
extern "C" void OcDivBlobEnd(void);

// Original 6 words of DoControl(1) body, retrieved from ROM disassembly
// (out/asm/dvfs_if.asm, 0x80148410..0x80148427). Keeping them here is mandatory:
// without rollback our patch remains in ROM-shadow TO LIVE AFTER PROCESS EXITS, i.e.
// a replaced kernel function hangs in the system forever (until reboot). If
// it is ever called by non-our code and with non-zero in a1, we dereference an arbitrary
// address in kernel mode - the device will reboot.
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

// Same for Usbc host, where address arrives in r2 (findings §32/§34):
//   cmp r2,#0 ; ldrne r0,[r2] ; moveq r0,#0 ; bx lr
static const TUint32 KReadPatchR2[4] =
    { 0xe3520000u, 0x15920000u, 0x03a00000u, 0xe12fff1eu };

// Stock 6 words of DLddUsbcChannel::DoControl body on E90, RETRIEVED FROM LIVE ROM
// (romdump 0x800bf874, findings §34) - not from offline image whose layout
// was disproven by device. Same role as KDvfsOriginal: without rollback
// replaced body of foreign driver remains in ROM-shadow until reboot.
static const TUint32 KUsbcOriginal[6] =
    { 0xe92d4ff0u,   // stmdb sp!, {r4-r9, sl, fp, lr}
      0xe24dd044u,   // sub   sp, sp, #68
      0xe1a04000u,   // mov   r4, r0
      0xe1a06002u,   // mov   r6, r2
      0xe1a07001u,   // mov   r7, r1
      0xe3a01000u }; // mov   r1, #0

// Selection by profile. Keep in one place so branches do not diverge.
static const TUint32* HostOriginal(const TOcProfile* aP, TInt& aWords)
    {
    if (aP->iHost == EHostUsbc) { aWords = 6; return KUsbcOriginal; }
    aWords = 6; return KDvfsOriginal;
    }

static const TUint32* HostReadPatch(const TOcProfile* aP)
    {
    return (aP->iHost == EHostUsbc) ? KReadPatchR2 : KReadPatch;
    }


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
    if (iHost == EHostUsbc)
        {
        // USB client driver opens without a cable; we select version because
        // it differs across firmwares (verified: on E90 it takes 0.0.0).
        const TVersion vers[] = { TVersion(0,0,0), TVersion(1,0,0), TVersion(2,0,0) };
        TInt r = KErrNotFound;
        for (TInt i = 0; i < 3 && r != KErrNone; ++i)
            r = DoCreate(KUsbcName, vers[i], KNullUnit, NULL, NULL, EOwnerProcess);
        return r;
        }
    return DoCreate(KDvfsName, TVersion(0,0,0), KNullUnit, NULL, NULL,
                    EOwnerProcess);
    }

TInt RDvfsChannel::ReadOrCall(TUint32 aAddress)
    {
    // Usbc dispatcher passes opcode 69 to DoControl even when the channel is not
    // ready (USB without a cable) - which is why this host was chosen (findings §32).
    return DoControl((iHost == EHostUsbc) ? 69 : 1, (TAny*)aAddress);
    }

// --- CM_CLKSEL1_CORE fields -------------------------------------------------
//
// Allowed values are exactly those accepted by power.dll setters. Each does a
// cmp/jump table before Modify32; whatever fails gets KErrNotSupported.
// Allowed masks below are collected from those checks (findings §21).
#define ALL_EXCEPT(n)  (0xfffffffeu & ~(1u << (n)))

// FIXED 2026-08-15. For L3, ALL_EXCEPT(18) stood here - meaning "all allowed
// except 18". This was a validation reading error.
//
// L3 validator @0x800AC424 reads as follows:
//   cmp r1,#5 / beq reject          -> 5 rejected
//   bgt >5                          -> branch for larger
//   cmp r1,#0 / cmpne r1,#3 / bne accept   -> 0 and 3 rejected
//   cmp r1,#9 / cmpne r1,#18 / bne accept  -> 9 and 18 rejected
// That is, 0, 3, 5, 9, 18 are rejected - the same list in form as in the
// DSP divider setter (0, 5, 9, 16, 18).
//
// BUT THIS TABLE ALONE IS NOT ENOUGH, and that is the main lesson of §26. DSS1 validator
// @0x800AC49C rejects ONLY 18, yet the device refused to take `dssdiv=7`: two
// runs with it (in the second - with known-valid `l3div=6`) did not change
// the register at all, whereas the same run with `dssdiv=6` wrote successfully. Thus the hardware
// set of dividers is narrower than what firmware validates, and an invalid value in
// ONE field rejects the ENTIRE write to CM_CLKSEL1_CORE, not just its own field.
//
// Hence two different sets. Here - the wide one from firmware validation plus measured
// exclusion of 7 for DSS1: manual mode remains flexible, and device refusal
// is caught by read-back (MISMATCH line). For AUTOMATIC selection,
// the deliberately narrower KAutoDivAllowed is used - there is no margin for error there.
static const TCoreField KCoreFields[ECoreFieldCount] =
    {
    // name       mask          shift allowed              base
    { "L3   ",  0x0000001fu,   0, ALL_EXCEPT(18) & ~((1u<<3)|(1u<<5)|(1u<<9)), 0 },
    { "L4   ",  0x00000060u,   5, (1u<<1)|(1u<<2),        1 },
    { "DSS1 ",  0x00001f00u,   8, ALL_EXCEPT(18) & ~(1u<<7), 0 },
    { "DSS2 ",  0x00002000u,  13, (1u<<0)|(1u<<1),       -1 },
    { "VLYNQ",  0x000f8000u,  15, ALL_EXCEPT(5),          0 },
    { "SSI  ",  0x01f00000u,  20, (1u<<1)|(1u<<2)|(1u<<3)|(1u<<4)|(1u<<6)|(1u<<8), 0 },
    { "USB  ",  0x0e000000u,  25, (1u<<1)|(1u<<2)|(1u<<4), -1 }
    };

// Set for AUTOMATIC selection: 1, 2, 4, 5, 6, 8, 12, 16.
//
// Narrower than firmware validation, and intentionally so: an invalid value means
// NOTHING gets written, and CORE will run with old dividers - exactly the situation
// that broke rendering (§21). Here is only what is either measured or indisputable.
//
// Measured on device: 2, 4, 6, and 8 write successfully; 5 too (device worked for 13 hours
// with dssdiv=5 left by another run); 7 does NOT write.
// Values 1, 12, and 16 were not tested, but they do not appear in selection for the working
// multiplier range.
//
// 3 and 9 are deliberately NOT included: L3 validator rejects them, and for DSS1 they are not
// verified. The cost - display compensation is inaccurate on multipliers below ~60, where
// divider 3 is specifically needed. For working range 86..111 this value is not
// needed; for ramp down to 43 it is needed, but that path is blocked anyway
// (§26.10).
const TUint32 KAutoDivAllowed =
    (1u<<1)|(1u<<2)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<8)|(1u<<12)|(1u<<16);

// For L3 - same, but without 5: validator @0x800AC424 rejects it. Two sets,
// not one, for this exact reason: a shared set would either allow an invalid value
// for L3 or unnecessarily remove a valid one for DSS1.
const TUint32 KAutoL3Allowed = KAutoDivAllowed & ~(1u<<5);

TInt MpuDivStep(TInt aValue, TInt aDir)
    {
    for (TInt v = aValue + aDir; v >= 0 && v <= 31; v += aDir)
        {
        if (KMpuAllowed & (1u << v))
            return v;
        }
    return aValue;
    }

// Next/previous DSP divider from those allowed by firmware. Same mechanics
// as in MpuDivStep: guardrails are taken from stock setter validation, not from
// our own notions of what "should work".
TInt DspDivStep(TInt aValue, TInt aDir)
    {
    for (TInt v = aValue + aDir; v >= 1 && v <= 31; v += aDir)
        {
        if (KDspDivAllowed & (1u << v))
            return v;
        }
    return aValue;
    }

const TCoreField& CoreField(TInt aId)
    {
    if (aId < 0 || aId >= ECoreFieldCount)
        aId = 0;
    return KCoreFields[aId];
    }

TInt CoreFieldValue(TUint32 aCoreReg, TInt aId)
    {
    const TCoreField& f = CoreField(aId);
    return (TInt)((aCoreReg & f.iMask) >> f.iShift);
    }

TUint32 CoreFieldSet(TUint32 aCoreReg, TInt aId, TInt aValue)
    {
    const TCoreField& f = CoreField(aId);
    return (aCoreReg & ~f.iMask) | ((((TUint32)aValue) << f.iShift) & f.iMask);
    }

TInt CoreFieldStep(TInt aId, TInt aValue, TInt aDir)
    {
    const TCoreField& f = CoreField(aId);
    // Field width bounds range: for 1-bit DSS2 it is 0..1, for 5-bit 0..31.
    TInt maxv = (TInt)(f.iMask >> f.iShift);
    for (TInt v = aValue + aDir; v >= 0 && v <= maxv; v += aDir)
        {
        if (v < 32 && (f.iAllowed & (1u << v)))
            return v;
        }
    return aValue;                 // no more valid values - leave as was
    }

// --- clock schedule ---------------------------------------------------------

void TOcClocks::Decode(TUint32 aClkSel1, TUint32 aClkSel2, TUint32 aMpuDiv,
                       TUint32 aCoreDiv)
    {
    TUint32 sysSel = (aClkSel1 >> 23) & 7;
    iSysKhz = (sysSel == 0) ? 19200u : ((sysSel == 2) ? 13000u : 12000u);
    iMult = (TInt)((aClkSel1 >> 12) & 0x3ff);
    iDiv = (TInt)((aClkSel1 >> 8) & 0xf);
    iCoreSel = (TInt)(aClkSel2 & 3);
    iMpuDiv = (TInt)(aMpuDiv & KMpuDivMask);
    iL3Div = (TInt)(aCoreDiv & 0x1f);
    iDss1Div = (TInt)((aCoreDiv & KCoreDss1Mask) >> KCoreDss1Shift);
    }

// ... existing frequency getters ...
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
  : iProfile(&KProfileRM320),
    iOrigCount(0), iPatcherOpen(EFalse), iDvfsOpen(EFalse), iReady(EFalse), iExecMode(EFalse), iPatched(EFalse), iShadowed(EFalse),
    iApplied(EFalse), iKeep(EFalse), iRestoreNeedsReboot(EFalse),
    iFoundGlobal(0), iFoundBody(0), iFoundSram(0), iLastCoreWritten(0), iAppliedMult(0), iSram(0), iStockClkSel1(0), iStockCore(0), iStockMpu(0),
    iBootCore(0), iBootMpu(0), iBootMult(0),
    iStockDspDiv(0), iDspDiv(0), iStep(EOcStepIdle)
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
    //   2. restore original kernel bytes - patch also survives the process, and
    //      leaving a replaced function in the system means planting a landmine for later;
    //   3. only then close channels (step 2 is impossible without patcher).
    if (iApplied && !iKeep && iSram)
        {
        TUint32 st = 0;
        RestoreStock(st);
        }
    // DSP divider - by same rule as core frequency: process dies while
    // the domain remains reconfigured, and silently leaving the device like this would be
    // improper. KeepOnExit covers this too.
    if (!iKeep && iSram && iDspDiv != iStockDspDiv)
        {
        TInt rc = 0;
        RestoreDspDiv(rc);
        }
    RestoreKernelCode();
    // Last - remove shadow. After this, neither our bytes nor
    // the page allocated for them remain in the kernel.
    UnshadowKernelPage();
    if (iDvfsOpen)
        iDvfs.Close();
    if (iPatcherOpen)
        iPatcher.Close();
    }

// Restores DoControl(1) body to original state. Safe to call repeatedly.
//
// We write all 6 words: EXEC patch takes exactly that much. Previously 4 words stood here
// with the rationale "16 bytes is the tested memmove path"; that rationale was
// a misdiagnosis - the true reason for failed rollback was I-cache, not
// write length (findings §19).
TInt COcEngine::RestoreKernelCode()
    {
    if (!iPatcherOpen || !iPatched)
        return KErrNone;
    if (iOrigCount != 6)
        return KErrNotReady;
    TInt r = PokeWords(HostBody(), iOrigWords, 6);
    if (r == KErrNone)
        {
        iPatched = EFalse;
        iExecMode = EFalse;
        }
    return r;
    }

// Removes ROM-shadow from DoControl(1) body: the page is returned to stock
// mapping directly to ROM, and the RAM page allocated by patcher is freed.
// Done ONCE, on exit: we restore contents after each operation anyway,
// whereas alloc/free of kernel pages on every read is unnecessary churn.
TInt COcEngine::UnshadowKernelPage()
    {
    if (!iPatcherOpen || !iShadowed)
        return KErrNone;
    TUint32 addr = HostBody();
    TUint32 len = 16;
    TInt r = iPatcher.FreeShadow(addr, len);
    if (r == KErrNone)
        iShadowed = EFalse;
    return r;
    }

// Rollback verification. There is nothing to read restored bytes with - the read primitive IS
// those bytes. But the original function has known behavior: DoControl(1, 0)
// returns percentage of nominal (findings 3), i.e. 50 or 100. Our READ patch
// on a1=0 returns exactly 0, and EXEC patch would jump to NULL address.
// Therefore a non-zero percentage here is proof that stock code is back in the kernel.
TInt COcEngine::VerifyRestored()
    {
    if (!iDvfsOpen || iPatched)
        return KErrNotReady;
    if (iProfile->iHost == EHostUsbc)
        {
        // Percentage trick does not work here: stock DoControl(69, x) on Usbc
        // returns 0 even without patch (measured, findings §34).
        //
        // Calling stock DoControl(69) with non-zero a1 for verification is FORBIDDEN:
        // opcode 69 in the real driver is a real function, and what it will do with
        // an alien pointer we do not know. Instead we check directly: on this
        // device XIP is readable from user mode (findings §34), so we simply verify
        // the six body words against the reference. This is stronger proof than any indirect one -
        // we inspect the bytes themselves, without making any call into the kernel.
        const volatile TUint32* body = (const volatile TUint32*)HostBody();
        for (TInt i = 0; i < 6; ++i)
            if (body[i] != iOrigWords[i])
                return 0;
        return 1;
        }
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
_LIT(KStepUnsupported, "profile has no read path for this host");
_LIT(KStepFirmware,    "firmware does not match the profile");
_LIT(KStepVerifyBody,  "code at the patch address is not what we expect");
_LIT(KStepDiscover,    "scanning the ROM found no usable signature");
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
        case EOcStepUnsupported: return KStepUnsupported;
        case EOcStepFirmware:    return KStepFirmware;
        case EOcStepVerifyBody:  return KStepVerifyBody;
        case EOcStepDiscover:    return KStepDiscover;
        case EOcStepReady:       return KStepReady;
        default:                 return KStepIdle;
        }
    }

// Chain global -> object -> SRAM copy, with copy verification against original in ROM.
//
// Verification was one word, and for a single known address this was sufficient. For
// SEARCH one word is too little: 0xe92d40fe occurs often in memory, and
// an accidental candidate would pass. Therefore eight words here - this is already a function signature,
// not a coincidence.
TInt COcEngine::ChaseSram(TUint32 aGlobal, TUint32& aSram)
    {
    aSram = 0;
    TUint32 obj = (TUint32)iDvfs.ReadOrCall(aGlobal + 4);
    if (obj < KKernDataLo || obj >= KKernDataHi || (obj & 3))
        return KErrNotFound;
    TUint32 sram = (TUint32)iDvfs.ReadOrCall(obj + 16);
    if (sram < KKernDataLo || sram >= KKernDataHi || (sram & 3))
        return KErrNotFound;

    iStep = EOcStepVerifySram;
    for (TInt i = 0; i < 8; ++i)
        {
        const TUint32 copy = (TUint32)iDvfs.ReadOrCall(sram + i * 4);
        const TUint32 rom  = (TUint32)iDvfs.ReadOrCall(SramRoutine() + i * 4);
        if (copy != rom)
            return KErrCorrupt;
        }
    aSram = sram;
    return KErrNone;
    }

// Search for power-down global in a window around the hint taken from ROM.
//
// The hint is the literal pool value of getter/setter pair found in
// the image by signature. On RM-320 this pool yields 0xc80030ec, whereas the true
// global is 0xc80030c8, i.e. nearby, but NOT the same: the signature finds a neighboring
// pair. That is why offline value is not put into profile, but searched on
// the device - and only what passed SRAM verification is accepted.
TInt COcEngine::DiscoverPwrDownGlobal(TUint32& aGlobal, TUint32& aSram)
    {
    aGlobal = 0;
    aSram = 0;
    const TUint32 hint = iProfile->iPwrDownHint;
    if (!hint)
        return KErrNotSupported;
    // Hint is taken from profile, and profile can be written by a third party per
    // docs/PORTING.md. Therefore range is verified here, rather than relying on
    // the table containing a meaningful number: reading happens in kernel mode, and address
    // outside kernel data is a kernel exception, not KErrNotFound. Upper bound
    // also prevents hint + span from wrapping around and looping the search.
    if (hint < KKernDataLo || hint >= KKernDataHi || (hint & 3))
        return KErrArgument;
    const TUint32 lo = hint - KPwrDownSearchSpan;
    const TUint32 hi = hint + KPwrDownSearchSpan;
    for (TUint32 a = lo; a <= hi; a += 4)
        {
        TUint32 sram = 0;
        if (ChaseSram(a, sram) == KErrNone)
            {
            aGlobal = a;
            aSram = sram;
            return KErrNone;
            }
        }
    return KErrNotFound;
    }

// Search run ON DEMAND, when address is already known. Single purpose: verify the
// mechanism itself where correct answer is known in advance, and see whether it
// reproduces it. Without this, test profiles would rely on an unverified method -
// exactly the mistake offline global search made once already.
TInt COcEngine::TryDiscoverGlobal(TUint32& aGlobal, TUint32& aSram)
    {
    // Reading works only inside patch session: Prepare() removes patch after
    // itself, so here it must be reapplied and necessarily removed afterward.
    TInt r = ApplyReadPatch();
    if (r != KErrNone)
        return r;
    r = ReopenDvfs();
    if (r == KErrNone)
        r = DiscoverPwrDownGlobal(aGlobal, aSram);
    EndPatchSession();
    return r;
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

// Channel has to be reopened after each patch change: the driver holds
// state, while we need to guarantee that next DoControl(1) goes into fresh body.
TInt COcEngine::ReopenDvfs()
    {
    // Open Usbc host ONCE and do not disturb. The proven run (usbcboot)
    // opened channel BEFORE applying patch; close/open of an already patched
    // USB driver is unnecessary risk, because DoCreate performs actual driver initialization,
    // and blind opening of kernel channels on E90 already crashed the device (state.md,
    // 2026-09-02). For dvfs behavior remains as it was.
    if (iProfile->iHost == EHostUsbc && iDvfsOpen)
        return KErrNone;
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

// We do NOT reopen channel. Proven probe run opened it exactly
// once, and reopening here gives nothing: driver dispatcher calls
// patched body via "bl 0x80148410" directly, caching nothing. Redundant
// close/open cycles are redundant kernel driver operations on each read,
// and device before this work was stable, so there is no reason to introduce new churn.
TInt COcEngine::ApplyReadPatch()
    {
    // Patch cannot be applied without captured original: there would be nothing to roll back with.
    // Check is placed here, not just in Prepare(), so none of several patching
    // paths can bypass it.
    if (iOrigCount != 6)
        return KErrNotReady;
    TInt r = PokeWords(HostBody(), HostReadPatch(iProfile), 4);
    if (r != KErrNone)
        return r;
    iExecMode = EFalse;
    iPatched = ETrue;
    iShadowed = ETrue;
    return KErrNone;
    }

TInt COcEngine::ApplyExecPatch()
    {
    // Patch cannot be applied without captured original: there would be nothing to roll back with.
    // Check is placed here, not just in Prepare(), so none of several patching
    // paths can bypass it.
    if (iOrigCount != 6)
        return KErrNotReady;
    // Bytes are taken from assembler template, not from constant array: instruction
    // encoding is computed by assembler, not manually by me.
    const TBool r2 = (iProfile->iHost == EHostUsbc);
    const TUint8* p = (const TUint8*)((TUint32)(r2 ? &OcExecPatchR2Start
                                                  : &OcExecPatchStart) & ~1u);
    const TUint8* q = (const TUint8*)((TUint32)(r2 ? &OcExecPatchR2End
                                                  : &OcExecPatchEnd) & ~1u);
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

    TInt r = Poke(HostBody(), patch);
    if (r != KErrNone)
        return r;
    iExecMode = ETrue;
    iPatched = ETrue;
    iShadowed = ETrue;
    return KErrNone;
    }

// Removes patch RELIABLY. Overwriting bytes is NOT enough for this: no patcher
// path performs Cache::IMB (findings §14), while the patched function has already
// executed, so its instructions remain in I-cache. Observed directly:
// after restoring bytes DoControl(1,0) returned 0, meaning patch was alive.
// FreeShadowPage instead discards RAM page and returns mapping to
// real ROM - along with proper cache maintenance.
// Bytes are still rewritten first: if unshadow ever fails, page
// content must be stock.
TInt COcEngine::EndPatchSession()
    {
    RestoreKernelCode();
    return UnshadowKernelPage();
    }

// Reading several addresses in a SINGLE patch/unshadow cycle. Each such cycle is
// a window in which patched DoControl(1) body lives in the kernel, and dvfs_interface
// channel is opened not only by us (§15: mmrmserver.dll,
// fmradiotunercontrol.dll). Thus N separate ReadRegs are N windows instead of one,
// and there is no reason to pay with this for convenience.
TInt COcEngine::ReadRegs(const TUint32* aAddr, TUint32* aOut, TInt aCount)
    {
    if (!iPatcherOpen)
        return KErrNotReady;
    TInt r = ApplyReadPatch();
    if (r != KErrNone)
        return r;
    for (TInt i = 0; i < aCount; ++i)
        aOut[i] = (TUint32)iDvfs.ReadOrCall(aAddr[i]);
    EndPatchSession();
    return KErrNone;
    }

TInt COcEngine::ReadReg(TUint32 aAddr, TUint32& aOut)
    {
    return ReadRegs(&aAddr, &aOut, 1);
    }

TInt COcEngine::SelfTestLevel(TInt aLevel, TInt& aVerify, TUint32& aReadValue)
    {
    aVerify = -1;
    aReadValue = 0;

    // Same gate as in Prepare(), and for the same reason. Level 2 and above
    // APPLIES patch, while the profile here defaults to reference (RM-320) because
    // the caller might not invoke SetProfile. Without this check, "selftest 2" on
    // an alien device would write RM-320 address into alien kernel - exactly what
    // the gate was set up to prevent.
    if (aLevel >= 2 && CheckFirmware() != KErrNone)
        {
        iStep = EOcStepFirmware;
        return KErrNotSupported;
        }

    iStep = EOcStepLoadDriver;
    TInt r = User::LoadLogicalDevice(KPatcherFile);
    if (r != KErrNone && r != KErrAlreadyExists)
        return r;

    iStep = EOcStepOpenPatcher;
    // Prepare can be entered more than once: on an unrecognised phone the user
    // cycles through test profiles and presses OK on each. Opening an already
    // open channel leaks the previous handle, so close it first.
    if (iPatcherOpen)
        {
        iPatcher.Close();
        iPatcherOpen = EFalse;
        }
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

    // Capture original right here: levels 0 and 1 do not touch kernel, and requiring
    // byte verification from them would mean rejecting a harmless check.
    iStep = EOcStepVerifyBody;
    if (CaptureOriginal() != KErrNone)
        return KErrCorrupt;

    // Level 2: first touch of kernel code. Right here ROM-shadow page arises,
    // and it remains until reboot even after content restoration.
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
    // Verify AFTER unshadow, not after overwriting bytes: unshadow specifically
    // is the reliable patch removal.
    EndPatchSession();
    aVerify = VerifyRestored();
    if (aLevel < 3)
        return KErrNone;

    return Prepare();
    }

// Firmware gate.
//
// Profile addresses were derived from a SPECIFIC firmware build, not from device model.
// Machine uid match means only "same phone model", and on another build the same
// patch will land on different code - that is no longer "reboot restores stock", but a write
// into a random kernel function. While there was one device and own firmware, there was no
// difference; with foreign devices appearing, it becomes the project's primary risk.
//
// Therefore: we read version from device and require match with the one addresses were derived
// for. Failed to read - rejection too: "failed to verify" is not
// "verified". A profile with empty iFwVersion is not gated at all, and this must
// be visible in documentation, not just here.
// Definition here, not in class: reference fields are declared below this method, and
// GCCE 3.x compiler does not accept such in-class body.
TBool COcEngine::BootedNonStock() const
    {
    return iBootMult != iStockClocks.iMult
        || iBootCore != iStockCore
        || (iBootMpu & KMpuDivMask) != (iStockMpu & KMpuDivMask);
    }

// Captures stock words of DoControl body from live ROM and verifies that
// what is there is indeed what we expect. Twofold purpose.
//
// First, rollback. Previously constant KDvfsOriginal was substituted here, but in
// it words bl, ldr [pc,#imm], and b are offsets unique to the RM-320 build. On
// another build such a rollback would write alien offsets into alien body, and the driver
// would remain corrupted until reboot. Live words are free from this defect by
// design.
//
// Second, device recognition. A test profile has neither machine uid nor
// firmware gate, so bytes at the patch address are the only thing that distinguishes
// the correct device from any other Symbian phone. XIP is readable from
// user mode (findings §34), so check costs zero and is done BEFORE
// the first write to kernel.
//
// Mask repeats the search signature (tools/romscan.js): three words are exact, in the remaining
// three only opcode is checked, since offsets there are expected to differ.
TInt COcEngine::CaptureOriginal()
    {
    iOrigCount = 0;
    const TUint32 a = HostBody();
    if (a < KRomLo || a >= KRomHi || (a & 3))
        return KErrArgument;
    const volatile TUint32* body = (const volatile TUint32*)a;
    TUint32 w[6];
    for (TInt i = 0; i < 6; ++i)
        w[i] = body[i];

    TBool ok = EFalse;
    if (iProfile->iDiscover)
        {
        // Profile without addresses: build is unknown, so there is nothing to compare
        // six words with against a constant. Instead we verify the PROLOGUE - exactly the
        // signature by which address was found. It also answers the
        // main question of this function: is the body still intact, or does it already
        // contain our patch from a previous run that failed to roll back.
        ok = (w[0] == 0xe92d4ff0u) && (w[1] == 0xe24dd044u)
          && (w[2] == 0xe1a04000u) && (w[3] == 0xe1a06002u);
        }
    else if (iProfile->iHost == EHostUsbc)
        {
        ok = ETrue;
        for (TInt i = 0; i < 6; ++i)
            if (w[i] != KUsbcOriginal[i])
                ok = EFalse;
        }
    else
        {
        ok = (w[0] == 0xe92d4010u)
          && (w[1] == 0xe3a00001u)
          && ((w[2] >> 24) == 0xebu)
          && ((w[3] & 0xfffff000u) == 0xe59f1000u)
          && (w[4] == 0xe8bd4010u)
          && ((w[5] >> 24) == 0xeau);
        }

    if (ok)
        {
        for (TInt i = 0; i < 6; ++i)
            iOrigWords[i] = w[i];
        iOrigCount = 6;
        return KErrNone;
        }

    // Mismatched. For a profile with a firmware gate, this is almost certainly a trace
    // of a previous run that did not have time to roll back: there the constant is proven
    // to match this specific build, so we heal with it. For a test profile
    // there is no such anchor - it is simply an alien device, and writing into it is forbidden.
    if (iProfile->iFwVersion && iProfile->iFwVersion[0])
        {
        TInt words = 0;
        const TUint32* orig = HostOriginal(iProfile, words);
        for (TInt i = 0; i < 6; ++i)
            iOrigWords[i] = orig[i];
        iOrigCount = 6;
        return KErrNone;
        }
    return KErrCorrupt;
    }

// Discovers addresses on the device ITSELF, via live XIP scanning.
//
// Why do this at all. Image extracted from firmware for some devices is only
// a fragment of the full XIP, and file offset does NOT equal address: on E90 the same
// DoControl body is in the file at 0x80094ac4, but on the device at 0x800bf874 - a difference
// of 0x2adb0 (findings 47.1). Offline-derived constants for such devices are
// invalid in principle, and the only honest way is to search on-site.
//
// Why it is safe. XIP is read from USER mode, by normal
// dereferencing (findings 34): an unmapped page would produce an exception in our
// process, not a device reboot. Kernel is not touched at all here.
//
// Match must be EXACTLY ONE. Two matches mean the signature does not
// distinguish target function, and picking between them at random is the shortest path
// to patch an alien body.
TInt COcEngine::DiscoverRomAddresses()
    {
    // DLddUsbcChannel::DoControl prologue: four consecutive words.
    const TUint32 KUsbcProlog[4] =
        { 0xe92d4ff0u, 0xe24dd044u, 0xe1a04000u, 0xe1a06002u };

    TInt romSize = 0;
    TUint32 span = 0x00200000u;
    // HAL reports ROM size in kilobytes on some phones and in bytes on others.
    // The boundary is inclusive on purpose: 0x10000 as a byte count would be a
    // 64 KB ROM, which does not exist, while as a kilobyte count it is 64 MB.
    if (HAL::Get(HALData::EMemoryROM, romSize) == KErrNone && romSize > 0)
        span = (romSize <= 0x10000) ? (TUint32)romSize * 1024u : (TUint32)romSize;
    if (span > 0x04000000u)
        span = 0x04000000u;

    const TUint32 base = KRomLo;
    const TUint32 last = base + span - 32u;
    TUint32 body = 0, sram = 0;
    TInt bodyHits = 0, sramHits = 0;

    for (TUint32 a = base; a < last; a += 4)
        {
        const TUint32 w = *(const volatile TUint32*)a;
        if (w == KUsbcProlog[0]
            && *(const volatile TUint32*)(a + 4)  == KUsbcProlog[1]
            && *(const volatile TUint32*)(a + 8)  == KUsbcProlog[2]
            && *(const volatile TUint32*)(a + 12) == KUsbcProlog[3])
            {
            ++bodyHits;
            body = a;
            }
        // power.dll relock routine. Two of eight words are ldr with PC-relative
        // offset, and offset depends on placement, so they are matched by mask.
        if (w == KSramRoutineSignature[0]
            && (*(const volatile TUint32*)(a + 4) & 0xfffff000u) == 0xe59f1000u
            && (*(const volatile TUint32*)(a + 8) & 0xfffff000u) == 0xe59f5000u
            && *(const volatile TUint32*)(a + 12) == KSramRoutineSignature[3]
            && *(const volatile TUint32*)(a + 16) == KSramRoutineSignature[4]
            && *(const volatile TUint32*)(a + 20) == KSramRoutineSignature[5]
            && *(const volatile TUint32*)(a + 24) == KSramRoutineSignature[6]
            && *(const volatile TUint32*)(a + 28) == KSramRoutineSignature[7])
            {
            ++sramHits;
            sram = a;
            }
        }

    if (bodyHits != 1 || sramHits != 1)
        return KErrNotFound;
    iFoundBody = body;
    iFoundSram = sram;
    return KErrNone;
    }

TInt COcEngine::TryDiscoverRom(TUint32& aBody, TUint32& aSram)
    {
    aBody = 0;
    aSram = 0;
    const TInt r = DiscoverRomAddresses();
    aBody = iFoundBody;
    aSram = iFoundSram;
    // Search was called for verification, not for operation: do not keep
    // discovered values in fields, otherwise next Prepare() would take them silently.
    iFoundBody = 0;
    iFoundSram = 0;
    return r;
    }

TInt COcEngine::CheckFirmware()
    {
    iFirmware.Zero();
    if (!iProfile->iFwVersion || !iProfile->iFwVersion[0])
        return KErrNone;

    TBuf<KSysUtilVersionTextLength> v;
    if (SysUtil::GetSWVersion(v) != KErrNone)
        return KErrNotSupported;

    // Version string is multiline: version, date, model, copyright,
    // so we take only the first line for the log - the rest is not needed.
    TInt nl = v.Locate(TChar(10));
    iFirmware.Copy(v.Left(nl == KErrNotFound ? Min(v.Length(), iFirmware.MaxLength())
                                             : Min(nl, iFirmware.MaxLength())));

    // There can be several builds in profile, separated by ';'. A list, rather than a single version,
    // because addresses between adjacent builds often match - but each such
    // build is entered here ONLY after verification, not by version number similarity.
    TBuf<128> list;
    list.Copy(TPtrC8((const TUint8*)iProfile->iFwVersion));
    TInt pos = 0;
    while (pos < list.Length())
        {
        TInt sep = list.Mid(pos).Locate(';');
        const TInt end = (sep == KErrNotFound) ? list.Length() : pos + sep;
        const TPtrC one = list.Mid(pos, end - pos);
        if (one.Length() && v.Find(one) != KErrNotFound)
            return KErrNone;
        pos = end + 1;
        }
    return KErrNotSupported;
    }

TInt COcEngine::Prepare()
    {
    if (CheckFirmware() != KErrNone)
        {
        iStep = EOcStepFirmware;
        return KErrNotSupported;
        }

    // Porting guard: this entire procedure patches iDvfsBody and reads kernel through
    // this patch. On a profile without dvfs (E90: iRead != EReadDvfs, iDvfsBody == 0)
    // applying patch would go to address 0 of foreign kernel and hang the device
    // (findings §27). Therefore we reject BEFORE any touch to the kernel. Read
    // path for such devices is Usbc bootstrap, a separate procedure.
    // Zeroes in these fields mean "not yet measured". Patching at NULL address
    // or verifying SRAM against zero is a guaranteed way to crash the device, so
    // we reject BEFORE any touch to the kernel.
    // Profile without addresses discovers them itself, and does so FIRST: until addresses
    // are known, there is nothing below to check.
    if (iProfile->iDiscover && (iFoundBody == 0 || iFoundSram == 0))
        {
        iStep = EOcStepDiscover;
        if (DiscoverRomAddresses() != KErrNone)
            return KErrNotFound;
        }

    if (iProfile->iRead != EReadDvfs || HostBody() == 0
        || SramRoutine() == 0
        || (iProfile->iPwrDownGlobal == 0 && iProfile->iPwrDownHint == 0))
        {
        iStep = EOcStepUnsupported;
        return KErrNotSupported;
        }

    iStep = EOcStepVerifyBody;
    if (CaptureOriginal() != KErrNone)
        return KErrCorrupt;

    iStep = EOcStepLoadDriver;
    TInt r = User::LoadLogicalDevice(KPatcherFile);
    if (r != KErrNone && r != KErrAlreadyExists)
        return r;
    // Primitive host is chosen by profile; channel must know about it BEFORE opening.
    iDvfs.SetHost(iProfile->iHost);
    if (iProfile->iHost == EHostDvfs)
        User::LoadLogicalDevice(KDvfsFile);   // already in ROM, attempt won't hurt

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

    // Open Usbc host BEFORE patch - in the same order as proven
    // usbcboot. If channel failed to open, nothing is written to the kernel.
    if (iProfile->iHost == EHostUsbc)
        {
        iStep = EOcStepOpenDvfs;
        r = ReopenDvfs();
        if (r != KErrNone)
            return r;
        }

    // Mark patch applied BEFORE write: if PokeWords lands partially (driver
    // might have already created shadow page), EndPatchSession below will still roll back.
    iStep = EOcStepReadPatch;
    iExecMode = EFalse;
    iPatched = ETrue;
    // Profile-specific variant specifically: on Usbc host address arrives in r2, while r1
    // holds opcode. r1 stub here would dereference 69 in kernel mode - verified
    // the hard way on 2026-09-09, device crashed instantly.
    r = PokeWords(HostBody(), HostReadPatch(iProfile), 4);
    if (r == KErrNone)
        r = PrepareProbe();

    // Remove patch ALWAYS - both on success and on any probe error. This is
    // the main porting guard: on a device with different addresses ReopenDvfs or
    // SRAM verification will abort, and without this line patched foreign kernel body
    // would remain alive until reboot (findings §27).
    EndPatchSession();
    if (r != KErrNone)
        return r;

    iStep = EOcStepReady;
    iReady = ETrue;
    return KErrNone;
    }

TInt COcEngine::PrepareProbe()
    {
    iStep = EOcStepOpenDvfs;
    TInt r = ReopenDvfs();
    if (r != KErrNone)
        return r;

    // Usbc host: first prove that patch is ALIVE, and only then trust it.
    // Without this check, zero from stock DoControl(69) cannot be distinguished from
    // zero read by primitive - exactly what bootstrap was once lost on
    // (findings §34). Reset vector is known and immutable.
    if (iProfile->iHost == EHostUsbc)
        {
        // Own step: the driver did open, so reporting a failure to open it
        // would send anyone reading the log looking in the wrong place.
        iStep = EOcStepVerifyBody;
        if ((TUint32)iDvfs.ReadOrCall(0x80000000u) != 0xea00007eu)
            return KErrNotReady;
        }

    // Relock routine signature. We read THROUGH patch, so this single block proves both
    // that patch is alive and that iRomSramRoutine points to the right place. For
    // a test profile this is the primary check: we do not know build version, and
    // firmware gate there is empty.
    if (iProfile->iSramSignature)
        {
        iStep = EOcStepVerifySram;
        for (TInt i = 0; i < 8; ++i)
            {
            const TUint32 got =
                (TUint32)iDvfs.ReadOrCall(SramRoutine() + i * 4);
            // Words 1 and 2 are ldr with PC-relative offset, and offset depends on
            // function placement. For a proven profile we know exact
            // values for this specific build and require them. For a profile that
            // discovered address itself, build is unknown, so these two words go by
            // mask - remaining six remain exact.
            if (iProfile->iDiscover && (i == 1 || i == 2))
                {
                const TUint32 want = (i == 1) ? 0xe59f1000u : 0xe59f5000u;
                if ((got & 0xfffff000u) != want)
                    return KErrCorrupt;
                continue;
                }
            if (got != iProfile->iSramSignature[i])
                return KErrCorrupt;
            }
        }

    iStep = EOcStepResolveSram;
    TUint32 sram = 0;
    TUint32 global = iProfile->iPwrDownGlobal;
    if (global)
        {
        r = ChaseSram(global, sram);
        if (r != KErrNone)
            return r;
        }
    else
        {
        // Profile without global address (test). We do not guess: try a narrow
        // window around ROM hint and take the address whose chain yields
        // an SRAM copy byte-for-byte identical to original. Erroneous candidate will not
        // pass such verification, and window is deliberately small - reading arbitrary
        // kernel address is executed IN KERNEL MODE, and an unmapped page would mean
        // not an error, but a reboot.
        r = DiscoverPwrDownGlobal(global, sram);
        if (r != KErrNone)
            return r;
        iFoundGlobal = global;
        }
    iSram = sram;

    iStep = EOcStepReadRegs;
    TUint32 sel1 = (TUint32)iDvfs.ReadOrCall(KRegClkSel1Pll);
    TUint32 sel2 = (TUint32)iDvfs.ReadOrCall(KRegClkSel2Pll);
    TUint32 mpud = (TUint32)iDvfs.ReadOrCall(KRegClkSelMpu);
    TUint32 core = (TUint32)iDvfs.ReadOrCall(KRegClkSel1Core);
    if (((sel1 >> 12) & 0x3ff) == 0)
        return KErrCorrupt;
    iStockClkSel1 = sel1;
    // State in which the device was found. Needed to know where to start
    // the ramp from, and to report loudly about non-stock start - but it is
    // NOT a reference point for compensation.
    iBootCore = core;
    iBootMpu = mpud;
    iBootMult = (TInt)((sel1 >> 12) & 0x3ff);
    // Reference point is constant. Why so - see comment at KStockClkSel1Core.
    // Zero in profile means "we do not know stock layout for this device"
    // - as in test profiles. Then reference point becomes whatever was found at
    // start, which is why such a device must be freshly rebooted.
    // For proven profiles reference point remains constant (see comment at
    // KStockClkSel1Core): live registers of an overclocked device would have drifted.
    iStockCore = iProfile->iStockClkSel1Core ? iProfile->iStockClkSel1Core : core;
    iStockMpu = iProfile->iStockClkSelMpu ? iProfile->iStockClkSelMpu : mpud;
    // DSP divider. Firmware does not change it in runtime even once: both
    // stock setters (power.dll @0x800AC59C, ehwaomap.pdd @0x801AE900) across the entire
    // ROM are neither called nor exported. Therefore value retrieved here is sufficient
    // as reference - it will not "drift" on its own.
    TUint32 dsel = (TUint32)iDvfs.ReadOrCall(KRegClkSelDsp);
    iStockDspDiv = (TInt)(dsel & KDspFwDivMask);
    if (iStockDspDiv == 0)
        iStockDspDiv = 3;
    iDspDiv = iStockDspDiv;
    // core_sel reads either as 1 or as 2: DVFS governor moves it in
    // runtime (findings 12, 18). Instantaneous reading here is deceptive - catching
    // device at lower OPP would show 165 MHz instead of 330. Therefore we take
    // a series UNDER LOAD and take maximum: governor raises OPP only
    // when there is work, and interference can slow reading, but not overestimate.
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
    // Decode STOCK layout, not the one found: sys_clk and DPLL divider are taken from
    // hardware (they are immutable), multiplier and dividers - from constants.
    iStockClocks.Decode(sel1, sel2, iStockMpu, iStockCore);
    iStockClocks.iMult = iProfile->iStockMult ? iProfile->iStockMult : iBootMult;
    // But current multiplier is specifically what is in hardware: ramp must proceed from where
    // the device actually stands. iApplied remains false here: it means
    // "WE changed this", and foreign overclock must not be touched silently in the destructor.
    iAppliedMult = iBootMult;

    // Probe complete. Patch is removed by caller (Prepare) - both here and on emergency
    // exits above, via single EndPatchSession, so no path leaves it in the kernel.
    return KErrNone;
    }

void COcEngine::ClocksFor(TInt aMult, TOcClocks& aOut) const
    {
    aOut = iStockClocks;
    aOut.iMult = aMult;
    }

// Selection of dividers for new CORE. Goal is to keep L3 and DSS1 as close to their
// STOCK frequencies as possible: their drift broke the device, not core frequency itself.
// Dividers are integers, so exact compensation is not always possible; for equal
// error we prefer LOWER frequency - it is safer for both panel
// and memory (refresh is counted in L3 cycles, so slower bus means
// more frequent refresh, not less frequent).
// aAllowed is a bitmask of values accepted by the DEVICE ITSELF. It cannot
// be omitted: an invalid value in even one field means write to
// CM_CLKSEL1_CORE fails ENTIRELY, along with correct fields (§26).
static TInt PickDivider(TUint32 aNewCoreKhz, TUint32 aWantKhz, TInt aMin, TInt aMax,
                        TUint32 aAllowed)
    {
    if (!aWantKhz)
        return aMin;
    TInt best = aMin;
    TUint32 bestErr = 0xffffffffu;
    for (TInt d = aMin; d <= aMax; ++d)
        {
        if (d < 32 && !(aAllowed & (1u << d)))
            continue;
        TUint32 got = aNewCoreKhz / (TUint32)d;
        TUint32 err = (got > aWantKhz) ? (got - aWantKhz) : (aWantKhz - got);
        // Strictly less: for equal error LARGER divider remains, i.e.
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

    // DSS1 - exact compensation to nearest. This is the frequency whose drift broke
    // rendering, and we do not need it higher: display does not care about speed,
    // it needs ITS OWN frequency.
    const TInt dss = PickDivider(newCore, iStockClocks.Dss1Khz(), 1, 31,
                                 KAutoDivAllowed);

    // L3/SDRAM - take HIGHEST bus that does not exceed measured ceiling.
    //
    // FIXED 2026-08-15 - there was an erroneous conclusion here, tilting the decision in
    // wrong direction. It stated that slower bus was safer because "refresh is counted in
    // L3 cycles, so slower bus means more frequent refresh". In fact
    // the opposite is true. `SDRC_RFR_CTRL0 = 0x0004DA01`, meaning ARCV = 1242 L3 cycles: at
    // 165.12 MHz this is 7.5 us, exactly the auto-refresh window for 8192 rows over 64 ms.
    // Slower bus STRETCHES this window in real time, so refresh becomes
    // rarer, not more frequent, and memory loses charge. Firmware compensates its lower OPP
    // itself, knowing nothing of our divider. Therefore dropping bus below
    // stock is a risk, not a guard.
    //
    // The 213120 kHz ceiling is not arbitrary: this is the bus frequency at which the device
    // actually operates daily at mult 111. Compensation was never applied
    // there (§26), meaning the bus ran together with CORE the entire time, and in every
    // log SDRAM integrity check yielded zero errors.
    const TUint32 kL3Max = 213120u;
    TInt l3 = iStockClocks.iL3Div ? iStockClocks.iL3Div : 4;
    for (TInt d = 1; d <= 31; ++d)
        {
        if (!(KAutoL3Allowed & (1u << d)))
            continue;
        if (newCore / (TUint32)d <= kL3Max)
            {
            l3 = d;
            break;
            }
        }

    // And one more boundary, relative instead of absolute. The 213120 ceiling was measured on
    // RM-320, where stock bus is 165.12 MHz; on another device it means
    // nothing. E90 at the same core frequency has stock bus 110.08 MHz
    // (l3div=6, findings §35.1), so this same ceiling would allow it l3div=4,
    // i.e. 205 MHz - almost twice above stock, without any measurement.
    //
    // Therefore L3 divider is NEVER lowered below stock for THIS device.
    // Then the bus rises in exactly the same proportion as core - a mode
    // proven on RM-320 by daily operation (§26) - while returning does not touch
    // the divider at all, meaning we avoid the exact operation that on RM-320
    // crashed the device three times (§26.12, §26.14, §26.15).
    // For RM-320 this changes nothing: there the ceiling yields stock 4 anyway.
    if (iStockClocks.iL3Div && l3 < iStockClocks.iL3Div)
        l3 = iStockClocks.iL3Div;

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

// Read-back of LIVE CM_CLKSEL1_CORE. Missing for a long time, which is why
// an erroneous conclusion lived for a month: hardware rejects ENTIRE write to this register
// if even one field is invalid (§26), yet no log showed that - we reported
// INTENT rather than result. Now every run reports whether write took effect.
TInt COcEngine::LiveCore(TUint32& aOut)
    {
    return ReadReg(KRegClkSel1Core, aOut);
    }

TInt COcEngine::WriteRelockBlob(TUint32 aClkSel1Value, TUint32 aCoreValue,
                                TUint32 aMpuValue)
    {
    const TUint8* p = (const TUint8*)((TUint32)&OcRelockBlobStart & ~1u);
    const TUint8* q = (const TUint8*)((TUint32)&OcRelockBlobEnd & ~1u);
    TInt len = (TInt)(q - p);
    if (len <= 0 || len > 480)
        return KErrCorrupt;
    TBuf8<512> blob;
    blob.Copy(p, len);

    // Two placeholder words: target value and initial (for hardware rollback
    // inside critical section). Both must be found, otherwise blob is not what
    // we expect, and executing it is forbidden.
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
        else if (w == 0xf00dbeefu)
            v = aMpuValue;
        else if (w == 0xfeedc0deu)
            v = iStockMpu;
        else
            continue;
        blob[i+0] = (TUint8)(v);
        blob[i+1] = (TUint8)(v >> 8);
        blob[i+2] = (TUint8)(v >> 16);
        blob[i+3] = (TUint8)(v >> 24);
        ++patched;
        }
    if (patched != 6)
        return KErrNotFound;
    return Poke(iSram + KSramSlot, blob);
    }

// Places trampoline to DSP setter into the same SRAM slot where EXEC patch
// jumps. Slot is single, so relock blob and this blob simply overwrite
// each other - both are written immediately before their single invocation.
TInt COcEngine::WriteDspBlob(TInt aDiv)
    {
    const TUint8* p = (const TUint8*)((TUint32)&OcDspBlobStart & ~1u);
    const TUint8* q = (const TUint8*)((TUint32)&OcDspBlobEnd & ~1u);
    TInt len = (TInt)(q - p);
    if (len <= 0 || len > 160)
        return KErrCorrupt;
    TBuf8<160> blob;
    blob.Copy(p, len);

    TInt patched = 0;
    for (TInt i = 0; i + 4 <= len; i += 4)
        {
        TUint32 w = (TUint32)blob[i] | ((TUint32)blob[i+1] << 8) |
                    ((TUint32)blob[i+2] << 16) | ((TUint32)blob[i+3] << 24);
        TUint32 v = 0;
        if (w == 0xd5d1d1d1u)
            v = (TUint32)aDiv;
        else if (w == 0xd5a0a0a0u)
            v = iProfile->iDspSetter;
        else
            continue;
        blob[i+0] = (TUint8)(v);
        blob[i+1] = (TUint8)(v >> 8);
        blob[i+2] = (TUint8)(v >> 16);
        blob[i+3] = (TUint8)(v >> 24);
        ++patched;
        }
    // Both placeholders must be found. If assembler collapsed either into
    // an immediate operand, the blob is not what we expect, and executing
    // it in kernel mode is forbidden.
    if (patched != 2)
        return KErrNotFound;
    return Poke(iSram + KSramSlot, blob);
    }

// Same SRAM slot, same EXEC patch - but a blob that only rewrites
// dividers and commits them with latch, without stopping DPLL. Rationale - at
// OcDivBlobStart.
TInt COcEngine::WriteDivBlob(TUint32 aCoreReg, TUint32 aMpuReg)
    {
    const TUint8* p = (const TUint8*)((TUint32)&OcDivBlobStart & ~1u);
    const TUint8* q = (const TUint8*)((TUint32)&OcDivBlobEnd & ~1u);
    TInt len = (TInt)(q - p);
    if (len <= 0 || len > 160)
        return KErrCorrupt;
    TBuf8<160> blob;
    blob.Copy(p, len);

    TInt patched = 0;
    for (TInt i = 0; i + 4 <= len; i += 4)
        {
        TUint32 w = (TUint32)blob[i] | ((TUint32)blob[i+1] << 8) |
                    ((TUint32)blob[i+2] << 16) | ((TUint32)blob[i+3] << 24);
        TUint32 v = 0;
        if (w == 0xd1c0de01u)
            v = aCoreReg;
        else if (w == 0xd1c0de02u)
            v = aMpuReg;
        else
            continue;
        blob[i+0] = (TUint8)(v);
        blob[i+1] = (TUint8)(v >> 8);
        blob[i+2] = (TUint8)(v >> 16);
        blob[i+3] = (TUint8)(v >> 24);
        ++patched;
        }
    if (patched != 2)
        return KErrNotFound;
    return Poke(iSram + KSramSlot, blob);
    }

// Rewrite dividers over whatever is currently set. aRc is the result of the blob
// itself: 0 = latch cleared, 1 = did not clear within allocated cycles.
//
// Multiplier is NOT touched, so this can be called even when a measured
// process with a live display runs in the system - unlike ApplyMultAndCore.
TInt COcEngine::ReassertDividers(TUint32 aCoreReg, TUint32 aMpuReg, TInt& aRc)
    {
    aRc = KErrGeneral;
    if (!iReady || !iSram)
        return KErrNotReady;
    TInt r = WriteDivBlob(aCoreReg, aMpuReg);
    if (r != KErrNone)
        return r;
    r = ApplyExecPatch();
    if (r != KErrNone)
        return r;
    aRc = iDvfs.ReadOrCall(iSram + KSramSlot);
    EndPatchSession();
    return KErrNone;
    }

// Executes arbitrary ARM code blob in kernel mode via SRAM slot.
// aBlob is ready code bytes, up to 2048 bytes. aResult is what the blob
// returned in r0.
TInt COcEngine::ExecCustomBlob(const TDesC8& aBlob, TInt& aResult)
    {
    aResult = 0;
    if (!iReady || !iSram)
        return KErrNotReady;
    if (aBlob.Length() <= 0 || aBlob.Length() > 2048)
        return KErrArgument;

    TInt r = Poke(iSram + KSramSlot, aBlob);
    if (r != KErrNone)
        return r;
    r = ApplyExecPatch();
    if (r != KErrNone)
        return r;
    aResult = iDvfs.ReadOrCall(iSram + KSramSlot);
    EndPatchSession();
    return KErrNone;
    }

// Changes DSP domain divider. aSetterRc is what stock setter returned:
// 0 = written, -6 (KErrArgument) = firmware rejected the value.
//
// Unlike DPLL relock, neither bypass nor IDLEST polling nor
// SDRAM self-refresh is needed here: this is a divider of a separate domain, not PLL. CORE, SDRAM, display,
// and USB are not affected.
//
// CORRECTION of previous conclusion. Previously it was stated here that PRCM_CLKCFG_CTRL
// latch is not needed, because both stock setters perform a standard
// read-modify-write without a commit. This proved false and was shown on the device:
// the setter returned 0, while the register remained with its old value. The latch is mandatory, and
// our blob performs it; this also explains why both stock setters are never called
// anywhere in ROM - in this form they do not work. Interrupts for the duration of
// "write + latch" are also disabled, because the governor uses the same latch.
TInt COcEngine::ApplyDspDiv(TInt aDiv, TInt& aSetterRc)
    {
    aSetterRc = KErrGeneral;
    if (!iReady)
        return KErrNotReady;
    // Profile without DSP setter (E90: address not measured, hence zero) - rejection BEFORE
    // any action. Without this check, the blob would execute blx 0 in kernel mode:
    // zero here is not "do nothing", but a jump to NULL address. This is exactly
    // the class of error guarded against by the rule "nothing that can hang
    // by default" - yet the DSPDIV item in GUI is always available.
    if (iProfile->iDspSetter == 0)
        return KErrNotSupported;
    // We check on our side too. The setter accepts values >31, and Modify32
    // masks them with 0x1F - meaning 32 would write 0 into the field and stop the DSP clock.
    if (aDiv < 1 || aDiv > 31)
        return KErrArgument;
    if (!(KDspDivAllowed & (1u << aDiv)))
        return KErrArgument;

    TInt r = WriteDspBlob(aDiv);
    if (r != KErrNone)
        return r;
    r = ApplyExecPatch();
    if (r != KErrNone)
        return r;
    aSetterRc = iDvfs.ReadOrCall(iSram + KSramSlot);
    EndPatchSession();
    if (aSetterRc == KErrNone)
        iDspDiv = aDiv;
    return KErrNone;
    }

TInt COcEngine::RestoreDspDiv(TInt& aSetterRc)
    {
    aSetterRc = KErrNone;
    if (iDspDiv == iStockDspDiv)
        return KErrNone;
    return ApplyDspDiv(iStockDspDiv, aSetterRc);
    }

// Rereads CM_CLKSEL_DSP from device. We do not trust our remembered
// value - same rule as for LiveMult.
TInt COcEngine::LiveDspDiv(TInt& aOut)
    {
    TUint32 v = 0;
    TInt r = ReadReg(KRegClkSelDsp, v);
    if (r != KErrNone)
        return r;
    aOut = (TInt)(v & KDspFwDivMask);
    return KErrNone;
    }

// DSP frequency as calculated by firmware itself: CORE / CM_CLKSEL_DSP[3:0]
// (power.dll ord60 @0x800B1620). This exact number is written by ehwaomap.pdd driver into
// _BRIDGEINIT_DSP_FREQ symbol of DSP base image.
TUint32 COcEngine::DspKhz(TInt aDiv) const
    {
    if (aDiv <= 0)
        return 0;
    return iStockClocks.CoreKhz() / (TUint32)aDiv;
    }

TInt COcEngine::ApplyMult(TInt aMult, TUint32& aStatus)
    {
    return ApplyMultAndCore(aMult, CompensatedCore(aMult), 0, aStatus);
    }

TInt COcEngine::ApplyMultAndCore(TInt aMult, TUint32 aCoreReg, TUint32 aMpuReg,
                                 TUint32& aStatus)
    {
    aStatus = 0;
    if (!iReady)
        return KErrNotReady;
    if (aMult < KOcMultMin || aMult > KOcMultMax)
        return KErrArgument;

    // Bus divider goes into hardware as specified by user.
    //
    // Until now a guard stood here that clamped l3 field with live value and prevented
    // raising the bus. It was removed deliberately: tuning overclock still has to be done
    // on a specific device, and an unrestricted multiplier is just as capable
    // of rebooting the phone. Restricting one of two equally dangerous quantities while
    // leaving the other free makes no sense.
    //
    // Protection remained where it is not about freedom of choice: RestoreStock does not
    // restore an already lowered bus back to stock, because that exact operation killed the device
    // four times out of four (findings §38.2). Reboot restores stock clocking
    // in any case, so this refusal takes nothing away from user.

    // Modify exactly bits [21:12], preserve the rest of the register.
    TUint32 target = (iStockClkSel1 & ~0x003ff000u) |
                     (((TUint32)aMult & 0x3ff) << 12);

    iLastCoreWritten = aCoreReg;
    TInt r = WriteRelockBlob(target, aCoreReg, aMpuReg ? aMpuReg : iStockMpu);
    if (r != KErrNone)
        return r;
    r = ApplyExecPatch();
    if (r != KErrNone)
        return r;

    TUint32 rc = (TUint32)iDvfs.ReadOrCall(iSram + KSramSlot);
    aStatus = rc >> 28;

    // Remove patch IMMEDIATELY and completely: measurements follow, and keeping replaced
    // kernel function body the entire time is unacceptable.
    EndPatchSession();

    iAppliedMult = aMult;
    // "We left device non-stock" is not only about multiplier. MPU divider at
    // stock multiplier changes core frequency by half, and failing to notice this in
    // destructor would mean leaving the device twice slower.
    iApplied = (aMult != iStockClocks.iMult) ||
               (aCoreReg != 0 && aCoreReg != iStockCore) ||
               (aMpuReg != 0 && aMpuReg != iStockMpu);
    return KErrNone;
    }

TInt COcEngine::ApplyMultRamp(TInt aTarget, TUint32& aStatus)
    {
    return ApplyRampWithCore(aTarget, 0, 0, aStatus);
    }

// aCoreReg == 0 means automatic selection at each step; otherwise the same
// manually specified dividers stand on all steps.
TInt COcEngine::ApplyRampWithCore(TInt aTarget, TUint32 aCoreReg,
                                  TUint32 aMpuReg, TUint32& aStatus)
    {
    aStatus = 0;
    if (!iReady)
        return KErrNotReady;
    if (aTarget < KOcMultMin || aTarget > KOcMultMax)
        return KErrArgument;

    // We step from ACTUAL multiplier, not remembered one: if previous
    // attempt rolled back on its own, hardware state might not match ours.
    TInt cur = 0;
    TInt r = LiveMult(cur);
    if (r != KErrNone)
        return r;
    if (cur < KOcMultMin || cur > KOcMultMax)
        return KErrCorrupt;
    const TInt from = cur;

    TBool coreChecked = EFalse;
    while (cur != aTarget)
        {
        cur += (aTarget > cur) ? 1 : -1;
        TUint32 st = 0;
        r = aCoreReg ? ApplyMultAndCore(cur, aCoreReg, aMpuReg, st)
                     : ApplyMult(cur, st);
        aStatus |= st;
        if (r != KErrNone)
            return r;
        // Non-zero status means blob already rolled back to previous
        // value itself. We go no further: next step would only repeat the failure.
        if (st != 0)
            return KErrAbort;
        // The dividers are the same on every step, so the first step already
        // says whether the hardware takes them. Climbing on after a refusal
        // would only carry the old dividers further from the clock they were
        // set for.
        if (aCoreReg && !coreChecked)
            {
            coreChecked = ETrue;
            TUint32 live = 0;
            r = CheckCoreTook(aCoreReg, live);
            if (r == KErrOcCoreRejected)
                return UndoRejectedRamp(cur, from, live, aMpuReg, aStatus);
            if (r != KErrNone)
                return r;
            }
        User::After(20000);            // 20 ms for subsystems to settle
        }

    // Explicit final write of layout when dividers are specified.
    //
    // THIS IS A BUG FIX (findings §38.1). The loop above spins only while
    // multiplier does not equal target. But when user changes ONLY dividers without
    // touching multiplier - and that is the primary way to use them - the loop
    // executes zero times, and nothing goes into hardware. Externally this looked
    // exactly like "dividers don't work": neither bus nor display reacted because
    // there was no write. The probe did not show this because `patcher clocks` always
    // performs such final write separately.
    //
    // The write is idempotent: if ramp took place, the last step already set those
    // exact values.
    if (aCoreReg)
        {
        TUint32 st = 0;
        r = ApplyMultAndCore(aTarget, aCoreReg, aMpuReg, st);
        aStatus |= st;
        if (r != KErrNone)
            return r;
        if (st != 0)
            return KErrAbort;
        TUint32 live = 0;
        r = CheckCoreTook(aCoreReg, live);
        if (r == KErrOcCoreRejected)
            return UndoRejectedRamp(aTarget, from, live, aMpuReg, aStatus);
        if (r != KErrNone)
            return r;
        }
    return KErrNone;
    }

// KErrNone when CM_CLKSEL1_CORE holds exactly what was requested,
// KErrOcCoreRejected when it holds something else.
//
// The hardware refuses the whole register when one field is out of its own
// range, which is narrower than what the firmware validates (findings 26), and
// the refusal is silent. The PLL is a different register and is written by the
// same blob, so a refused layout leaves a new multiplier running on the old
// dividers. On the E90 that meant m105 with the stock display divider: the
// panel clocked 22% above its own frequency, and the phone rebooted shortly
// after (2026-10-12).
TInt COcEngine::CheckCoreTook(TUint32 aCoreReg, TUint32& aLive)
    {
    aLive = 0;
    const TInt r = LiveCore(aLive);
    if (r != KErrNone)                  // a failed read is not a refusal
        return (r == KErrOcCoreRejected) ? KErrGeneral : r;
    return (aLive == aCoreReg) ? KErrNone : KErrOcCoreRejected;
    }

// Walks the multiplier back from aFrom to aBackTo, one step at a time, keeping
// the dividers the hardware actually holds. Those are rewritten unchanged, so
// the bus divider never moves here, and the one transition known to kill the
// phone - a lowered bus returned to stock (findings 38.2) - cannot happen.
//
// aLiveCore is the CM_CLKSEL1_CORE the refusal left behind, already read by
// CheckCoreTook. The MPU divider is read here; if that read fails the walk
// still goes ahead with the MPU value the last step wrote, because stopping
// would leave the phone in exactly the state this function exists to undo.
//
// The engine state is then set from what the hardware holds, not from the
// last write. Otherwise a refused divider-only change (no multiplier step to
// undo) would leave iApplied set, boot would take the refusal for a held
// overclock, and the GUI would compare the readback with the rollback write
// instead of with the request.
TInt COcEngine::UndoRejectedRamp(TInt aFrom, TInt aBackTo, TUint32 aLiveCore,
                                 TUint32 aWrittenMpu, TUint32& aStatus)
    {
    const TUint32 wanted = iLastCoreWritten;
    TUint32 mpu = 0;
    if (ReadReg(KRegClkSelMpu, mpu) != KErrNone)
        mpu = aWrittenMpu ? aWrittenMpu : iStockMpu;

    TInt r = KErrNone;
    TInt cur = aFrom;
    while (cur != aBackTo)
        {
        cur += (aBackTo > cur) ? 1 : -1;
        TUint32 st = 0;
        r = ApplyMultAndCore(cur, aLiveCore, mpu, st);
        aStatus |= st;
        if (r != KErrNone)
            break;
        if (st != 0)
            {
            r = KErrAbort;
            break;
            }
        User::After(20000);
        }

    iLastCoreWritten = wanted;
    TInt liveMult = cur;
    LiveMult(liveMult);
    iAppliedMult = liveMult;
    iApplied = (liveMult != iStockClocks.iMult) ||
               (aLiveCore != iStockCore) ||
               (mpu != iStockMpu);
    return (r != KErrNone) ? r : KErrOcCoreRejected;
    }

// Restoring stock state - in two steps, and second is mandatory.
//
// The ramp brings multiplier to stock, compensating dividers at each step, and
// this is sufficient while multiplier itself was non-stock. But if multiplier is already stock, while
// DIVIDER remained non-stock (layout like "mult 86, mpu_div 4"), the ramp performs
// no write at all: its while loop exits immediately. Therefore it is followed by
// one explicit write of full stock layout.
TInt COcEngine::RestoreStock(TUint32& aStatus)
    {
    aStatus = 0;
    TInt r = ApplyMultRamp(iStockClocks.iMult, aStatus);
    if (r != KErrNone)
        return r;

    // The second step is done only when dividers are indeed non-stock, and
    // we determine this by READING, not from our own tracking. Reason: this write is
    // a full relock, i.e. bypass window, and with a live display such a window hung
    // the device (§20). Doing it "just in case" is forbidden.
    const TUint32 regs[2] = { KRegClkSel1Core, KRegClkSelMpu };
    TUint32 v[2] = { 0, 0 };
    if (ReadRegs(regs, v, 2) == KErrNone &&
        (v[0] & (KCoreL3Mask | KCoreDss1Mask)) ==
            (iStockCore & (KCoreL3Mask | KCoreDss1Mask)) &&
        (v[1] & KMpuDivMask) == (iStockMpu & KMpuDivMask))
        {
        iApplied = EFalse;
        return KErrNone;
        }

    // WE NO LONGER ATTEMPT TO RAISE BUS BACK (findings §38.2).
    //
    // Score of operation "restore l3div to a smaller value" - four deaths out of
    // four on two different devices: three times on RM-320 (§26.12, §26.14, §26.15)
    // and once on E90 today. Both paths, full relock and lightweight
    // ReassertDividers, die identically. Today's run also disproved
    // the explanation in §26.15: on E90 there is no governor, sel2 is 2 the whole time, bus was
    // exactly at 82.56 MHz, and raise was only by 1.33x - and still death.
    //
    // But LOWERING the bus was measured as clean: layout holds, zero
    // corrupted words in SDRAM during observation window. Meaning asymmetry is in the operation
    // itself, not in its magnitude.
    //
    // Thus leaving lowered bus is SAFE, while raising it is not. Reboot restores device
    // to stock state guaranteed (nothing is written to flash), so correct
    // answer here is not to make fatal write and report this, rather than
    // "restore state" at the cost of reboot caused by ourselves.
    // We refuse specifically to RAISE bus: if live divider is larger than
    // stock, returning to stock is that exact fatal write. The opposite
    // case (bus higher than stock) we never create because CompensatedCore
    // clamps l3 from below to stock value, but narrowed condition is more precise than "any
    // difference" and will not block safe lowering.
    if ((v[0] & KCoreL3Mask) > (iStockCore & KCoreL3Mask))
        {
        iRestoreNeedsReboot = ETrue;
        iApplied = EFalse;   // we will do nothing more with this state
        return KErrNone;
        }

    // Remaining dividers (DSS1, MPU) are restored via LIGHTWEIGHT path: write plus latch,
    // without DPLL bypass. These changes do not touch the bus, and have no deaths behind them.
    //
    // Multiplier here is already stock - ramp above took care of that - so full relock
    // adds nothing except its bypass window. And that window, it seems, killed
    // the device when restoring from `l3div=8`: in bypass CORE drops to sys_clk, and bus
    // at divider 8 gets 2.4 MHz instead of 4.8 at stock divider 4. The 4 -> 8
    // change completed cleanly twice, reverse 8 -> 4 killed twice, dying right
    // here (§26.14). Asymmetry points to state BEFORE the window, not target
    // values.
    TInt rc = -1;
    r = ReassertDividers(iStockCore, iStockMpu, rc);
    if (r == KErrNone && rc == 0)
        {
        // Read back: lightweight path does not deserve trust without verification either.
        TUint32 w[2] = { 0, 0 };
        if (ReadRegs(regs, w, 2) == KErrNone &&
            (w[0] & (KCoreL3Mask | KCoreDss1Mask)) ==
                (iStockCore & (KCoreL3Mask | KCoreDss1Mask)) &&
            (w[1] & KMpuDivMask) == (iStockMpu & KMpuDivMask))
            {
            iApplied = EFalse;
            return KErrNone;
            }
        }

    // Failed - full relock as fallback. It is worse, but leaving device
    // with non-stock dividers is worse than it.
    TUint32 st = 0;
    r = ApplyMultAndCore(iStockClocks.iMult, iStockCore, iStockMpu, st);
    aStatus |= st;
    if (r == KErrNone)
        iApplied = EFalse;
    return r;
    }
