# How to Add a Device

This file describes exactly what is required for the application to run on another
TI OMAP 2420 phone, and why none of these values can be guessed.

## What a Profile Is

A profile is the `TOcProfile` struct in `src/ocengine.h`. It contains nine fields, and
they fall into three groups distinct in nature.

**Device identification**: `iMachineUid` (HAL EMachineUid) and `iFwVersion` (semicolon-separated
list of firmware builds for which addresses were derived). The first answers
the question "which phone is this", the second — "which exact build". Both are required, because
addresses depend on the build, not just the model.

**Kernel addresses**: `iDvfsBody` (body of `DoControl` of the READ/EXEC primitive carrier),
`iRomSramRoutine` (idle/relock routine in `power.dll`), `iDspSetter` (stock setter
for the DSP divider), and `iPwrDownGlobal` (kernel data global through which the
SRAM address is resolved). These four determine where the patch lands.

**Stock clock layout**: `iStockClkSel1Core`, `iStockClkSelMpu`,
`iStockMult`. This is the reference point for divider compensation, and every device
has its own because the display panel and memory differ. The E90 has `l3div=6` where the RM-320 has 4.

## Step 1: Read What the Device Reports Itself

Run the application on the device. It does not touch an unknown device at all, but
prints exactly the two lines with which a profile begins:

```
UNKNOWN DEVICE (uid 200005d6) - refusing
firmware: V 400.34.93
```

This is sufficient to populate `iMachineUid` and `iFwVersion`. Both values
are taken from the device, not from reference tables: machine UID numbers on the net are often
confused between revisions.

## Step 2: Kernel Addresses

The method is described in `findings.md` §31 (derivation from XIP image) and §34 (why
this method failed there). In brief: in uncompressed XIP, look for characteristic
instruction sequences — prologue `stmdb sp!, {r1-r7, lr}` together with a CP15 barrier and a write to
SDRC at offset `+0x70` for the relock routine; validator `cmp r1, #9` / `cmp r1, #16`
for the DSP setter; getter/setter pair `str r0, [r1, #4]` / `ldr r0, [r0, #4]` for
the power-down global.

Tool:

```
objdump -D -b binary -m arm --adjust-vma=0x80000000 out/roms/<device>.rom
```

**The primary caveat, and it cost the project a month.** The offline method
works only where XIP in the firmware is uncompressed and mapped linearly. On the E90
this assumption did not hold: the unpacked image was 1.7 MB versus 19.7 MB
of real ROM, most of the code is paged, and all four derived addresses
turned out to be wrong (§34). The correct ones were retrieved by scanning memory on the live hardware
via `ocprobe romscan` / `romdump`.

Therefore, the rule is: an address derived offline is a candidate, not a fact. Until it is
confirmed on hardware, a profile containing it is not added.

## Step 3: Stock Layout

Collected on a device that has just booted and has not overclocked anything:

```
tools\build.cmd run <ip> "patcher engine"
```

The log will show `CM_CLKSEL1_CORE`, `CM_CLKSEL_MPU`, and `CM_CLKSEL1_PLL`. These exact
values go into the profile. Taking them from live registers at runtime is not allowed: if
the phone is already overclocked, the reference point shifts, and divider compensation starts
aiming at a foreign frequency — this has already been hit before (see comment above `KStockClkSel1Core`).

## What Has Been Collected, but NOT Confirmed

In `out/analysis/` are offline analyses of ROM images for Nokia N82 (RM-313),
N95 classic (RM-159), N93 (RM-55), and N93i (RM-156). For N82 and RM-159, two of
the four addresses have been found (`iRomSramRoutine`, `iDspSetter`), with RM-159 matching
RM-320 byte-for-byte, and N82 shifted by exactly `+0x10`.

None of these addresses have been verified on hardware, we do not know the machine UIDs for these
devices, and the stock clock layout has not been recorded. Attempts to deduce UIDs offline yielded numbers
that appeared to be adjacent to known ones (for example `0x20002D85` alongside RM-320
`0x20002D84`) — that is, fabricated by pattern, without a trace of evidence. Therefore, these
profiles are never selected automatically: they remain in the application as test profiles, with
an empty machine UID, and can only be enabled manually via the TEST PROFILE row
on a device the application did not recognize. They will become stock profiles when
someone runs this on such a phone and submits the two lines from Step 1.

## N93 and N93i: A Profile That Finds Addresses Itself

For these two devices, providing offline addresses is impossible, and this is a measured fact,
not caution. The XIP extracted from firmware for them, as on the E90, is a fragment: 1.1 and
1.3 MB versus almost 20 MB on a live device, so the offset in the file does not equal
the address. For the E90, the difference was measured directly — the very same `DoControl` body lies in
the image at offset `0x94ac4`, but on the device at `0x800bf874` (findings §47.1).
A constant derived from such an image would point into a foreign page.

There is no version of the `dvfs` driver in the N93 and N93i firmware, so the primitive
carrier is `Usbc`, as on the E90.

Therefore, profiles `RM-55 N93 (TEST, auto)` and `RM-156 N93i (TEST, auto)` have all
addresses empty and the `iDiscover` flag set. Addresses are searched by the device itself, by scanning
live XIP from user mode, using two signatures: prologue of
`DLddUsbcChannel::DoControl` (four words) and the relock routine in `power.dll` (eight
words, two of which are masked because they are `ldr` with a PC-relative offset). There must be
exactly one match; two matches mean the signature does not distinguish the required
function, and the engine stops.

The mechanism was verified on two devices with independently known answers
(`ocprobe romfind`): on the E90 it yielded `0x800bf874` and `0x80058da4`, on the N95 —
`0x8010bd04` and `0x800a7f6c`. Both pairs match what is already present in
the proven profiles.

### What an N93 or N93i Owner Should Do

```
ocprobe romfind        whether both signatures are found, and at what addresses
ocprobe romscan        full scan if romfind found nothing
```

Both commands are read-only. If `romfind` returns `0` and two non-zero addresses,
the profile has a chance of working: select it via the TEST PROFILE row on a device
that was freshly rebooted (stock layout is read from live registers, so
the phone must be at its factory frequency).

If `romfind` returns `-1`, one of the signatures is missing on this device, and
this outcome is the most valuable: it means the USB driver or `power.dll` in
this build is different, and the signature must be derived from a dump. In that case,
`ocprobe romscan` is needed, followed by `ocprobe romdump <address> <length>` and a disassembler.

The DSP divider setter in these profiles is zero, meaning `ApplyDspDiv` on them
refuses. This is intentional: zero here is a branch address, not a "skip" flag.
