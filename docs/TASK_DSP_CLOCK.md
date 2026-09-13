# Task: DSP clock divider (`CM_CLKSEL_DSP`)

> **DONE 2026-08-11. Response to client — `findings.md` §23–§24.**
>
> In brief: **the hypothesis "DSP is at half frequency" is false.** On the device,
> `CM_CLKSEL_DSP = 0x23C3`, field `[3:0] = 3`, meaning C55x is already at nominal
> 220.16 MHz. The bimodal "110 000 / 220 000" in `_BRIDGEINIT_DSP_FREQ` is
> `CORE`, not the DSP divider: `ord60` computes `CORE / [3:0]`, while
> `CM_CLKSEL2_PLL[1:0]` itself shifts between 1 and 2 under the DVFS governor. That is,
> the conclusion in §14.3 of `DSP_GUIDE.md` ("the value is what the driver configures, and
> a 2x change does nothing") was correct, whereas the later §15.4 was not.
> The MAC/cycle numbers were calculated at 220 MHz and do not require recalculation.
>
> Step 2 in the form of "6 → 3" is discarded. Instead, something not in the task was done:
> the divider is now **changed in both directions**, and 3 → 2 yields DSP 330.24 MHz.
> The path details below in §3 partly did NOT hold true — see corrections in §24.3–§24.4:
> option A is impossible in principle, and the `PRCM_CLKCFG_CTRL` latch is **mandatory**.
>
> Unresolved: with divider 2 the device did not survive loading the DSP base image
> (one observation, §24.5).

Client — neighboring project `../TMS320C55x test` (custom code on C55x in the same
device). There, it was measured that C55x yields ~50 MMAC/s, and ROM reverse engineering indicates that it
is clocked **at half nominal**. Verifying and, if confirmed,
fixing it can only be done from here — kernel access to PRCM is required, which exists only in
this project.

Device: Nokia N95 8GB (RM-320), Symbian 9.2 / S60 3rd FP1, OMAP2420.

---

## 1. What is already known (verified by ROM reverse engineering, not a guess)

Clock tree from `WALKTHROUGH.md` of this same project:

```
sys_clk 19.2 MHz -> DPLL = 19.2 * 86 / 5 = 330.24 MHz
CORE = DPLL * CM_CLKSEL2_PLL[1:0] = 660.48 MHz
```

The DSP domain is clocked from **CORE** via `CM_CLKSEL_DSP` = `0x48008840`
(PRCM base `0x48008000` + `DSP_MOD 0x800` + `0x40`).

- `660.48 / 3 = 220.16 MHz` — this is the "nominal 220 MHz" for C55x in this SoC;
- `660.48 / 6 = 110.08 MHz`.

**Measurement from the DSP side.** The driver `ehwaomap.pdd` @`0x801A7220`, after loading
the base image, writes into its symbol `_BRIDGEINIT_DSP_FREQ` the result of
`power.dll::ord60(1) / 1000`, while `ord60` (@`0x800B1620`) computes the frequency **from
the actual PRCM divider field**, not from a constant. The client project read this
symbol from the DSP side: the value is bimodal, **~110 000 and ~220 000 kHz**. That is,
the divider indeed can be both 3 and 6.

**Indirect confirmation by performance.** The `RPT CSR` + `MACM` loop costs ~2
cycles per element. At 110 MHz this yields ~55 MMAC/s; **50.2 MMAC/s** was measured. At
220 MHz twice as much would be expected. That is, the DSP almost always sits at 110 MHz.

**Why this cannot be fixed from the DSP side.** In `ehwaomap.pdd` there is a function `0x801AE900`
capable of writing fields of `CM_CLKSEL_DSP`, — but **there is not a single reference to it in
the entire ROM** (verified by searching for the address across all modules: neither `BL`, nor via
pointer tables). The ioctl dispatcher of the `Hwa` channel (`ehwa.ldd` @`0x801B52A0`)
has no frequency control commands at all. Hence the only way is to write the register
directly.

---

## 2. Step 1 — read only (zero risk, done without mechanical changes)

`CM_CLKSEL_DSP` resides in the **same 4 KB PRCM page** that the project already
uses:

```c
// src/ocengine.h, existing constants
const TUint32 KRegClkSelMpu   = 0xc6008140u;
const TUint32 KRegClkSel1Core = 0xc6008240u;
const TUint32 KRegClkSel1Pll  = 0xc6008540u;

// add:
const TUint32 KRegClkSelDsp   = 0xc6008840u;   // CM_CLKSEL_DSP
```

The READ patch is universal — `cmp r1,#0 ; ldrne r0,[r1] ; moveq r0,#0 ; bx lr`
dereferences any kernel address, so `COcEngine::ReadReg` works as is:

```c
TUint32 v = 0;
engine->ReadReg(KRegClkSelDsp, v);
```

**What needs to be done:** read and report the **full 32-bit value**
(hex), not individual bits. At the same time it is useful to read `CM_FCLKEN_DSP`
(`0xc6008800`), `CM_ICLKEN_DSP` (`0xc6008810`), and `CM_IDLEST_DSP`
(`0xc6008820`) — to see whether the domain is even enabled at the moment of reading.

**Why the raw value specifically is needed.** The field layout is inferred from the setter in
`ehwaomap.pdd` and contains an ambiguity that I do not want to guess:

| field | bits | how it is known |
|---|---|---|
| `CLKSEL_DSP` | `[4:0]` | from OMAP2420 datasheet; PDD setter does **not** touch it |
| `CLKSEL_DSP_IF` | `[6:5]` | PDD setter, field 1, mask `0x60` — reliable |
| ? | `[7]` | PDD setter, field 0, mask `0x80` |
| `CLKSEL_IVA` | `[12:8]` | PDD setter, field 3, mask `0x1F00` — reliable |
| ? | `[13]` | second PDD setter, mask `0x2000` |

That is, **which exact field clocks C55x — `[4:0]` or `[12:8]` — does not follow unambiguously
from reverse engineering.** The raw value resolves this immediately: whichever field contains 6 (or
3) is the one we are looking for. If 6 is in both — we read once more under load.

> If the required field turns out to be **3**, the hypothesis "DSP is at half frequency"
> is false, and step 2 **must not** be done — simply return the value, the client
> will look for the cause of slowness elsewhere.

---

## 3. Step 2 — write (only if step 1 showed 6)

Goal: read-modify-write of that field, 6 → 3. Preserve the remaining register bits **without
changes**.

### Why this is considerably simpler and safer than the existing relock blob

`CM_CLKSEL_DSP` is a divider of a **separate domain**, not a PLL. Therefore the following are **not needed**:
bypass DPLL, polling `CM_IDLEST_PLL`, execution from SRAM, disabling interrupts,
worrying about SDRAM refresh. None of the things for which `OcRelockBlob` was written
apply here: CORE, SDRAM, display, and USB are not affected at all.

### What is missing in the code

- `Poke`/`RPatcherChannel::WriteBytes` goes through `RomPatcherPlus.Driver`, meaning
  it **patches ROM-shadow pages** — not suitable for MMIO.
- The only thing that actually writes to PRCM is `OcRelockBlob`, and its stores are hardcoded
  to offsets `0x80` / `0x140` / `0x240` / `0x540`.

### Proposed implementation (option A, minimal)

WRITE patch, mirroring the existing READ patch:

```
cmp r1, #0
strne r2, [r1]
mov r0, #0
bx lr
```

and invocation `DoControl(1, (TAny*)addr, (TAny*)value)`.

**Unverified assumption:** that `a2` arrives at the patched body in `r2`. The existing
READ patch only proves that `a1` arrives in `r1`. Verification is cheap and
safe — write a known value to a harmless cell (e.g. an unused
word in the SRAM slot `iSram + KSramSlot`, where the project already writes and reads) and
read it back with the READ patch.

**If the assumption does not hold (option B):** embed the value into the patch body
as a constant and re-patch before each write — exactly as the project already
does for `OcRelockBlob` (`WriteRelockBlob` substitutes `0xDEADBEEF`, etc.).

### Mandatory after write

The same ROM-shadow rollback as elsewhere in the project: **restore `KDvfsOriginal`
to the body of `DoControl(1)`**. A patch left in ROM-shadow lives until reboot,
and a foreign call to `DoControl(1, ...)` with a non-zero `a1` will perform a write in the kernel at
an arbitrary address. For a WRITE patch this danger is **greater** than for READ:
`mmrmserver.dll` and `fmradiotunercontrol.dll` are also clients of `dvfs_interface`
(see §15 of `WALKTHROUGH.md`). Keep the window open minimally.

---

## 4. What to return to the client

1. Raw 32-bit values of `CM_CLKSEL_DSP`, `CM_FCLKEN_DSP`, `CM_ICLKEN_DSP`,
   `CM_IDLEST_DSP` **before** the change.
2. Conclusion on which field contains the divider and why.
3. If write was performed — value of `CM_CLKSEL_DSP` **after** the change (re-read, not
   what was written), and whether the device survived.
4. Whether the ROM-shadow patch remained removed.

## 5. Expected effect and risk

Effect: **2x C55x frequency**, from 110 to 220 MHz — up to stock for this SoC.
For comparison, overclocking DPLL to multiplier 111 would yield only 1.29x for DSP and drag
along SDRAM, display, and USB.

Risk: localized to the DSP domain. In the worst case audio crashes until
reboot — DSP/BIOS in the loaded base image configured its
timers for the old frequency. There is no system risk (SDRAM/display).

Verification of the effect will be done by the client: their `dsptest` loads
the base image on every run, so the driver will rewrite `_BRIDGEINIT_DSP_FREQ` with the new value — in
a single run both the new declared frequency and the new actual throughput
will be visible.
