# Summary: Clocking Addresses Across All OMAP2420 Devices

All values were derived from the unpacked XIP code (`out/roms/*.rom`, base
`0x80000000`) and **independently verified with a byte test** (reader function
prologue `e92d4010`, pool `0xc6008540`, SRAM routine `e92d40fe`/`e92d407e`, DSP setter
`e3510009`). `agy` subagents analyzed N82/N93/N93i/N159 in parallel, while E90 and
the RM-320 reference were analyzed by the orchestrator.

| device | reader function | pool `0xc6008540` | SRAM relock (`iRomSramRoutine`) | DSP setter (`iDspSetter`) | reader veneer |
|---|---|---|---|---|---|
| RM-320 | 0x800b1544 | 0x800b16ec | 0x800a7f6c | 0x800ac59c | 0x800b1c60 |
| RM-159 | 0x800b1544 | 0x800b16ec | 0x800a7f6c | 0x800ac59c | 0x800b1c60 |
| N82    | 0x800b1554 | 0x800b16fc | 0x800a7f7c | 0x800ac5ac | 0x800b1c70 |
| N93    | 0x800451e4 | 0x80045314 | 0x8003e6d0 | 0x80040c80 | 0x8004577c |
| N93i   | 0x80046248 | 0x80046408 | 0x8003eb3c | 0x80041630 | 0x800468e8 |
| E90    | (in paged part) | 0x800b76dc | 0x80056874 | 0x80059aa4 | — |

Particulars:

- **RM-159 = RM-320 byte-for-byte** across all addresses — the same N95 codebase.
- **N93** has an SRAM routine prologue of `{r1-r6,lr}` (`e92d407e`) instead of `{r1-r7,lr}`:
  r7 is not used in this implementation, the rest of the relock logic matches.
- **E90** — uncompressed XIP contains the SRAM routine and DSP setter (both verified against
  RM-320: setter with `0x1f` mask), but the multiplier reader function itself resides in the
  paged (compressed) part; for E90, the reader accesses the register not via this
  function.

## What Is Still Missing for a Complete Profile

The engine profile has four firmware-dependent fields. Two of them (`iRomSramRoutine`,
`iDspSetter`) are confirmed above for all. There remain:

- **`iDvfsBody`** — body of `dvfs_interface DoControl(1)`, READ primitive. This is a driver
  from paged ROFS, not from resident XIP. For N82/RM-159 (full CORE
  43 MB), it can be found in the image; for N93/N93i/E90 (resident XIP only) —
  no, because ROFS is compressed. E90 does not have dvfs at all → `iDvfsBody = 0`.
- **`iPwrDownGlobal`** — kernel data global. For **E90 found: `0xc8004194`**
  (getter/setter `str/ldr r0,[global,#4]` in power zone, `findings.md` §31,
  `e90.md`). RM-320: `0xc80030c8`. For the remaining devices, not yet derived.

## E90 Status (Target Device for Flashing)

Three of four profile fields were derived offline: `iRomSramRoutine=0x80056874`,
`iDspSetter=0x80059aa4`, `iPwrDownGlobal=0xc8004194`; `iDvfsBody=0`. What remains is the
READ primitive for SRAM resolving. The WRITE primitive is RomPatcher+
(`RomPatcherPlus.Driver`), a third-party homebrew driver, cannot be rebuilt; it cannot
read arbitrary addresses. Bootstrap (user decision): use its write capability to
patch a user-accessible E90 KERNEL function as a reader (like dvfs DoControl
on RM-320) and read the chain `0xc8004194+4 -> obj -> obj+16`. Which exact function — is
the last thing to clarify; most E90 drivers are in the compressed part, so this is
an on-device reconnaissance step.
