# How It Was Done: Full Technical Breakdown

This document is a narrative breakdown of the entire work: what was done, in what order,
why, which branches turned out to be dead ends, and which conclusions had to be retracted.
`findings.md` answers the question "what has been established", `state.md` — "where we
are now"; here is the answer to "how we got here and why in this exact way".

**Audience.** For an engineer who knows hardware well and reads assembly, but has not
worked with Symbian. Therefore, there is a brief primer on Symbian specifics (Section 2)
and no primer on PLLs, caches, or MMIO.

**What is not here.** Repetition of numbers and proofs from `findings.md` — they are
referenced in the form of "§17". Best read in parallel.

---

## 0. The Problem in One Paragraph

Nokia N95 8GB (RM-320), TI OMAP 2420, Symbian 9.2, nominally 330 MHz. The same
silicon in the Nokia N810 runs in production at 400 MHz, meaning hardware headroom definitely
exists. The question: is it possible to reach it from the user side on a closed
OS that has neither sysfs, nor cpufreq, nor a documented frequency API, and without a
Kernel DevKit, which does not exist in public access.

The answer turned out to be affirmative: **330 → 426 MHz (+29%)**, at runtime,
without writing to flash. But the journey took about fifteen stages, of which four
were dead ends, and three conclusions had to be publicly retracted as false.

---

## 1. Hardware: What Exactly We Are Tweaking

### 1.1 The OMAP2420 Clock Tree

```
sys_clk 19.2 MHz (crystal)
    │
    ├─ DPLL:  CM_CLKSEL1_PLL[21:12] = mult, [11:8] = div
    │         DPLL = sys_clk * mult / (div + 1)
    │         nominal: 19.2 * 86 / 5 = 330.24 MHz
    │
    ├─ CORE = DPLL * CM_CLKSEL2_PLL[1:0]        nominal ×2 = 660.48 MHz
    │
    ├─ MPU  = CORE / CM_CLKSEL_MPU[4:0]         nominal /2 = 330.24 MHz  ← ARM1136
    │
    └─ CM_CLKSEL1_CORE — seven independent dividers from CORE:
         L3   [4:0]   /4 = 165.12 MHz   ← bus + SDRAM controller
         L4   [6:5]   peripheral bus
         DSS1 [12:8]  /6 = 110.08 MHz   ← display controller
         DSS2 [13]
         VLYNQ[19:15]
         SSI  [24:20]
         USB  [27:25]
```

The key property that defined all subsequent work: **DPLL is a single
multiplier for the entire system.** By raising it, you raise not the processor, but CORE, and
from CORE everything else is clocked via integer dividers — memory, display,
USB, SSI. The MPU divider is separate, and in this lay the only chance for something
more subtle — but it is an integer divider and already set to 2, so the next step
(/1) would give a jump of 330 → 660 MHz. There is nothing intermediate (§4, §15).

Hence the main architectural problem of the project: **you cannot overclock the processor
without affecting memory and the display.** It is solved by compensating dividers in the
same latch — Section 3.14 below.

### 1.2 Three Things That Make Frequency Change Non-Trivial

1. **The DPLL cannot be reprogrammed "on the fly".** It must be put into
   bypass (then CORE = sys_clk, meaning the entire system runs at 19.2 MHz), wait
   for confirmation in `CM_IDLEST_PLL`, write the new multiplier, latch
   `PRCM_CLKCFG_CTRL`, switch back to lock, and wait again. This is a window of
   several hundred microseconds during which the system runs 17 times
   slower.
2. **SDRAM cannot be touched during this window.** The SDRC controller is clocked from
   L3, which means from the very same CORE that is shifting. Any memory access
   during the transition is undefined behavior. Therefore, the critical section must
   execute **not from SDRAM**, but from the SoC's internal SRAM, with its own
   literal pool and without a single external call.
3. **And the display cannot be left unattended either.** The DSS continuously reads
   the framebuffer from SDRAM via DMA. This is precisely where the project stumbled: the
   exact same sequence that worked flawlessly with the screen off hung the device
   completely with a live GUI (§20).

All of this is not our invention. Exactly the same structure exists in Linux's `sram242x.S` for
OMAP2420 and, as became clear later, in the N95 firmware itself (§17). The problem was
not in devising the sequence, but in having the means to execute it.

### 1.3 ARM1136 as a Measuring Instrument

The ARM1136JF-S is an eight-stage scalar pipeline, one instruction per cycle for a
simple ALU operation without data dependency. This provides an honest way to measure the
**real** frequency: a chain of 256 independent `add` instructions unrolled in a loop, with
`ALWAYS_BUILD_AS_ARM` in `.mmp` (so the compiler does not slip into Thumb, where cycle
counts are different).

Why is this needed at all if `HAL::Get(ECPUSpeed)` exists: because on this
device it **lies**. It consistently returns 206438 kHz regardless of the actual
state — both before overclocking and after. This is not a curiosity, but the reason why all public
"Symbian overclocks" turned out to be fake: people measured a label, not actual work (§1, §5).

---

## 2. Symbian for the Hardware Person

In brief, what is essential to make sense of the rest of the document.

### 2.1 EKA2, User/Kernel, Drivers

Symbian 9.x is an EKA2 microkernel with strict user/kernel separation. A driver
consists of an **LDD** (logical device driver, `.ldd`, logic) and a **PDD**
(physical, `.pdd`, hardware). A user-side client opens a channel:

```cpp
User::LoadLogicalDevice(_L("patcher.ldd"));   // load driver
RBusLogicalChannel ch;
ch.DoCreate(_L("RomPatcherPlus.Driver"), TVersion(0,0,0), ...);
ch.DoControl(105, a1, a2);                     // synchronous call
ch.DoRequest(n, status, a1, a2);               // asynchronous
```

A detail critical for us: **`DoControl` is synchronous and executes in the context of
the client thread**. Therefore, the driver can dereference user pointers directly, without
`kumemget`. This is precisely what allowed passing addresses and descriptors into the patcher
without any wrapper (§14).

`DoRequest`, conversely, is asynchronous, completing via `Kern::RequestComplete` into a
`TRequestStatus`. And here is the first pitfall: **if a request does not complete, there is no way
to cancel it** when the driver has not implemented `Cancel`. This is exactly how `TestPowerAPI`
index 6 hung the device hard (§7).

### 2.2 Platform Security and Capabilities

Each executable carries a set of capabilities in its E32 header. A driver can
require them from the client. Our set:
`PowerMgmt WriteDeviceData ReadDeviceData ReadUserData WriteUserData ProtServ`.
It proved sufficient for both `dvfs_interface` and `patcher.ldd` — there was no need to
expand it.

Signing: on a dev-unlocked device, it is sufficient for the binary's SHA-1 to be in the
local registry `C:\sys\hash`. This is exactly what the delivery daemon does, so a SIS is not
needed for development — the EXE is placed directly in `E:\sys\bin`.

### 2.3 ROM Is XIP, and That Is Central

The Symbian ROM executes **directly from flash** (execute in place). Modules in it
do not have the familiar `E32ImageHeader` — they have a `TRomImageHeader`, **120 bytes**
(rather than the 100+ of E32), with code immediately following it. This is practically important: to
disassemble a module from a dump, one must strip exactly 120 bytes and give
objdump the correct VMA:

```
objdump -D -b binary -m arm --adjust-vma=<iCodeAddress> <file without 120 bytes>
```

It is easy to make a mistake here, and the consequence is nasty: all addresses will shift, while the
code still looks plausible.

**ROM-shadow.** Because the ROM is not writable, the runtime patch mechanism works like this:
the kernel allocates a RAM page, copies the ROM page into it, remaps the virtual
address to the RAM copy (`AllocShadowPage`), and thereafter the CPU executes RAM. The reverse
operation is `FreeShadowPage`. Keep this in mind: **overwriting the bytes back does not
unpatch**. Why — Section 3.12, this is the main pitfall of the entire work.

### 2.4 Imports and Ordinals

ROM modules are linked by **ordinals**, not by name. `patcher.ldd` imports
45 functions from `ekern.exe` — in a disassembler these are simply stubs with numbers. To
find out what is actually called, one must parse the **export directory of the real
`ekern.exe` from the same dump**: an array of 32-bit addresses, with ordinal `N` at entry
`[N-1]`.

Here lay a separate trap: `ekernu.def` from public Symbian sources turned out to be from a
different kernel version, with shifted ordinals. Had we used it, the entire chain would have been deduced
incorrectly, yet plausibly. A sanity check saved us: ordinals 401 and
402 had to be an adjacent lock/unlock pair, and in the real table they
indeed sit 96 bytes apart and have exactly that semantics. An index
shift of one would not have yielded such a match (§10).

---

## 3. Chronology: Fifteen Stages

### 3.1 ROM Inventory

The RM-320 ROM dump was unpacked (located in `eka2l1/data/drives/z/rm-320/`), containing
**2712 modules**. A search by name immediately yielded five DVFS modules:
`dvfs_interface.ldd`, `dvfs_engine.dll`, `dvfs_hal.dll`, `dvfs_conf.dll`,
`dvfs_monitor.dll`, all with UID2 = `0x100000AF` (kernel-side).

That is, **dynamic frequency scaling does exist in the firmware**, with a governor and
policies. The only question was what of this is visible from the user side.

### 3.2 Dead End #1: User-Side DVFS API

`dvfs_interface` opens, capabilities are sufficient. Disassembly of
`HandleMsg` revealed the message layout, followed by disappointment:

- `DoRequest(0, ...)` → `SetPerformance` compares the argument against 50 and operates as a
  **binary latch**: "someone needs performance / no one needs it".
  The level is hardcoded to constant 66;
- `dvfs_hal` quantizes any request to **two points**: 1..50 → 50, 51..100 → 100;
- `DoControl(1)` only returns a percentage of nominal.

Verified on device: `reserve(100)` and `release(0)` give identical 322–324 MHz.
**Overclocking via the stock API is impossible in principle** (§3).

What was valuable from this stage was not the result, but `DoControl(1)`: a ready, legally
reachable call into the kernel from the user side. Fifteen stages later, it would become the
entry point for everything.

### 3.3 Dead End #2: TestPowerAPI

`d_powerapi.ldd` was found in the ROM — the `d_` prefix in Symbian indicates a test
driver, and such drivers intentionally expose kernel APIs. It imports `kaomap.dll +0xF0`
and `+0xF8`, meaning **Write32 and Modify32** — exactly the missing primitives.

Analysis of the jump table yielded a list of resources (1..6, 65, 66, 67, 69, 70) and, as it
seemed, passing of the level from the caller. **This was a code-reading mistake**, and it was
honestly corrected in `findings.md` §7: `mov r2, r3` takes `r3` not from an argument, but from a
global where the driver at initialization stores a pointer to its own
callback object. That is, this is standard "acquire/release a fixed resource".

The cost of verification proved high: **resource 65 (index 6) never completes**,
the driver has no `Cancel`, the state remains in the kernel and is only cleared by a
device reboot. And because the delivery daemon is single-threaded and waits for process
exit, the hanging probe locked up the entire delivery channel.

Hence a set of safety rules that remained in effect until the end of the project:

- `TRequestStatus` for a driver must be **static**, not on the stack (otherwise late completion
  writes into someone else's stack frame);
- every request is wrapped in an `RTimer` with a timeout;
- a timeout on the request is not enough — blocking can already happen in `LoadLogicalDevice` or
  `DoCreate`, which are synchronous and have no timeout at all;
- **everything risky — only behind an explicit argument.** A regular run must be
  one after which the device is guaranteed to respond.

### 3.4 Dead End #3: Custom Driver

The obvious move — write a custom LDD with three lines of code. It does not work:
the public S60 3rd FP1 SDK **does not contain the kernel-side part**. There is neither
`include\kernel\`, nor `ekern.lib`. A Kernel DevKit is required, which Nokia never
published.

Therefore: executing our own code in the kernel could only be done by proxy.

### 3.5 Symbian-inflate: An Unexpectedly Large Chunk of Work

There remained RomPatcher+ — the legendary Symbian homebrew tool capable of
patching the ROM at runtime. `dumpsis -x` yielded three drivers, all compressed with
`KUidCompressionDeflate` (`0x101F7AFC`).

And here it turned out that **Symbian-deflate is incompatible with zlib.** It is a proprietary
Huffman implementation. `DeflateStream` cannot decompress it at any header offset;
`petran`/`elftran` only accept PE/ELF as input, not a ready E32.

We had to build a literal port of the decompressor from public Symbian source code
(`elf2e32`: `huffman.cpp`, `inflate.cpp`) — `tools/syminflate.js`.

Two details worth mentioning separately, as they cost time:

1. **Porting to Node specifically, not PowerShell.** The algorithm makes heavy use of
   unsigned 32-bit shifts (`reverse`, `huff >> 17`). PowerShell
   parses an 8-digit hex literal as `Int32` and silently makes it negative —
   this trap ate results in this project **three times**, each time silently. In JS
   there is `>>>`, so the port is native.
2. **The `HuffmanSubTree` trap.** In C++, the `aLevel` parameter has the type `TUint32**` and is
   passed **by value**. If one makes the level shared state in the port,
   the first recursive call will corrupt the level for the second. The symptom is treacherous:
   trees build, decoding proceeds, the output looks almost correct, and
   cuts off by a few bytes.

The proof of correctness was not "looks plausible", but two independent criteria: the files
decompress to exactly their `iUncompressedSize` (2688/2700/2780 bytes), and the initial
bytes yield a valid ARM prologue `stmfd sp!,{r4,lr}` / `cmp r0,#3` (§9).

### 3.6 Dissecting patcher.ldd: Write Primitive Found

Disassembly of the unpacked driver yielded the complete channel map:

- dispatcher `0x8604`: `sub r1,#0x64; cmp r1,#5; ldrls pc,[pc,r1,lsl#2]` —
  channel functions 100–105;
- routing by address: if the target is within the ROM range `[base, top)` — the
  ROM-shadow path with page preparation; if outside it — **direct write**;
- the write itself in both cases — kernel import **ordinal 725** with three arguments
  under a lock/unlock pair (401/402).

Resolving ordinal 725 against the export table of the real `ekern.exe` gave
`0x8006F800` = **`memmove(dest, src, len)`**: `cmp r2,#0; bxeq lr`, overlap
check, jump-table of byte-by-byte copying for `len ≤ 15`, word-loop for
larger sizes.

The call format was proven down to the client code level:

```cpp
TUint32 addr = 0x800B15DC;
TBuf8<64> patch;  patch.Copy(bytes, n);
ch.DoControl(105, &addr, &patch);   // dest = *a1, src = a2->Ptr(), len = a2->Length()
```

### 3.7 Two Limitations That Changed the Strategy

It seemed the problem was solved: arbitrary write to kernel memory exists, the address of
`CM_CLKSEL1_PLL` is known, the formula is known. But a full analysis of all six channel functions revealed
**two limitations, both straight from the code**:

1. **Arbitrary address reading does not exist.** Function 105 takes `src` only from
   a client descriptor, 103 is a fixed getter. memmove is symmetric, but the
   channel never allows substituting `src` = arbitrary kernel address. Without reading
   the current register value, a read-modify-write of a bitfield is impossible:
   a blind write of the whole 32-bit word would overwrite neighboring fields.
2. **Incorrect access width for MMIO.** For `len = 4`, memmove takes the branch `len ≤ 15`,
   meaning **four separate `strb` instructions**. Part of the OMAP2420 CM registers reliably
   accept only 32-bit access, whereas byte-by-byte access produces three intermediate register states.
   Forcing the word path for a 4-byte register is not possible.

`patcher.ldd` was created for patching **code bytes** in ROM, where byte-by-byte
copying is completely valid. For programming device registers, it is
unusable.

### 3.8 Searching the Entire ROM for an MMIO Primitive: Negative Result

Before changing strategy, it was worth verifying that the required primitive
truly did not exist anywhere. We scanned all 2712 modules for kaomap imports of
`Read32`/`Write32`/`Modify32`. 24 import them, six LDDs have a user side. For
each call-site, the address source was classified, and suspicious ones were checked manually.

**In every case, the address is the driver's own hardcoded base** (a walking-pointer across
its peripheral's register block); not a single one takes an address from the client. The result
was negative, but it closed the branch conclusively and honestly: production drivers do not expose raw
MMIO, and test drivers operate at fixed addresses (§11).

### 3.9 The Pivot: Patching Code, Not MMIO

The key idea of the entire project, which removes both limitations in one stroke:

> Do not attempt to write to registers on our own. Instead, **patch the firmware code**
> that already knows how to do it correctly.

Why this works:

- `power.dll` code resides in the ROM region, precisely where the patcher takes the
  shadow path. A code patch is a write to **regular memory**, where byte-by-byte
  memmove is valid. Limitation #2 disappears;
- access to registers is performed by `power.dll` itself using its correct 32-bit
  kaomap primitives. We only change what and where it reads;
- a trigger from the user side already exists — the same `DoControl(1)` from stage 3.2.

### 3.10 READ-Patch: First Contact with the Device

The target is the body of the function invoked by `DoControl(1)`, at address `0x80148410`.
A 16-byte patch:

```asm
cmp   r1, #0          ; 00 00 51 E3
ldrne r0, [r1]        ; 00 00 91 15
moveq r0, #0          ; 00 00 A0 03
bx    lr              ; 1E FF 2F E1
```

That is, `DoControl(1, addr)` now returns the contents of any kernel address, while
`DoControl(1, 0)` safely returns zero. This is the missing read primitive.

**The order of increasing risk was strictly observed.** First — a self-test with
zero risk to the kernel: a write via func 105 into our **own user buffer**.
An address outside ROM takes the direct path, kernel code is not touched at all. This
proves that the driver loads, the channel opens, capabilities are sufficient, and the
func 105 convention was decoded correctly. Only after success — patching the kernel.

First real numbers from a live device (2026-08-07):

```
CM_CLKSEL1_PLL = 0x00056400   sys_clk=19.2 MHz, mult=86, div=4  → DPLL 330.24 MHz
CM_CLKSEL2_PLL = 0x00000002   core_sel=2  → CORE 660.48 MHz
CM_CLKSEL_MPU  = 0x00000002   mpu_div=2   → MPU  330.24 MHz
CM_CLKEN_PLL   = 0x0000000F   DPLL locked
```

Matches the independent NOP measurement and `DoControl(1) → 100` before the patch. Seven
stages after starting, we saw for the first time the actual state of the device rather than a guess.

### 3.11 The Isolation Fallacy That Cost Three Reboots

The READ-patch plan stated: "the patch is isolated, no one in the stock
system calls `DoControl(1)` except our probe". **False.** The assertion
applied to callers of the *internal body* inside the `.ldd`, but was generalized to
the entire channel.

A ROM scan revealed that the channel name `dvfs_interface` resides as a UTF-16 string (`_LIT`
form for `DoCreate`) in two other system modules: `mmrmserver.dll`
(Multimedia Resource Manager) and `fmradiotunercontrol.dll`. While the patch is applied,
**any call by them to `DoControl(1, a1)` executes `ldr r0,[r1]` in kernel
mode at their address.**

Both observed crashes occurred when the device sat idle with the patch left in place —
precisely when the resource manager polls status.

A methodological detail is important here: **the baseline for comparison**. The device had been reflashed
with stock latest firmware a few days before the work and was completely
stable. RomPatcher writes nothing to flash. That is, the crashes cannot be written off as "old
hardware with modified firmware" — before our patches, they did not occur.

The lesson formulated in `findings.md` §20 and worth highlighting upfront:

> When hooking a kernel function, one must assume that code other than your own
> will call it. A patch must be safe for **any** caller — otherwise the window
> of exposure will sooner or later act as a landmine.

### 3.12 The Main Pitfall: Overwriting Bytes Does Not Unpatch

Four "reboots" during the work. The first thing done was to check whether these were
actually reboots at all.

**Network availability is not proof.** The device manages to come back up between checks.
Direct proof is `User::NTickCount()`, the nanokernel tick counter, which
resets only on a full reboot. The measurement after all four events: **43404 s
≈ 12 h**, exactly dating to the last genuine reboot the day before.

Therefore, **the kernel did not crash once**. What looked like a reboot was a restart
of the Symbian shell after a user-side system process died. The probe
now prints `UPTIME:` as the first line of every run, which simultaneously distinguishes
a kernel reboot from a shell restart.

And the real cause turned out to be this. Not a single path in the patcher **executes
`Cache::IMB`**. The patched function had already executed, its instructions reside in
the I-cache, and a write with *data* does not invalidate the instruction cache. The CPU continues to execute the old,
patched copy — "restoration" occurs only in memory.

Observed directly, using the same test on the same device:

| Unpatch method | `DoControl(1,0)` after it | What this means |
|---|---|---|
| overwrite original bytes | **0** | patch ALIVE (0 is patch response to `a1=0`) |
| + `FreeShadowPage` | **50** | stock code, actual percentage |

The solution was inside the patcher the whole time: **channel opcode 100** is `FreeShadowPage`. It
discards the RAM copy and restores mapping to the genuine ROM along with correct
cache maintenance.

Hence three rules, each bought with a reboot:

1. preserve original words and restore them after **each** operation, rather than at the
   end of the session;
2. after overwriting, `UnshadowKernelPage()` is mandatory;
3. rollback must be **verified functionally, and specifically after unshadow**: stock
   `DoControl(1, 0)` returns a percentage (50 or 100), the patch — exactly 0. A non-zero
   value is the proof. Reading the restored bytes directly is impossible —
   the read primitive IS those bytes.

Proven by bisection: the probe has `patcher level 0..3` levels, each leaving behind
exactly one more thing. After the fix, all four are clean, and uptime between them is
continuous.

### 3.13 Retracted Conclusion: The Firmware DOES Have an SRAM-Resident Critical Section

`findings.md` §12 stated: "there is no validated runtime relock path in the
firmware". This is **false**, and the cause of the error was methodological, which is why it is detailed here separately.

The §11/§12 scan had two blind spots:

1. it only counted calls through `kaomap`. But `power.dll` touches part of the PRCM and **all**
   of the SDRC with **direct `ldr`/`str`**, bypassing kaomap;
2. it searched for **full** 32-bit address literals. `power.dll` addresses these blocks as
   **base + offset**, so `CM_IDLEST_PLL` (`+0x520`) and all SDRC registers did not appear
   in the scan at all.

When searched properly, the following was found (all straight from the code, §17):

- at boot, `power.dll` maps 4 KB of physical `0x4029E000` (internal OMAP2420
  SRAM) and copies a 324-byte procedure from `0x800A7F6C` into it;
- on deep idle, it executes **from SRAM**: DSB → SDRAM into self-refresh →
  bypass DPLL → poll `CM_IDLEST_PLL` → WFI → relock → poll → relock DLL
  SDRC;
- this is the same structure as in `sram242x.S`, confirmed not by retellings of Linux, but by
  the code of this device;
- at the same time, the SDRC parameter was settled: `SDRC_RFR_CTRL = 0x0004DA01` (≈165 MHz).

Relocatability of the procedure was verified separately: in the range `0x800A7F6C..0x800A80AC`
there is not a single external `bl`, only PC-relative literal loads and
local branches.

**The new, precise blocker:** copying into SRAM happens once at boot. That is, a
runtime ROM-shadow patch at `0x800A7F6C` hits not the copy that actually
executes.

Yet the mere fact that SRAM is **mapped and accessible** proved more important than the
blocker: this was a ready staging ground for our own code.

### 3.14 Three Primitives, and From Them — Overclocking

The final construct consists of three things, none of which requires a
Kernel DevKit and none of which writes to flash:

1. **Arbitrary kernel address reading** — the READ-patch from stage 3.10.
2. **Arbitrary kernel code execution** — the same patch, but a jump instead of a read.
   The body of `DoControl(1)` is exactly 6 words; the dispatcher starts with the
   following word and remains intact. Calling contract on entry: `r0` = this,
   `r1` = `r2` = `a1`, `lr` = return address. A single 32-bit argument.
3. **Staging ground outside SDRAM** — the same SRAM chunk that `power.dll` maps at
   boot. On this device, the linear address is `0xCB202000`; the first 1024 bytes are occupied by
   firmware, we write at offset `+0x800`. The patcher writes there via the direct path
   (address outside ROM), and byte-by-byte memmove is valid here — this is regular memory,
   not MMIO.

Before the first relock, everything was verified sequentially: a `mov r0,#0x5A; bx lr` blob,
written into SRAM and executed, returned `0x5A`. This means the chunk is writeable,
executable, and coherent with respect to the I-cache. A call to a stub in ROM was verified separately.

The relock blob itself (272 bytes, executed from SRAM):

```
IRQ+FIQ off
SDRC_POWER |= 0x48            SDRAM into self-refresh
CM_CLKEN_PLL[1:0] = 2         fast-relock bypass
wait for CM_IDLEST_PLL[1:0]==1
CM_CLKSEL1_PLL = new          multiplier
CM_CLKSEL1_CORE = new         dividers — IN THE SAME latch
PRCM_CLKCFG_CTRL = 1          wait for self-clear
CM_CLKEN_PLL[1:0] = 3         lock
wait for CM_IDLEST_PLL[1:0]==2
SDRC DLLA/DLLB relock + delay
IRQ restore
```

Four details, each of which would have broken the result:

- **`EN_DPLL` is bits [1:0], not [2:0].** Bits [3:2] are the 54 MHz APLL; mask `#7`
  would have disabled it along with DPLL. Mask `#3` was taken from `power.dll` code
  (`and r2,r2,#0xfc`), not from memory of the datasheet.
- **Not a single external `bl` and not a single access to SDRAM in the critical window.**
  Constants are from the blob's own pool (`.ltorg` at the end), so `ldr [pc,#off]`
  remains valid after copying. The stack is touched only before and after the window.
- **All waits are bounded by a counter** (`0x10000`). Interrupts are disabled,
  so an infinite loop would mean a total freeze requiring battery removal. On
  timeout, the multiplier is not written at all, and the DPLL is forced back into lock.
- **Hardware rollback inside the critical section.** If the DPLL fails to lock, the blob
  returns to bypass, writes the **initial** multiplier, and locks onto it —
  all while interrupts are still disabled. Without this, the DPLL remained in bypass, MPU dropped
  to ~9.6 MHz, and the device appeared completely hung, although formally alive.
  It was fixable only by pulling the battery.

The blob contains two placeholder words: `0xDEADBEEF` — the target value, `0xFEEDFACE` — the
initial value. **Both must be found during substitution, otherwise the blob is not executed
at all.** This is cheap insurance against silent assembly errors.

We intentionally do **not** touch `SDRC_RFR_CTRL`: the counter counts L3 clocks, so at a
higher frequency refresh only becomes more frequent — the safe direction.

### 3.15 GUI vs Headless, and Divider Compensation

A headless run of `mult 96` (+11.5%) passed stably, while the same engine from the
application hung the device as early as `mult 88`. The tempting hypothesis of a "long bypass
window" was **disproven by experiment**: stepping up one tick at a time (each
step individually stable) also hung. Thus, it was not about the duration of a single window, but about
the number of windows — that is, the number of foreign calls that fell into them. This
led to the protected EXEC-patch from Section 3.11.

The second discovery of the same stage: **multiplier 77 (downward) hangs just like 88
(upward).** What breaks is not "too fast", but **any** change to CORE, and what breaks
is specifically rendering. The reason is clear from the architecture: the display is clocked by
`CORE / DSS1_div`, and by raising CORE we dragged DSS along with it. The firmware does not do
this — `dvfs_conf.dll` broadcasts notifications before and after a change, whereas we bypass
this path, so the display driver does not know its clocking has shifted.

The field map of `CM_CLKSEL1_CORE` was deduced from seven setters in `power.dll`
(`0x800AC458..0x800AC598`) — each validates the value, shifts it, and calls
`Modify32`. Masks and shifts are read directly from the code, and permissible values from the
validation jump tables. The layout matched Linux's `cm-regbits-24xx.h`, providing
two independent sources.

Here too is a corrected error, specific to ARM: initially permissible
SSI values were written down as "0,2,3,4,5,7,9". The reason — `pc` in ARM reads as
instruction address **+8**, so for `addls pc, pc, r1, lsl #2` the target is
`address+8 + r1*4`, not `address+4 + r1*4`. Correct: 1,2,3,4,6,8.

**The solution:** the blob writes `CM_CLKSEL1_CORE` together with `CM_CLKSEL1_PLL`, prior to a single
`CLKCFG_CTRL` latch. A single latch captures both multiplier and dividers, so L3 and
DSS never see the new CORE with the old divider.

Selection policy differs for the two domains, intentionally:

- **DSS1 — exact compensation** to the nearest divider. The display does not need a higher
  frequency; it needs its OWN.
- **L3/SDRAM — kept as high as possible** within +15% of nominal. Compensating it
  "to the nearest" would be a mistake: measurement showed 0 errors at +11.5%.

### 3.16 Results and Where the Limit Really Lies

| mult | MPU | L3 (div.) | DSS1 (div.) | Behavior |
|---|---|---|---|---|
| 86 nominal | 330.2 | 165.12 (4) | 110.08 (6) | — |
| **107** | 410.9 **+24.4%** | 164.35 (5) −0.5% | 117.39 (7) +6.6% | stable, calls and WiFi work |
| **111** | 426.2 **+29.1%** | 170.50 (5) +3.3% | 106.56 (8) −3.2% | maximum stable |
| 112 | 430.1 | 172.03 (5) | 107.52 (8) | auto-reboot after minutes |
| 129 | 495.4 | 165.12 (6) **0.0%** | 110.08 (9) **0.0%** | immediate reboot |

Row **129** is decisive here. `129/86 = 1.5`, and both dividers scale
by exactly 1.5 (4→6, 6→9), meaning L3 and DSS1 are set **precisely** to nominal
frequencies — yet the device still reboots immediately.

**Thus the ceiling is neither the memory bus nor the display, but the ARM1136 core itself at nominal
voltage.**

The failure profile confirms this: it holds 426 MHz indefinitely, 430 runs and crashes
after minutes, 445 crashes faster, 495 does not start at all. This is exactly what a
frequency-voltage limit looks like, rather than timing violations: timing violations would result
in data corruption and application hangs, but there are none — the device simply
reboots via watchdog timer.

Real-world application result, not synthetic: **SPmark 3D 62 → 75 fps (+21%)**.

---

## 4. Methodological Lessons

This is the most transferable part of the experience — it is barely about Symbian.

1. **Measure actual work, not the label.** `HAL::ECPUSpeed` lies consistently and
   convincingly. All public "Symbian overclocks" failed precisely here.
2. **Two independent instruments.** A chain of independent `add` instructions (absolute frequency) and an
   ALU loop with data dependency. When both yield the same percentage and it
   matches the calculation `19.2*mult/5` — it is no longer a coincidence.
3. **Network availability is not proof of no reboot.** Direct uptime measurement
   is.
4. **Do not deduce a threshold from two points.** In §21, from two observations ("87 works,
   88 hangs"), it was deduced that "the panel tolerates +1%". Later, mult 107 turned out to be
   stable at +6.6% on DSS1. A limit was seen where probability lay;
   the real cause was in the unprotected EXEC-patch.
5. **A kernel patch must be safe for a foreign caller** — see 3.11.
6. **Rollback must be functionally verified**, specifically after unshadow — 3.12.
7. **A negative result must also be proven.** A scan of 2712 modules (3.8) found
   nothing, closing an entire branch honestly rather than with "it seems there isn't one".
8. **Correct errors in one's own conclusions in the text, rather than quietly rewriting them.**
   In `findings.md`, there are five such corrections, and each is more useful than the assertion it
   replaced. Especially 3.13: the error was not in the fact, but in the **search
   method** — only one addressing mode was searched for.
9. **Verify an address or bitfield from memory of a datasheet against code.** Mask
   `#3` instead of `#7` for `EN_DPLL` was taken from firmware code. The
   `SDRC_ACTIM_CTRLA` offset was taken from memory and remains marked as a guess because it has not
   been verified.

---

## 5. Documentation for TI OMAP 2420

The short truth: **a complete public TRM for the OMAP2420 does not exist.** TI never
published it in open access — documentation went to partners under NDA
(confirmed by specialized forums and the absence of the document in the
ti.com/lit archive). Therefore, real work is done using surrogates, ranked below by
usefulness rather than official status.

### 5.1 Most Useful: Linux Source Code

This is the de facto register reference for the 2420, and it is accurate because it runs on actual hardware.
The v3.7 branch is convenient because it still contains full OMAP2420 support (later parts
were cleaned out), and it is what the references in `findings.md` rely on.

| File | What it provides |
|---|---|
| `arch/arm/mach-omap2/cm-regbits-24xx.h` | CM bitfields — used to verify the `CM_CLKSEL1_CORE` map (§21) |
| `arch/arm/mach-omap2/prm-regbits-24xx.h` | PRM bitfields |
| `arch/arm/mach-omap2/sdrc.h` | SDRAM controller registers |
| `arch/arm/mach-omap2/sram242x.S` | **reference DPLL relock sequence** — primary source for §16 |
| `arch/arm/mach-omap2/opp2420_data.c` | official OPPs: 330 MHz ARM / 660 CORE / 165 SDRC |
| `arch/arm/mach-omap2/clock2420_data.c` | complete clock tree with dividers |
| `arch/arm/mach-omap2/board-n8x0.c` | N800/N810 board — same SoC, only nominally 400 MHz |
| `drivers/mfd/menelaus.c` | TWL92230 PMIC: VCORE control |

Convenient to read via Bootlin tied to the version:
`https://elixir.bootlin.com/linux/v3.7/source/arch/arm/mach-omap2`
Raw file text — via `https://raw.githubusercontent.com/torvalds/linux/v3.7/<path>`.

Specifically regarding voltage (verified by reading `menelaus.c` v3.7): Menelaus I2C address is
`0x72`, registers `VCORE_CTRL1..CTRL5` = `0x02..0x06`, table
`vcore_values[]` maps **1000–1450 mV in 25 mV steps** to 4-bit codes
0–18. Two modes: `menelaus_set_vcore_sw()` writes the level directly into `VCORE_CTRL1`,
`menelaus_set_vcore_hw()` stores floor/roof in `VCORE_CTRL3`/`CTRL4` and enables
hardware switching.

### 5.2 Closest Public TRM by Architecture

**OMAP35x TRM (SPRUF98)** — https://www.ti.com/lit/ug/spruf98y/spruf98y.pdf

Next generation, but PRCM, SDRC/SDRAM controller, DSS, and GPTIMER are architecturally
derivative, with identical register naming (`CM_CLKSEL*`, `CM_IDLEST*`, `PRM_*`,
`SDRC_*`). Read for **understanding the mechanism**; specific addresses and field
layouts there are **different**, and numbers cannot be taken from there. In this project, this rule
was observed: all numbers were taken from firmware code or Linux 24xx headers.

Separately: the OMAP2423 TRM (same 242x line) contains a PRCM section equivalent to
2420 — referenced in §16. It is also not in TI's public archive, but
can be found on mirrors.

### 5.3 The ARM Side

- **ARM1136JF-S TRM, ARM DDI 0211** — developer.arm.com. Pipeline, caches, CP15,
  MMU, `WFI`, `PLD`, barriers. This is what justifies "one cycle per independent
  `add`" in the measuring instrument and `mcr p15,0,r3,c7,c10,4` in the blob.
- **ARM Architecture Reference Manual v6, DDI 0100** — instruction set,
  including the semantics of `pc` as "address + 8", on which the decoding
  of the SSI jump table stumbled (Section 3.15).

### 5.4 An Underrated Source: QEMU

The Nokia N800 model in QEMU (`hw/arm/omap2.c`, `hw/arm/nseries.c`) is effectively
reverse-engineering documentation of the OMAP2420 written as working code: register maps of PRCM,
SDRC, GPTIMER, DSS along with behavior. When one needs to understand what a specific
bit does, it is sometimes faster to read the model than to search for a datasheet.

### 5.5 Overview and Reference Material

- TI product brief: https://www.ti.com/pdfs/wtbu/TI_omap2420.pdf
- TI SDP OMAP2420: https://www.ti.com/pdfs/wtbu/TI_sdp_omap2420.pdf
  (both marketing-level — only for an overall picture of peripherals)
- Lauterbach OMAP2420: https://www.lauterbach.com/supported-platforms/chips/OMAP2420
  (list of peripherals and supported registers, occasionally useful as a map)
- DatasheetArchive: https://www.datasheetarchive.com/TI%20OMAP2420-datasheet.html
- Discussion on documentation availability:
  https://www.edaboard.com/threads/omap2420-datasheet-req.130183/

### 5.6 The Symbian Side

- Public Symbian source code (notably `elf2e32`: `huffman.cpp`,
  `inflate.cpp`) — from which the decompressor port was made (3.5).
- S60 3rd FP1 SDK — user-side headers, `RBusLogicalChannel`, `HAL`, `RTimer`.
  It contains no kernel-side part (3.4).

---

## 6. Project Tooling

| Tool | Purpose |
|---|---|
| `tools/build.cmd` | build → upload over WiFi → run → retrieve log |
| `tools/romtool.ps1` | parsing ROM modules, disassembly, resolving imports across all modules |
| `tools/syminflate.js` | Symbian-deflate (port of `TInflater`/`Huffman` from elf2e32) |
| `tools/build_sis.cmd` | building and signing application SIS |
| `src/ocprobe.cpp` | probe: measurements, patches, relock |
| `ocgui.exe` | on-device application: overclocking and stress tests without PC |

Tooling pitfalls are compiled separately in `state.md` — from the `Int32` trap in
PowerShell to `signsis` without `-s`, which returns 0 and produces an unsigned file.

---

## 7. What Remains Open

1. **Voltage.** Menelaus is accessible, the mechanism is understood (5.1), but not a single
   touch was made. This is the only branch where "reboot restores things to as they were" ceases
   to be complete protection: registers are volatile, but silicon degradation is not.
2. **Instrument divergence.** At mult 96, the bus scaled accurately (+11.5% vs
   calculated +11.63%), while both CPU instruments yielded +8.5%. Hypothesis —
   the governor adjusts the MPU divider upon detecting a higher CORE. Verified by reading
   `CM_CLKSEL_MPU` under load.
3. **`ocgui.exe` unverified on device.** The engine beneath it is proven,
   the interface is not.
4. **`SDRC_ACTIM_CTRLA` `+0x9C`** — offset taken from the datasheet from memory and not
   verified against code. Does not affect overclocking (we do not write timings), but is marked
   as a guess and must remain so until verified.
