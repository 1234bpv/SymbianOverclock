# Overclocking Nokia N95 8GB on Symbian: Full Technical Report

This document describes **how** runtime CPU overclocking from 330 to 426 MHz
(+29%) was achieved on Nokia N95 8GB running Symbian 9.2, from the first disassembly to a working
application. This is not a retelling of `findings.md` (which contains atomic technical facts), but an
end-to-end narrative of the work process: decisions, false hypotheses, and why each step
was taken in that exact way.

Target reader: an engineer who knows ARM assembly and hardware, but has not written for
Symbian. Therefore, Symbian-specific details are explained, while the basic architecture is not.

---

## 0. Hardware and Terminology in 5 Minutes

**SoC:** TI OMAP 2420. Inside:
- **ARM1136JF-S** — application core (ARMv6, single scalar pipeline,
  L1 16+16 KB, no L2). This is the processor that we are overclocking.
- **TMS320C55x DSP** — separate signal processor (audio/video).
- **PowerVR MBX** — 3D accelerator.
- A bunch of peripherals on two internal buses (see below).

**Clock tree — the key to everything.** A single PLL, followed only by integer dividers:

```
       19.2 MHz crystal
            │  DPLL: ×mult ÷(div+1)
            ▼
    ┌─── CORE_CLK ────────────────────────────┐   highest internal frequency
    │  = 19.2 × mult/(div+1) × core_sel        │
    │                                          │
    ├─ ÷ mpu_div  → MPU (ARM1136)   ← core     │
    ├─ ÷ l3_div   → L3 → SDRC → SDRAM ← memory │
    │              L3 ÷ l4_div → L4  ← periph  │
    ├─ ÷ dss1_div → DSS1  ← pixel clock        │
    ├─ ÷ ssi_div  → SSI   ← ~modem channel     │
    ├─ ÷ vlynq/ssi/usb...                      │
    └──────────────────────────────────────────┘
```

Stock (mult=86, div=4, core_sel=2, all measured on the device):

| Domain | Divider | Frequency |
|---|---|---|
| DPLL | — | 330.24 MHz |
| CORE | ×2 | 660.48 MHz |
| MPU (core) | ÷2 | 330.24 MHz |
| L3 (bus+SDRAM) | ÷4 | 165.12 MHz |
| L4 (peripherals) | ÷2 from L3 | 82.56 MHz |
| DSS1 (display) | ÷6 | 110.08 MHz |

**Main consequence of this topology:** raising the DPLL multiplier means raising
CORE, and along with it **everything simultaneously**: core, memory, display, DSP. This is both a strength
(memory does not lag behind the core) and the main difficulty (one cannot overclock just the
processor without affecting the display). Half of the work below is about dealing with specifically
this.

**Symbian specifics that will be needed:**
- `LDD` (Logical Device Driver) — a kernel-space driver, a `.ldd` file.
  The user side opens a channel via `RBusLogicalChannel::DoCreate`,
  communicates via `DoControl(func, a1, a2)` (synchronously) and `DoRequest`
  (asynchronously).
- `capabilities` — Symbian has capability-based security. A kernel driver requires
  certain capabilities from whichever process opens it.
- ROM — this is `TRomImageHeader` format (120-byte header), not `E32Image`.
  It executes **directly from ROM** (XIP), not copied into RAM. This will become important.

---

## 1. Starting Point: Why All Previous Symbian "Overclocks" Are Fake

The first thing I did was check the "overclocks" available on the net. All turned out to be
fakes of one of three types (`findings.md §1`):

1. Editing `20001079.txt` with the value 332 — this is an **N-Gage** config, it has
   nothing to do with frequency.
2. "OC patches" for RomPatcher changed the text string "MHz" in a DLL — that is, a **label**.
3. Utilities like Todo Speed gave ~1% in benchmarks — the effect of freed RAM.

Hence the first working rule that remained until the end: **measure work, not the
label.** `HAL::Get(ECPUSpeed)` on this device consistently lies (206438 kHz
regardless of actual state). Therefore, I built two independent instruments:

- **NOP chain**: 256 consecutive `add r3,r3,#1`. The ARM1136 is single-issue, each
  costs exactly 1 cycle, no dependencies, no I-cache misses (1 KB of code with
  a 16 KB cache). Cycle count is divided directly by time → absolute frequency.
- **ALU loop with data dependency**: LCG, each iteration waits for the previous one —
  time scales with core frequency, not with pipeline width.

Both must yield the same percentage of change. Divergence = trust neither.

---

## 2. Reverse Engineering: What and Where to Write

The goal narrowed down to a single register. While analyzing `power.dll` from the ROM dump
(objdump on the stripped 120-byte header), I found the formula directly in
the getter code `@0x800B1544`:

```
DPLL = sys_clk × mult / (div + 1)          [CM_CLKSEL1_PLL, phys 0x48008540]
mult = bits [21:12],  div = [11:8],  sys_clk_sel = [25:23]
```

**Peripheral mapping** was derived from two independent hits: virtual = physical +
`0x7E000000` (for the `0x48xxxxxx` window). That is, `CM_CLKSEL1_PLL` is accessible to the kernel at
`0xC6008540`. Later it turned out that for SDRC the offset is different — but more on that in §7.

At this stage, I knew **what** to write. Next, two years of Symbian homebrew folklore
claimed that this was impossible, and it was necessary to understand why.

---

## 3. Four Dead Ends (and Why Each Was Closed)

To obtain a write to a kernel register from the user side, I went through all the obvious paths.
Each was closed with **proof**, not an assumption (`findings §3, §7, §8, §11`):

| Path | Why it does not work |
|---|---|
| **DVFS API** (`dvfs_interface`) | It is a binary toggle: `SetPerformance` checks only `a2 cmp #50`, the level is hardcoded by constant `#66`. It does not provide overclocking in principle. |
| **Custom kernel LDD** | The public S60 SDK lacks `include\kernel\` and `ekern.lib`. There is nothing to build kernel code with. |
| **`TestPowerAPI`** (`d_powerapi.ldd`) | Gives access to a broader set of resources, but lacks an arbitrary address write primitive, and probing freezes the driver hard (resource 65 does not complete the request, there is no `Cancel`). |
| **Raw MMIO across the entire ROM** | Scanned **2712** modules for `kaomap` Read32/Write32/Modify32 imports. 24 import them; in each, the address is a hardcoded driver base, none takes an address from the client. |

The conclusion was formulated precisely: what was missing was not knowledge of "what to write" (known), but
**a working channel into the kernel with the correct access width and reading capability**.

---

## 4. Breakthrough: RomPatcher+ as a Write Primitive

`RomPatcher+` is a homebrew utility that patches ROM code at runtime. Its driver
`patcher.ldd` is compressed with Symbian-specific deflate (**not** zlib-compatible —
a proprietary Huffman implementation). I had to write a decompressor — a literal port of
`TInflater`/`Huffman` from open-source elf2e32, in Node.js
(`tools/syminflate.js`). Ported specifically to Node rather than PowerShell, because unsigned
32-bit shifts in PS would fall into the Int32 trap (see §9 about pitfalls).

Having analyzed the decompressed driver, I found the channel dispatcher
(`DoControl func 100..105`) and **function 105** — a single write:

```cpp
DoControl(105, &addr, &desc)
  → memmove(dest = addr, src = desc.Ptr(), len = desc.Length())
```

`memmove` was resolved against the export table of the **real** `ekern.exe` from the dump
(not from the `.def` file, as that turned out to be from a later kernel) — ordinal 725,
`@0x8006F800`.

**However**, two limitations for our task immediately surfaced (`findings §10`):
1. The channel **does not provide reading** of an arbitrary address.
2. For `len=4`, memmove takes a byte-by-byte branch (4×`strb`) — the wrong access width for
   32-bit PRCM registers.

Thus patcher is unsuitable for **direct** writing to PRCM. This forced a change of
the entire strategy.

---

## 5. Strategy Change: Patch CODE, Not Registers

The key idea. Instead of writing to MMIO ourselves (where both access width and reading are
problems), I decided to **patch kernel code that already knows how to do this correctly**.

Chosen target: the body of `dvfs_interface.ldd::DoControl(1)` @`0x80148410`. This is exactly 6
words; beyond that, from `0x80148428`, is the dispatcher, which must not be touched. Three sequential
patches of the exact same location yielded three primitives:

**READ patch** (16 bytes):
```asm
cmp   r1, #0       ; a1 == 0 ?
ldrne r0, [r1]     ; yes: read 32 bits at address a1
moveq r0, #0       ; no: safely return 0
bx    lr
```
Now `DoControl(1, addr)` reads any kernel address. **This resolved the lack of
reading.** The first READ on the device yielded `CM_CLKSEL1_PLL = 0x00056400` → mult=86,
confirming the entire model.

**EXEC patch** (later, secure version — see §8):
```asm
ldr  r0, [pc, #12] ; allowed address
cmp  r1, r0
bxeq r1            ; branch ONLY to it
mov  r0, #0
bx   lr
```
Now `DoControl(1, addr)` executes code at that address in kernel mode.

To what extent is this a "hack"? No — this is homebrew on **one's own unlocked** device:
the delivery daemon registers our dev build in the local hash registry
(`C:\sys\hash`), by which the stock loader recognizes its capabilities. A routine
Symbian homebrew mechanism.

---

## 6. Beachhead in SRAM and How Firmware Itself Changes Frequency

Here the most instructive part happened. Initially I recorded in `findings` the conclusion that
**there is no validated runtime relock path in the firmware**. That turned out to be **false**,
and false in our favor — I corrected it right in the text (§12/§16/§17, the discipline
of proof requires not rewriting silently).

The cause of the error was methodological: I only scanned `kaomap Write32` calls and only
**full** 32-bit address literals. But `power.dll` addresses part of PRCM and the **entire** SDRC
via a **direct `ldr`/`str` pair by base+offset**. The scan missed this.

In fact, the firmware contains a complete SRAM-resident relock routine (`findings §17`):
at boot, `power.dll` maps 4 KB of physical SRAM (`0x4029E000`, in our case linear
`0xCB202000`) and copies a routine there which, on deep idle, does:

```
self-refresh SDRAM → bypass DPLL → wait for CM_IDLEST_PLL → WFI →
relock → wait → relock DLL SDRC
```

This is the same structure as in reference Linux `arch/arm/mach-omap2/sram242x.S`.
However — and this was a new blocker — the copy is done **once at boot**, so a runtime
ROM patch does not hit the copy that actually executes.

**Solution:** use this SRAM chunk as our own beachhead. Via the EXEC patch, I
write my relock blob into the unused part of the chunk (offset +0x800) and execute it
from there. SRAM is critical because:
- during bypass, CORE = sys_clk, SDRAM is starved without refresh — code cannot be
  in SDRAM;
- the blob must be **completely self-contained**: no external `bl`
  (after copying, relative branches would lead nowhere), all constants in
  its own literal pool (`.ltorg`).

The test worked immediately: wrote `mov r0,#0x5A; bx lr` into SRAM, called it
via the EXEC patch — it returned `0x5A`. Thus SRAM is accessible, executable, and coherent
with respect to I-cache.

---

## 7. The First Overclock and What Else Had to Be Accounted For

The relock blob (detailed bit-by-bit breakdown — `findings §18`, `src/ocengine.cpp`).
Three details, each of which breaks the result without prior experience:

1. **`EN_DPLL` is bits [1:0], not [2:0].** Bits [3:2] are the 54 MHz APLL; mask `#7`
   would disable it along with DPLL. Mask `#3` was taken from the code of power.dll itself.
2. **SDRAM into self-refresh before the window** (`SDRC_POWER |= 0x48`). Without this,
   a headless run succeeded, but with a live display the device froze: DSS
   continuously reads the framebuffer, while we take away memory during the transition.
3. **All waits are bounded by a counter.** Interrupts are disabled, so
   an infinite loop = pulling the battery. On timeout — hardware rollback.

**New SDRC base:** `0xC6129000` (phys `0x68009000`) — proven by the match of six
offsets to known registers. The mapping here is **not** `+0x7E000000`; the rule of §4 applies only
to the `0x48xxxxxx` window.

First result (mult 86→91): **+5.8%**, confirmed by both instruments, 0
memory errors. Overclocking worked.

---

## 8. Four "Reboots" — In Reality, None

The most difficult diagnostics. While working with the GUI, the device "rebooted"
several times. Twice I put forward a hypothesis and twice I was mistaken — both times corrected
honestly (`findings §19, §20`).

**The key was provided by direct measurement.** I started printing `User::NTickCount()` (ticks since
system start, reset only on full reboot). It showed **12 hours
of continuous operation** — meaning the kernel **did not crash a single time**. The "reboot"
was a restart of the UI shell after the death of a system process.

A lesson in methodology: initially I "proved" the absence of a reboot by network availability.
That is not proof — the device manages to come back up between checks. Direct measurement of uptime is
proof.

**Two real causes:**

*Cause A (§19): rewriting bytes does not remove the patch.* Patcher does not execute `Cache::IMB`,
the patched function remains in the I-cache. Observed directly: after restoring
the bytes, `DoControl(1,0)` returned 0 (the patch's response), rather than 50/100 (stock code).
Only `DoControl(100)` = `FreeShadowPage` reliably removes it — that same patcher has
a stock opcode that drops the RAM page and restores the mapping back to ROM along with
cache maintenance.

*Cause B (§20): the EXEC patch was dangerous for foreign calls.* A simple `bx r1`
meant that any call to `DoControl(1, a1)` with non-zero `a1` **from another
client** (and the `dvfs_interface` channel is also opened by `mmrmserver.dll` and the window
server) branched in kernel mode to an arbitrary foreign address. Hence the safeguard "branch only to one
expected address" from §5.

Both errors are of the same kind: when hooking a kernel function, one must assume that
it will be called not only by your code, and one must clean up reliably.

---

## 9. Display: Why +2 Froze, While +21 Did Not

The user provided a crucial observation: **lowering** the multiplier (77) freezes just
the same as raising it. Therefore, any change to CORE breaks things, and rendering breaks.

Disassembly of the seven setters in `power.dll` yielded a full map of `CM_CLKSEL1_CORE`. The display
(DSS1) is clocked by `CORE / dss1_div`, stock ÷6 = 110 MHz. By raising CORE, we
were raising the pixel clock without compensating for anything.

**Solution:** compensate dividers in the **very same commit** of `CLKCFG_CTRL` as the
multiplier:
```asm
str  r6, [r4, #0x540]   ; new CM_CLKSEL1_PLL
str  r2, [r4, #0x240]   ; new CM_CLKSEL1_CORE (L3, DSS1 dividers)
str  r0, [r4, #0x140]   ; new CM_CLKSEL_MPU
str  #1, [r4, #0x80]    ; single commit latches everything together
```
A single commit — so that domains never see the new CORE with an old divider.

Selection policy differs: **DSS1** is compensated exactly to stock (the display needs
its own frequency), **L3/SDRAM** is kept as high as possible within +15% (measurements showed 0
errors at +11.5%, lowering it would be a waste).

---

## 10. Where the Limit Actually Lies

Final measurements by the user on the device:

| mult | MPU | State |
|---|---|---|
| 107 | 411 MHz (+24%) | stable, calls+WiFi, bus exactly stock |
| **111** | **426 MHz (+29%)** | **maximum stable** |
| 112–116 | 430–445 | auto-reboot within minutes |
| 129 | 495 | instant reboot |

Row **129 is decisive**: there `129/86 = 1.5`, and the L3/DSS dividers are multiplied exactly
by 1.5, meaning the bus and display are **exactly at stock frequencies** — yet the device still
crashes. Therefore **the limit is the ARM1136 itself at stock voltage**, not the bus and not the display.

The failure profile confirms this: 426 holds indefinitely, 430 crashes within minutes,
495 does not start — a classic frequency-voltage ceiling. There are no application freezes
in this process (distinguishing it from SDRC timing violations).

Practical result — **SPmark 3D: 62 → 75 fps (+21%)**, interface and emulation
are noticeably faster.

The next obvious lever is **voltage** (Menelaus PMIC), which we did not touch.
However, this poses irreversible risk to the silicon, not reversible by a reboot, so that is a separate
topic.

---

## 11. The Application

`ocgui` — AVKON application (fullscreen, joystick control). Two-level
protection: slider stages → `APPLY` arms → separate `CONFIRM` executes. Tests
(ALU, bandwidth, SDRAM integrity, cache, combined) — each is self-verifying: ALU
runs LCG forward and backward via a modular inverse multiplier, while the rest verify
a pseudorandom pattern. That is, each test is simultaneously a benchmark and a silent-error
detector.

The engine (`ocengine.cpp`) is shared with the console probe — the relock blob is in one
place, because two diverged copies would hang the device.

Versions: v1.0 (auto-selection of dividers, `release/v1/`), v1.1 (manual dividers for all
domains), v1.2 (MPU divider — to test the hypothesis "the limit is in CORE, not in ARM":
`mpu_div=1` gives the same core frequency at half the CORE frequency).

Signing: RSA+SHA-1 is mandatory (Symbian 9.2 does not parse SHA-256 and blames
the file), `.pkg` in ASCII+CRLF (makesis corrupts UTF-8/LF), verification with `signsis -o`.

---

## 12. What Remains Open

- **Voltage (Menelaus)** — untouched; the only lever beyond 426 MHz.
- **SSI** (likely the modem channel) scales without compensation; at 111 this is
  +29%, cannot be compensated exactly (dividers only 1,2,3,4,6,8).
- **CPU instruments show less gain than calculated** (+8.5% instead of +11.6% at
  mult 96), whereas the bus scales precisely. Hypothesis: the governor moves
  `mpu_div`. Being verified by manual mode in v1.2.

---

## References

The official OMAP2420 TRM (SPRU749) is under NDA and publicly unavailable. All register
work relied on three open replacements, each cross-checked against device code:

- **Linux kernel (mach-omap2)** — the primary source of masks and sequences. Masks
  `CM_CLKSEL1_CORE` and `CM_CLKSEL_MPU` were verified bit-for-bit against
  [`cm-regbits-24xx.h` (v3.7)](https://raw.githubusercontent.com/torvalds/linux/v3.7/arch/arm/mach-omap2/cm-regbits-24xx.h);
  the relock sequence — against
  [`sram242x.S`](https://raw.githubusercontent.com/torvalds/linux/v3.7/arch/arm/mach-omap2/sram242x.S);
  stock OPPs — against `opp2420_data.c`.
- **Public TI materials** (high-level overviews, without registers):
  [OMAP2420 Key Features](https://www.ti.com/pdfs/wtbu/TI_omap2420.pdf),
  [OMAP2420 SDP](https://www.ti.com/pdfs/wtbu/TI_sdp_omap2420.pdf).
  The closest available complete TRM is for the related
  [OMAP35x (SPRUF98)](https://www.ti.com/lit/ug/spruf98y/spruf98y.pdf): PRCM/SDRC
  of the same family conceptually match, convenient as a reference, but **not**
  equivalent in addresses.
- **ROM dump of the device itself** (`eka2l1/.../rm-320/`) — the final arbiter: everything
  I asserted was either confirmed on a live device or derived from this code.

Project reading order: `CLAUDE.md` → `docs/state.md` → `docs/findings.md`
(§17–22 — the essence of overclocking) → this document for the end-to-end picture.
