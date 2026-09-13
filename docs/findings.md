# Overclocking Symbian 9.2 / OMAP 2420: What Has Been Established

Reference platform — Nokia N95 8GB (RM-320), firmware 35.0.001. Sources: disassembly
of the ROM image from `../eka2l1/data/drives/z/rm-320/` and measurements on live hardware
via `SymbianDeployDaemon`.

This file is updated incrementally; every item is either confirmed on hardware or deduced
from ROM code — guesses are marked explicitly.

> Context and scope — `docs/SCOPE.md`. In brief: this is homebrew on **one's own**
> unlocked device; technical terms below (kernel write, patcher,
> dev-build hash registry) pertain to modifying one's own device, not someone else's.

## 1. Context: All Public Symbian "Overclocks" Are Fake

- Editing `C:\private\10202be9\20001079.txt` (value 332) is an N-Gage config;
  it has nothing to do with frequency.
- "OC patches" for ROMPatcher modify a text string of MHz in a DLL.
- Utilities like Todo Speed / Quokie yield ~1% in benchmarks — an effect
  of RAM freed after restart.

Hardware headroom, however, is real: the N800 on the same OMAP2420 started at
330 MHz and transitioned to stock 400 MHz with the OS2008 update; the N810 has 400 MHz
in production. That is, the silicon is rated for 400 MHz.

## 2. Firmware Contains Full Kernel-Side DVFS

| Module | Role |
|---|---|
| `dvfs_interface.ldd` | sole entry point from user side |
| `dvfs_engine.dll` | governor; policies `AVGn`, `AVGn 2`, `MAW`, `New Thread` |
| `dvfs_hal.dll` | level quantization, reading PRCM |
| `dvfs_conf.dll` | pre/post-change notifications, voltage via Menelaus |
| `dvfs_monitor.dll` | load collection |

All have UID2 = `0x100000AF`, meaning they are kernel-side modules.

## 3. User-Side Access to DVFS: Exists, but Yields No Overclock

The registered channel name is **`dvfs_interface`**. The string `DVFSInterfaceDriver`
inside `.ldd` is the factory name; attempting to open using it gives `KErrNotFound`.
The exact name was obtained by enumerating `TFindLogicalDevice` on the device.
```
DoCreate("dvfs_interface", TVersion(0,0,0)) -> KErrNone
```
The capability set `PowerMgmt WriteDeviceData ReadDeviceData
ReadUserData WriteUserData ProtServ` is sufficient. `GetCaps` returns 64 zero bytes.

### Message Layout (`HandleMsg` @`0x80148698`)

| `iValue` | Action |
|---|---|
| `0x80000000` | close channel |
| `< 0` | `DoRequest`, number = `~iValue` → `0x80148638` |
| `>= 0` | `DoControl` → `0x80148428` |

### `DoRequest(0, status, a1, a2)` → `SetPerformance` @`0x80148460`
```asm
cmp r2, #50          ; r2 = a2. a1 is NOT read at all
> 50  -> ++count; if first -> mov r0,#66 ; call into power.dll
<= 50 -> --count; if it drops to zero -> reverse call
```
This is a **binary latch, not a frequency setter**. The level is hardcoded to constant 66.
Overclocking via this interface is impossible in principle.

Verified on hardware: `reserve(100)` and `release(0)` both return 0,
frequency 322 → 324 → 324 MHz, which is within noise margins.

### `DoControl(1, a1)` — Reading Clock Frequency
```asm
mov r0, #1 ; bl <power.dll 0x800B15DC>   ; GetClockFrequency(MPU)
ldr r1, <3300000> ; b <ekern uidiv>      ; divide
```
Returns `mpu_frequency_hz / 3 300 000`, that is, a percentage of nominal.
Other opcodes return 0.

### Levels Quantize to Two (`dvfs_hal` @`0x80146520`)
```asm
sub r0,r0,#1 ; cmp r0,#100 ; bcc   -> valid range 1..100, otherwise KErrArgument
r2=1: r0 = 50*r2 ; if (perf <= r0) return r0 ; ++r2 ; while (r2 <= 2)
```
`perf 1..50 → 50`, `perf 51..100 → 100`. **Operating points are exactly two.**
The hardcoded value 66 quantizes to 100.

## 4. What Programs the Clock Frequency

Peripheral mapping: **virtual = physical + `0x7E000000`**. Confirmed
by two independent hits: `0xC6008240` → `0x48008240` (CM_CLKSEL1_CORE) and
`0xC6028028` → `0x48028028` (GPTIMER1 TCRR).

`kaomap.dll` is `AsspRegister`, the access primitives themselves:
```
+0xE8  ldr r0,[r0] ; bx lr     Read32
+0xF0  str r1,[r0] ; bx lr     Write32
+0xF8  Modify32 (interrupts disabled)
```
**PRCM is owned by `power.dll`** (codeAddr `0x800A7C98`) — 112 references across the entire
register file `0x48008xxx`. The entire DVFS superstructure only reads.

### Format of CM_CLKSEL1_PLL (`power.dll` @`0x800B1544`)
```
DPLL = sys_clk * mult / (div + 1)
```
| Field | Bits |
|---|---|
| multiplier `mult` | [21:12] |
| divider `div` (divide by `div+1`) | [11:8] |
| `sys_clk` selection | [25:23] → 0 = 19.2 MHz, 2 = 13.0, otherwise 12.0 |

The function first checks `CM_CLKEN_PLL & 3 == 3` (DPLL in lock mode),
otherwise returns 0.

### Remainder of the Chain

- `power.dll` @`0x800B15B0`: core = DPLL × `CM_CLKSEL2_PLL[1:0]`
- `power.dll` @`0x800B15DC`: `GetClockFrequency(id)`; for `id==1` returns
  MPU = `core / CM_CLKSEL_MPU[3:0]`, in hertz

### Map of Levers

| Register | Physical | Field | Action |
|---|---|---|---|
| CM_CLKSEL1_PLL | `0x48008540` | [21:12] / [11:8] | multiplier / divider of DPLL |
| CM_CLKSEL2_PLL | `0x48008544` | [1:0] | core multiplier from DPLL |
| **CM_CLKSEL_MPU** | `0x48008140` | **[4:0]** | **MPU-only divider**; valid 1,2,4,6,8 |
| CM_CLKEN_PLL | `0x48008500` | [1:0] | DPLL mode |
| PRCM_CLKCFG_CTRL | `0x48008080` | [0] | configuration latch |

> **CLARIFIED 2026-08-08.** Field `CM_CLKSEL_MPU` is **[4:0]**, not [3:0]: setter
> `power.dll @0x800AC410` calls `Modify32` with mask `0x1F`. Valid divider
> values are **1, 2, 4, 6, 8**; the jump table @`0x800AC3D0` rejects 0, 3,
> 5, 7. That is, divider 3 does not exist on this SoC, even though the field could hold it.

`dvfs_conf.dll` switches levels by writing `1` (100%) or `0` (50%) to
`0x4805A018` plus a write to Menelaus; reads silicon revision from `0x48014220`, bit 17.

## 5. Absolute Numbers

`dvfs_conf` compares `GetClockFrequency(1) / 3300000` with `100` and with `50`.
Thus **100% = 330 MHz exactly, 50% = 165 MHz**.

An independent measurement on hardware (a chain of independent `add`s, ARM1136 = 1 cycle per
instruction) gave 323–324 MHz assuming 258 cycles per iteration; with
actual ~263 cycles this is indeed 330 MHz. Matches.

**`HAL::Get(ECPUSpeed)` lies**: consistently returns 206438 kHz regardless of
the actual state. Do not rely on it — including in PS1Emulator.

Baseline of instrument (`ocprobe`): ALU 17.05 Mitr/s, memory ~510 MB/s.
Under 30 s of continuous load, the frequency does not change — the governor is already
holding the maximum.

## 6. Where to Look for Overclocking

Two levers, with different risk profiles:

- **`CM_CLKSEL_MPU[3:0]`** divides only the core, leaving L3/SDRAM/DSP untouched. But this is an
  integer divider: if currently /2, the next step /1 doubles the frequency.
  There are no intermediate values — the step is too coarse.
- **Multiplier `CM_CLKSEL1_PLL[21:12]`** provides a fine step (each unit is ≈ +1.9 MHz
  on DPLL), but carries along core, L3, and SDRAM, thus requiring verification of
  SDRC timings.

Before choosing, one needs to **read the current values** of these registers to know
the actual breakdown of `sys_clk × mult / (div+1) / core_sel / mpu_div`. Reading is
kernel-mode; tool candidate is the RomPatcher+ driver.

## 7. Tool for Kernel-Mode Access: `TestPowerAPI`

### RomPatcher+ — The Path Turned Out Expensive

`dumpsis -x RomPatcherPlus_3.1.sisx` yields three drivers: `patcher.ldd`,
`patcherShadow.ldd`, `patcherS3.ldd`. All are compressed with `KUidCompressionDeflate`
(`0x101F7AFC`), and Symbian's deflate is **incompatible with zlib** — a custom
Huffman implementation. `System.IO.Compression.DeflateStream` fails at any
header offset; `petran`/`elftran` accept only PE/ELF as input, not a
ready E32. To take this path, one would have to implement Symbian-inflate.

### A Better Candidate Is Already in ROM

`d_powerapi.ldd` — registered name **`TestPowerAPI`**, 2516 bytes,
uncompressed (ROM image); it is absent from the loaded list, but resides in
`Z:\sys\bin` and is loaded via `User::LoadLogicalDevice`.

The `d_` prefix in Symbian denotes a test driver — these deliberately expose
kernel APIs to the user side.

Imports:

| Target | What it is |
|---|---|
| `kaomap.dll +0xF0` | **Write32** |
| `kaomap.dll +0xF8` | **Modify32** |
| `power.dll` × 9 | notably `0x800AD064` — the same function DVFS calls with id 66 |
| `ekern.exe` × 30 | standard kernel scaffolding |

### Resource Dispatch Table

The dispatcher (`0x80148FF8`) branches on resource index, then each block
parses the subaction in `r0`:

| Subaction | Action |
|---|---|
| 0 | **set** level; value comes from caller in `r3` |
| 1 | read state |
| 3 | another operation (`0x80148EAC`) |

The block for resource **66** (`0x80149018`) is CPU performance, the same id that
`dvfs_interface.ldd` hardcodes as a constant:
```asm
cmp r0, #0
mov r2, r3          ; VALUE FROM THE CALLER
add r1, r4, #88
mov r0, #66         ; resource 66
bl <power.dll 0x800AD0A4>
```
Key point: the path goes **directly into power.dll, bypassing `dvfs_hal`**, so quantization
to 50/100 does not apply here. What limits power.dll itself imposes is unknown,
verifiable only by experiment.

Available resources, judging by constants in blocks: 1, 2, 3, 4, 5, 6, 65, 66, 67,
69, 70.

Separately, opcode 12 (`0x80148EFC`) executes a fixed sequence across
System Control Module registers (`0x480000CC`, `0x480000D0`, `0x480000D4`, `0x480000D8`,
`0x48000270`) with a single value from caller. This is not PRCM, but proves
the existence of raw `Write32` with controlled data.

### Message Encoding (`HandleMsg` @`0x80148FB8`)

This is `DoRequest`, not `DoControl`:
```asm
ldr r0, [r6,#28]     ; a1 = TRequestStatus*
mvn r0, r1           ; r0 = ~iValue = request number = SUB-ACTION
ldr r1, [r6,#32]     ; r1 = a2 = jump table INDEX
cmp r1, #12
addls pc, pc, r1, lsl #2
```
The result is returned via `Kern::RequestComplete` **into `TRequestStatus`**
(`0x8014928C`: `mov r1, r5 ; b <RequestComplete>`), thus
`DoRequest(subaction, status, index)` and then `status.Int()`.

Index table: 0 → `KErrNotSupported`; 1..5 → resources 1..5; 6 → 65;
**7 → 66 (CPU performance)**; 8 → 67; 9 → 69; 10 → 70; 11, 12 → other.

### Correction: The Level Does NOT Come from the Caller

Initially I read `mov r2, r3` in the resource 66 block as "value from caller".
This is a mistake. `r3` is populated earlier from `*(0xC8097150)`, where the driver during
initialization stores a pointer to its own 8-byte object
(`mov r0,#8 ; bl <alloc> ; str r0,[r5]` @`0x80148D8C`). That is, this is a callback
object, and the call is shaped like
`f(id=66, this+88, callbackObj, 0)` — **standard acquire/release of a fixed
resource, without an arbitrary level**.

Thus TestPowerAPI grants access to a wider set of resources than DVFS (65, 67, 69,
70, 1..6), but **does not provide an arbitrary address write primitive**. The only raw
`Write32` with data from caller is `DoControl(12, a1, a2)`, and it writes to
fixed addresses in System Control Module, not PRCM.

### Resource Enumeration: What We Managed to Read
```
LoadLogicalDevice(d_powerapi.ldd) -> 0
DoCreate(TestPowerAPI, v0.0.0)    -> 0
  get(index=0) -> -5     KErrNotSupported, exactly as the code says
  get(index=1) -> -1     KErrNotFound
  get(index=2) -> -1
  get(index=3) -> -1
  get(index=4) -> -1
  get(index=5) -> -1
  get(index=6) -> the request NEVER COMPLETES (resource 65)
```
Index 0 returning exactly `-5` confirms the correctness of the jump table
decoding. Resources 1..5 do not exist on this device.

**Resource 65 (index 6) hangs the request dead.** There is nothing to cancel with: the driver
lacks `Cancel`.

### Consequence: Driver State Survives Daemon Restart

After hanging on index 6, the driver remains with an uncancelable request. The next
attempt to open the channel blocks already **before** any request — judging by
behavior, on a mutex held by that request. That is:

- a timeout must be attached not only to requests, but also to `LoadLogicalDevice`/`DoCreate`
  — yet this cannot be done by standard means, as they are synchronous and without timeout;
- restarting the daemon does not help: state lives in the kernel, cleared only
  by rebooting the device;
- the single-threaded daemon waits for process exit, so a hang in the probe blocks the entire
  delivery channel. The device remains alive — TCP handshake succeeds.

**Conclusion on TestPowerAPI: dead end.** It provides no arbitrary address write
primitive (see above), while the cost of further probing is a device reboot
on every failed index. There is no point in pursuing this path further.

### Practical Lesson: A Hang Blocks the Delivery Channel

The first run of resource enumeration did not return: a request failed to complete,
`User::WaitForRequest` hung dead, and the daemon is single-threaded and waits for
process exit — thus not only the probe hung, but the entire delivery channel. The device
was alive (TCP handshake succeeded), recovered by reboot.

Because of this, in the probe now:

- `TRequestStatus` for the driver is **static**, not on the stack: if a request ever
  completes after our departure, the driver will write to a live address rather than
  someone else's stack frame;
- each request is wrapped with `RTimer` with a timeout (`User::WaitForRequest(s, ts)`),
  returning `KErrTimedOut`;
- after timeout, the channel is **not closed** and further requests are not sent —
  there is nothing to cancel an already pending request with, the driver has no `Cancel`;
- the entire section is enabled only with the `power` argument, so that a normal run remains
  one after which the device is guaranteed to respond.

## 8. Toolchain Limitations

The public S60 3rd FP1 SDK **cannot build kernel-side code**: in
`S60_3rd_FP1\Epoc32` there is neither `include\kernel\` nor `ekern.lib`. There are no tools
to build a custom LDD — a Kernel DevKit is required, which is absent. Hence the bet on
the ready-made RomPatcher+ driver.

## 9. Symbian-inflate: Decompressing RomPatcher+ LDDs (Confirmed)

`patcher.ldd` and siblings are compressed with `KUidCompressionDeflate` (`0x101F7AFC`) —
Symbian's own Huffman deflate, which is **incompatible with zlib**. A verbatim
port of the decompressor from Symbian open-source code (elf2e32:
`huffman.cpp`/`inflate.cpp`) has been implemented — `tools/syminflate.js` (Node).

Confirmed on hardware targets: `file0/1/2` decompress to exactly their
`iUncompressedSize` — **2688 / 2700 / 2780** bytes. The first bytes of
`patcher.ldd` are `E92D4010` (`stmfd sp!,{r4,lr}`), `E3500003` (`cmp r0,#3`):
a valid ARM prologue, meaning decompressed correctly rather than "looks plausible".

Compressed E32 format: uncompressed portion = `oh->TotalSize()` == `iCodeOffset`
(field @0x64), and precisely there (for these LDDs — offset **156**) begins the
deflate stream. `iUncompressedSize` @0x7C is the body length after decompression.
Restored image = uncompressed header + decompressed body; the code section
(`iCodeSize` = `0x948` = 2376 B) lands with `iCodeBase` = `0x8000`.

Artifacts: `out/rp/inflated/patcher.img` (+`Shadow`,`S3`) and
`out/rp/inflated/patcher_code.asm`.

## 10. Write Primitive in `patcher.ldd` (Structure and Key Ordinals Proven)

Disassembly `patcher_code.asm` (code from `iCodeBase = 0x8000`). Imports **45**
functions from `ekern{000a0000}[100041af].exe` (kernel). Proven **directly from code**
(independent of versions):

- **Dispatcher `0x8604`**: `sub r1,#0x64; cmp r1,#5; ldrls pc,[pc,r1,lsl#2]` —
  jump table to channel functions **100–105** (`0x64…0x69`).
- **Routing beyond `0x85a4`**: takes object (import), compares target
  address with `obj[+140]` (base) and `obj[+140]+obj[+244]` (top):
  - `addr ∈ [base, top)` → `0x8530`: page preparation (`0x8410`) and write
    in 32-byte chunks — ROM-shadow path;
  - `addr ∉ [base, top)` → `0x84f4`: **direct write without deviation**.
- Both paths perform the write itself using a single kernel import — **ordinal 725**, with
  three arguments `(dest, src, len)`, under a lock/unlock pair (**ordinals
  401/402**). Three arguments is the signature of raw memory copy, not
  `ThreadRawWrite` (which takes 4 arguments and treats the address as user-space).

**Consequence:** destination address comes from the client; address **outside ROM**
(such as our PRCM `0xC6008540`) takes the direct path and is not rejected. This is
the arbitrary kernel address write primitive that was missing for `CM_CLKSEL1_PLL`
(section 4). len for register write = 4.

### Resolving Ordinals via Real `ekern.exe` from Dump (Confirmed from Code)

`ekernu.def` from SymbianSource turned out to be from a later kernel (ordinals shifted,
imports incoherent). Instead, the export directory of the **real**
`ekern.exe` from the ROM dump (`../eka2l1/data/drives/z/rm-320/sys/bin/ekern.exe`) was parsed:
UID1 `1000007A`, `iCodeAddress` = `0x8006A000`, `iExportDirCount` = **808**,
`iExportDir` = `0x8009D9DC`. Export dir is an array of 32-bit addresses; ordinal `N`
is at entry `[N-1]`. Ordinal 725 (`< 808`) is within bounds.

| Ordinal | Address | Function (deduced from code) |
|---|---|---|
| **725** | `0x8006F800` | **`memmove(dest, src, len)`**. `cmp r2,#0; bxeq lr`; overlap analysis (`subnes r3,r0,r1; cmphi r2,r3`); for `len≤15` — jump table of byte-by-byte copy (`sub pc, ip, r2, lsl #3`); for `len>15` — word-loop `0x8006f944` (`pld`/`str`). Returns `dest` (r0 untouched). |
| **401** | `0x80070E60` | acquire scheduler lock: global `[0x8007087c]`, field `[+304]`, operates on `[+48]/[+52]` |
| **402** | `0x80070EC0` | unlock: decrement nesting counter `[+48]`, check reschedule flag `[+52]`, conditional wakeup |
| 248 | `0x80073A0C` | trivial getter `return *(global 0x8007397c)` (current object/thread) |

The resolution is self-consistent: 401/402 are adjacent ordinals (addresses 96 bytes apart) and
form a clean lock/unlock pair; an index shift by one would not yield this.
This confirms the correctness of export table decoding at the same level as
"two independent addresses on known registers" in section 4.

**Closed:** ordinal 725 is `memmove(dest,src,len)`, a clean CPU store without
MMU tricks or cache barriers. len for register write = 4. Three arguments and
copy semantics confirm: this is raw memory copy, not
`ThreadRawWrite`. Destination address outside ROM (`0xC6008540`) takes the direct path
`0x84f4` and is not rejected — the arbitrary kernel address write primitive is proven
from beginning (client) to end (str in kernel).

### Full Map of patcher.ldd Channel and Two Limitations for Our Case

All 6 channel functions parsed (dispatcher `0x8604`, `r1-100`, jump table) and all
30 import stubs resolved against the export table of real `ekern.exe`
(stub→ordinal→address). Key ones:

| patcher Stub | Ordinal | ekern | Role |
|---|---|---|---|
| `0x87C4` | **725** | `0x8006F800` | memmove — write itself |
| `0x883C` | **401** | `0x80070E60` | lock (before write in `0x84f4`/`0x8530`) |
| `0x87E4` | **402** | `0x80070EC0` | unlock (after) |
| `0x8854` | 248 | `0x80073A0C` | ROM region object getter (`[+140]`=base, `[+244]`=top) |
| `0x87CC` | 238 | `0x8007828C` | page preparation (loop in `0x848c`) |
| `0x87D4` | 303 | `0x800765D4` | page size |
| `0x8804` | 639 | `0x8006ED0C` | client descriptor parser (a2 = data, not address) |

Channel functions: 100 — page preparation/validation; 101/102/104 — bulk write
via router `0x85a4`; **105 — single write** `write(dest=*a1, src=data from
descriptor a2, len=*a2 & 0x0FFFFFFF)`; 103 — fixed getter (ord 360),
without arguments.

This definitively confirms §10, but reveals **two limitations specifically for writing to
PRCM** (both — directly from code, not a guess):

1. **No read primitive.** Not a single channel function reads an arbitrary address:
   105 takes `src` only from client descriptor (via ord 639), 103 is a
   fixed getter. memmove is symmetric, but the channel never permits substituting
   `src` = arbitrary kernel address. Thus reading current `CM_CLKSEL1_PLL` /
   `CM_CLKSEL2_PLL` / `CM_CLKSEL_MPU` with this driver **is impossible**, and without this
   a safe read-modify-write of a bitfield is impossible (a blind write of an entire
   32-bit register would clobber adjacent fields).

2. **Wrong access size for MMIO.** For our `len=4`, memmove takes the
   `len≤15` branch — which is **four separate `strb`s**, not one `str`. Writing a PRCM register
   with four byte-wide accesses is undocumented and risky access:
   some OMAP2420 CM registers reliably accept only 32-bit access, whereas
   byte-wide access produces 3 intermediate register states. memmove was designed for patching
   *code bytes* in ROM, where this is valid, — not for programming device
   registers. Forcing the word path (`len>15`) for a 4-byte register is impossible.

**Consequence:** `patcher.ldd` provides arbitrary write to kernel memory, but as a tool
for programming PRCM it is **unsuitable** — neither reading current state nor performing
a proper 32-bit write is possible. We do not close the branch as a failure, but for overclocking
we need a primitive with `Read32`/`Write32`/`Modify32` of proper width
(like kaomap `+0xE8/+0xF0/+0xF8`, which are imported specifically by `TestPowerAPI`, — but that
branch is a dead end for another reason, §7). The search for such a primitive remains open.

Regarding MMIO mapping (the original question of the item): router `0x85a4` indeed sends an address
outside ROM via direct path `0x84f4` without rejection, and kernel-mode `str` via kaomap
mapping `phys+0x7E000000` reaches the device (proven by the operation of kaomap Write32 for
PRCM). That is, the *mapping* permits writing to MMIO; what blocks is specifically the width of memmove
(item 2 above) and the lack of reading (item 1).

Hardware left untouched.

Analysis artifacts: `out/asm/ekern.asm` (full disassembly, 56854 lines),
`out/asm/ekern.bin` (body without 120-byte header).

## 11. Search for MMIO Primitive Across the Entire ROM: Negative Result

Scanned all **2712** modules in the ROM dump for imports of kaomap primitives
(`Read32` `0x800A1C40`, `Write32` `0x800A1C48`, `Modify32` `0x800A1C50` —
opcodes verified: `ldr r0,[r0]` / `str r1,[r0]` / RMW with interrupts disabled).
They are imported by **24 modules**; of these, only LDDs have a user-facing side:
`d_display`, `d_powerapi`, `custom`, `secldd`, `sosadlserver`, `xbus`.

For each LDD, the address source (`r0`) was classified at each call site of
Read32/Write32/Modify32: PC literal (fixed base) or register argument.
Manual verification of "suspicious" ones (`mov r0,rN`) showed that `rN` is always
an internal driver base (a walking pointer across its peripheral register block),
not an address from a client:
- `d_display` / `sosadlserver`: `r5 = *literal` (the same display-polling
  code, `sub r4,r5,#20; tst r0,#16`);
- `custom`: `r0 = *literal` (reading peripheral ID/revision);
- the rest are base+offset from hardcoded constants.

**Conclusion: not a single driver in ROM exposes arbitrary MMIO address write/read
to the user side.** This is expected — production drivers do not expose raw MMIO, while
the only test ones (`d_*`) operate on hardcoded addresses. Artifacts: `out/asm/`
(`d_display.asm`, `custom.asm`, `sosadl.asm`).

## 12. How Firmware Controls Clock Frequency at Runtime (Reorientation)

> **CORRECTED 2026-08-07 — see §17.** The main conclusion of this section
> ("there is no validated runtime relock path in the firmware") is **false**. It
> relied on an inventory of only `kaomap Write32` calls and only **full**
> 32-bit address literals. In reality, `power.dll` addresses part of PRCM and all of SDRC
> via a **direct `ldr`/`str` pair by base+offset, bypassing kaomap**, and precisely
> there lies the full sequence of bypass → IDLEST polling → relock → DLL relock.
> The text below is retained as-is — within its scope it is correct — but it must be
> read together with §17.

Analyzed `power.dll` (base `0x800A7C98`, 145 calls to Write32). Key fact
**directly from code**:

- Among all 145 Write32 calls, **not one is to `CM_CLKSEL1_PLL` (`0xC6008540`)** —
  the DPLL multiplier/divider. Domain/peripheral dividers are written: `0xC6008200/8204/
  8210/8214/821C` (CLKSEL for domains), `0x8300/8310`, `0x8410`, `0x82A0/82A4`,
  `0x83C8`, plus numerous `*_FCLKEN/ICLKEN` (`018/028` in blocks `C6018/A/C/E`).
- `CM_CLKSEL1_PLL` in power.dll is only **read**: getter `0x800B1544` (§4) and
  init function `0x800A94xx`, which checks the div field (bits [11:8]) and if
  necessary performs Modify32 of neighboring `0xC6008530` — that is, boot-config validation,
  not setting a new frequency.
- Runtime setters of power.dll take **values from arguments** and program
  specifically dividers: e.g. `0x800AC698` writes a 3-bit field in `0xC6008340` from
  values 1..4; `0x800AC6CC` latches `PRCM_CLKCFG_CTRL[0]=1`.

**Consequence (strategy reorientation).** The DPLL multiplier is set once at
boot; runtime DVFS only scales **dividers** down from the 330 MHz ceiling
(§5 — the governor already holds maximum, that is, minimum divider). Hence two
mutually exclusive paths for overclocking, both running into the same blocker:

1. **Reduce the divider** (`CM_CLKSEL_MPU[3:0]` or core) below current. But
   if MPU is already at minimum — there is no headroom; one must **read** the current
   divider, and we lack a read primitive (§10, §11).
2. **Raise DPLL multiplier** (`CM_CLKSEL1_PLL`) with relock. ~~There is no validated
   runtime path for this in the firmware~~ — **false, see §17**:
   a bypass→relock path exists in the firmware (`power.dll @0x800A7F6C`), lacking
   only the write of the new multiplier itself. The rest of the paragraph is also obsolete: this
   path does not need a kaomap primitive, because power.dll writes to MMIO there via
   **direct `str`**, not via kaomap.

That is, both paths are blocked by the lack of proper user-side MMIO read/write.
This is not a dead end for the project, but an exact formulation of what
is truly missing now: **not "what to write" (known), but a working channel into the kernel with
proper access width and reading.** Further candidates — section in
`state.md`.

Artifacts: `out/asm/power.asm` (11639 lines), `out/asm/power.bin`.

## 13. Synthesis: Patch power.dll CODE, Not MMIO (The Path Forward)

Across the entire dump, the boot multiplier of DPLL is not programmed by Symbian code: only power.dll
references `CLKSEL1_PLL(v)`, and only by reading; physical `0x48008540` is nowhere
to be found (§12). Thus it is set by the pre-Symbian bootloader or raw bootstrap outside
`sys/bin`. A "clean" patch of a frequency table is impossible — there is no table.

However, the two blockers (lack of reading; memmove writes in bytes — §10, §11) are eliminated
in a single move if we **patch power.dll code rather than MMIO**:

- The code of power.dll (`0x800A7C98..0x800B3298`) resides in the **ROM region**, which is precisely
  the `[base, top)` range that patcher processes via shadow path `0x8530`
  (§10). Patching code is a write into **normal ROM-shadow memory**, where byte-by-byte
  memmove is correct (it was designed for this). The access width issue concerned
  only direct writes to MMIO — here it does not exist.
- Register access itself is performed by power.dll using its own **correct** kaomap
  primitives (`Read32`/`Write32`/`Modify32`, 32-bit, Modify32 with interrupts
  disabled). We only change *what* and *where* it reads/writes.
- A trigger from user side already exists: `DoControl(1)` on `dvfs_interface` calls
  `GetClockFrequency` @`0x800B15DC` (§3), both addresses being in the patchable
  region.

**Two steps, in order of increasing risk:**

1. **READ (safe first touch of the hardware).** Patch the code so that a
   user-reachable call (e.g. `DoControl(1)` or an unused opcode) returns the raw
   value of `CM_CLKSEL1_PLL` / `CM_CLKSEL_MPU` instead of the derived frequency. Reading
   MMIO is safe, leaving frequency untouched. This (a) tests the entire patcher chain
   end-to-end for the first time and (b) resolves the lack of reading — providing the actual
   `mult/div/core_sel/mpu_div` layout, without which choosing a lever is blind.
2. **WRITE.** Knowing current values and target, patch the code so that a
   user-reachable call performs a proper 32-bit DPLL relock (writing
   `CLKSEL1_PLL` with new multiplier → `CLKEN_PLL` → latching `CLKCFG_CTRL[0]`)
   by the hands of power.dll itself. patcher in this process touches only code bytes, never MMIO.

**Open items needed before the first touch:**
- Exact format of the patcher client call (descriptor a2 in func 105, §10) —
  in order to actually apply the patch.
- Whether patcher's shadow path properly prepares I-cache/pages for executable code of
  power.dll (not just data) — otherwise the patched code will not take effect.
- Choice of patch location: fit within existing bytes or place into a code-cave and
  make a branch.

This turns existing tools into sufficient ones; we do not touch hardware until step 1
(READ patch) is fully designed on paper.

## 14. patcher.ldd Client Invocation Format (Proven from Code)

The channel registers as **`RomPatcherPlus.Driver`** (factory `DPatcherDevice`,
channel `DPatcherChannel`). Dispatcher `0x8604` resides in channel vtable (`0x8908`),
meaning invocation is `RBusLogicalChannel::DoControl(func, a1, a2)`, `func` 100..105
(`sub r1,#100; cmp #5; ldrls pc,[pc,r1,lsl#2]`). The driver dereferences
arguments `a1/a2` **directly** — DoControl is synchronous, kernel runs in client context,
so user pointers are accessible without `kumemget`.

**func 105 — single write** (`0x8658`), argument flow proven:
- `dest = *a1` (first word at `a1` = target address);
- `src  = a2->Ptr()` (via ord 639 = `TDesC8::Ptr()`, decodes descriptor type);
- `len  = a2->Length()` (`*a2 & 0x0FFFFFFF`, length from descriptor header);
- if `*a1 == 0` — returns 0 (no-op).

That is, **`a1` = pointer to target address**, **`a2` = descriptor with bytes
to write**. Client (user-side):
```cpp
RBusLogicalChannel ch;                       // opened on "RomPatcherPlus.Driver"
TUint32 addr = 0x800B15DC;                    // power.dll code address (ROM)
TBuf8<64> patch;  patch.Copy(bytes, n);       // new bytes (the patch's ARM code)
TInt r = ch.DoControl(105, &addr, &patch);    // dest=addr, src=patch, len=n
```
Route by address: power.dll (`0x800A7C98..0x800B3298`) is in ROM, so the router
sends to **shadow path** `0x8530` (prepares pages ord 240, writes in 32-byte
chunks via memmove under lock ord 401/402). This is indeed the proper path for patching ROM code.

**Open item (hypothesis, verified by READ patch):**
- Neither write path performs an explicit `Cache::IMB` after memmove. I-cache
  coherency for patched code should be provided by shadow-page remap in ord
  240. If not — patched code will not take effect until a cache flush. The READ patch will
  show this: worked → coherency exists.
- Exact values of object `base/top` (ord 248) are not known statically; if power.dll
  were unexpectedly outside `[base,top)`, the write would take direct path `0x84f4` without shadow and without
  coherency — also to be revealed on the READ patch.
- Capability set for opening channel (from E32 header of `patcher.ldd` or
  by trial); delivery daemon registers its own dev build in the local
  hash registry (`C:\sys\hash`) — standard homebrew mechanism of this unlocked
  device, through which its loader recognizes the dev build with its caps.

## 15. READ Patch: Design (Deduced from Code; NOT Verified on Hardware)

The goal is to obtain raw reading of PRCM registers from user side without having a read primitive,
via kernel code patching (strategy §13) using the patcher tool (§14).

**Patch target:** `dvfs_interface.ldd` @ `0x80148410`. This is the body that DoControl(1)
invokes to calculate the percentage (`GetClockFrequency(1)/3300000`). Sole caller —
DoControl(1) @`0x8014843c` (verified: grep for `0x80148410` yields a single bl).

> **CORRECTED 2026-08-08.** Previously stated here: "patch is isolated: affects only
> DoControl(1), which in stock system nobody except our probe calls."
> **This is false, and the error cost two device reboots.** The assertion
> applied to callers of the *internal body* inside `.ldd`, but I generalized it
> to the entire channel. A ROM scan shows that the channel name `dvfs_interface` exists as
> a UTF-16 string (`_LIT` form for `DoCreate`) in **two more system modules**:
> `mmrmserver.dll` (Multimedia Resource Manager) and `fmradiotunercontrol.dll`.
>
> Consequence: while the READ patch is applied, any call of theirs to `DoControl(1, a1)`
> with non-zero `a1` executes `ldr r0,[r1]` at that address **in kernel mode**.
> Both observed reboots occurred when the device sat idle with
> the patch left behind — precisely when the resource manager polls state.
>
> Causality is not definitively proven (in DLLs data references go through
> relocations and do not resolve in raw disassembly), but the assumption of
> isolation is disproven, and that is sufficient.
>
> **The baseline for comparison is important.** The device was reflashed with stock latest
> firmware a few days before these experiments and was completely stable prior to them;
> RomPatcher writes nothing to flash. That is, blaming reboots on "old hardware with
> modified firmware" is invalid — prior to our patches there were none.
>
> **Fix in code (proven on hardware 2026-08-08):** the engine restores
> the original body words (`ocengine.cpp`, `KDvfsOriginal`) after **every**
> operation, rather than at session end.
>
> The first rollback attempt wrote 6 words (24 bytes) and **silently failed** —
> after which the device rebooted a third time. Reason: 24 bytes take the
> `memmove` branch for `len>15`, whereas the verified path is exactly 16 bytes, matching the patch
> itself. Words 5 and 6 of the body are untouched by the patch, so restoring them is unnecessary.
>
> Rollback is now **verified functionally**: stock `DoControl(1, 0)` has known
> behavior — it returns percentage of nominal (§3), whereas the READ patch on
> `a1=0` returns exactly 0. On hardware after rollback, **100** was obtained, meaning
> the kernel again contains original code. Reading restored bytes directly is impossible:
> the read primitive IS those bytes.

**Patch (16 bytes, overwriting `0x80148410..0x8014841F`):**

| Address | Bytes (LE) | Instruction |
|---|---|---|
| `0x80148410` | `00 00 51 E3` | `cmp r1, #0` |
| `0x80148414` | `00 00 91 15` | `ldrne r0, [r1]` |
| `0x80148418` | `00 00 A0 03` | `moveq r0, #0` |
| `0x8014841C` | `1E FF 2F E1` | `bx lr` |

`r1` at entry to `0x80148410` = `a1` from DoControl (dispatcher `0x80148438`:
`mov r1,r2`). Guard `a1==0` prevents NULL dereference (existing `PerfPercent()`
calls `DoControl(1,0)` — after patching it will safely return 0). Words `0x80148420/
424` remain (dead, unreachable after `bx lr`). r4 does not need saving —
DoControl itself executes `stmdb {r4,lr}`.

**Trigger (user-side):** `RDvfs::DoControl(1, (TAny*)0xC6008540)` → returns raw
`CM_CLKSEL1_PLL`; similarly `0xC6008544` (CLKSEL2_PLL), `0xC6008140`
(CLKSEL_MPU), `0xC6008500` (CLKEN_PLL). Decode using formula of §4 — and for the first time
learn the actual `mult/div/core_sel/mpu_div` breakdown.

**Order of increasing risk (critical — this is the first patch of kernel code):**

0. **SELF-TEST (zero risk to kernel).** Prior to any kernel patch —
   verify the patcher channel and func 105 format by writing to **our own user buffer**:
   `DoControl(105, &addr, &desc)`, where `addr` = address of own buffer. Non-ROM address
   takes direct path `0x84f4` (memmove), kernel code untouched. Proves:
   patcher loads, channel opens (caps/version), func 105 convention
   is correct, write works. Only after success — step 1.
1. **READ patch** (this section): apply 16 bytes, read registers.
   Concurrently confirms hypotheses of §14 (I-cache coherency of shadow-remap;
   power.dll/dvfs_interface within `[base,top)` of patcher).
2. **WRITE patch** (later): DPLL relock by the hands of power.dll.

**Delivery prerequisites (discovered during design):**
- `patcher.ldd` is absent from ROM (Z:) — it is a file from RomPatcher+ SIS. For
  `LoadLogicalDevice("patcher.ldd")` one must deploy the **original** (compressed)
  `out/rp/file0` to `E:\sys\bin\patcher.ldd` + register its SHA-1 in
  `C:\sys\hash` (the same mechanism as for ocprobe; loader will decompress
  Symbian-deflate on its own). Our inflated `.img` is for analysis only, does not go to hardware.
- Capabilities required for opening channel — from E32 header of `patcher.ldd`.
- `build.cmd` currently deploys only ocprobe; for test must additionally deploy
  patcher.ldd (+hash) — extend deploy.

Hardware left untouched until step 0 (self-test) is passed.

### Verifying READ Patch on Hardware — 2026-08-07

The sequence from §15 was executed on RM-320. First `ocprobe patcher selftest`:
original compressed `out/rp/file0` deployed as `E:\sys\bin\patcher.ldd` with
SHA-1 in `C:\sys\hash\patcher.ldd`; `LoadLogicalDevice -> 0`,
`DoCreate("RomPatcherPlus.Driver", v0.0.0) -> 0`, and
`DoControl(105, user-buffer, 4 bytes) -> 0` changed the buffer to exactly the expected
bytes. This confirmed delivery, capabilities, func 105 format, and direct path without
touching ROM.

Next, `ocprobe patcher read` repeated self-test and wrote 16 bytes at
`0x80148410`; `DoControl(105, READ patch, 16 bytes) -> 0`. Invoking
`dvfs_interface::DoControl(1, addr)` thereafter returned:

| Register | Value |
|---|---|
| `CM_CLKSEL1_PLL` | `0x00056400` |
| `CM_CLKSEL2_PLL` | `0x00000002` |
| `CM_CLKSEL_MPU` | `0x00000002` |
| `CM_CLKEN_PLL` | `0x0000000F` |

Decoding `CM_CLKSEL1_PLL`: `sys_clk=19.2 MHz` (selector 0), `mult=86`,
`div=4`, yielding DPLL = `19.2 * 86 / (4+1) = 330.24 MHz`. From the code formula of
power.dll `core = DPLL * CM_CLKSEL2_PLL[1:0]`, and then
`MPU = core / CM_CLKSEL_MPU[3:0]`, values `2`/`2` yield MPU = 330.24 MHz.
This matches independent NOP measurement 322–325 MHz (error in assumption of
cycle count) and stock `DoControl(1) -> 100` before patching.

**New constraint.** `mpu_div=2` is not headroom for cautious overclocking:
transitioning to 1 would double MPU to approximately 660 MHz. Thus the next practical
candidate is only a small step of DPLL, e.g. `mult 86 -> 87` (+1.16%), but not
before runtime relock sequence, voltage, and SDRC limits are fully proven.
The patch was runtime-only; reboot restores ROM, and before a new
experiment it must be performed to eliminate the READ patch and LDD state.

## 16. WRITE Patch: Previous Model Was Incomplete (Do Not Touch Hardware)

After rebooting the device, an external reference source was analyzed:
`arch/arm/mach-omap2/sram242x.S` Linux v3.7 for **OMAP2420**. This is not proof
of RM-320 behavior in itself, but the code directly implements the PRCM of this SoC and
corrects a dangerous assumption in the previous plan §13/§15.

`omap242x_sram_set_prcm` performs the full sequence:

1. saves registers and preloads its own critical code into I-cache;
2. puts `CM_CLKEN_PLL[1:0]` into fast-relock bypass (`2`) and waits for
   `CM_IDLEST_PLL[1:0] == 1`;
3. only in bypass writes new `CM_CLKSEL1_PLL` and `PRCM_CLKCFG_CTRL = 1`;
4. puts DPLL into lock (`3`), waits for `CM_IDLEST_PLL[1:0] == 2`;
5. updates SDRC refresh, puts DLLA/DLLB into lock-mode and waits not less than
   0x400 L3 cycles for DLL relock.

A separate function `omap242x_sram_reprogram_sdrc` also performs a memory barrier,
adjusts refresh, and calls `voltage_shift` before raising frequency. Therefore
the previous abbreviated formula "`CLKSEL1_PLL` -> `CLKEN_PLL` -> `CLKCFG_CTRL`"
was incorrect in order and omitted SDRC/voltage/SRAM context.

Additional external confirmation: `opp2420_data.c` Linux v3.7 sets the fastest
official point as **330 MHz ARM**, `CORE_CLK = 660 MHz`, `SDRC_RFR_CTRL_165MHz`.
Thus `mult=87` is not a stock OPP and cannot be the first touch merely because
the step is small.

**Conclusion.** The READ patch proved transport, ROM-shadow, and register reading, but
is not proof of DPLL write safety. The next work is to find/design specifically
SRAM-safe execution, read SDRC parameters, and establish power limits;
prior to that, no WRITE patch and no command shall be added on hardware.

> **CLARIFIED 2026-08-07 (§17).** The sequence requirement itself is confirmed, and
> now not merely from an external source: the same structure (self-refresh → bypass →
> IDLEST polling → WFI → relock → IDLEST polling → DLL relock) was found **in
> `power.dll` of this device**. But the formulation "find/design SRAM-safe
> execution" is no longer the sole option: a ready-made critical section already exists in
> firmware, and embedding into it is a cheaper and safer path than a custom
> sequence. The SDRC parameter is also no longer unknown: `SDRC_RFR_CTRL` = 
> `0x0004DA01`. The main point remains unchanged: **do not perform WRITE**, do not
> touch hardware.

External sources for reproduction: Linux v3.7
`arch/arm/mach-omap2/sram242x.S`, `arch/arm/mach-omap2/opp2420_data.c`; TRM
OMAP2423 (section 5.10.15, PRCM portion equivalent to OMAP2420 in this product line).

## 17. The Firmware HAS an SRAM-Resident PRCM/SDRC Critical Section (Corrects §12, §16)

The motive for verification was doubt regarding the origin of the conclusion in §16 (it relied on
a retelling of Linux code). Verification confirmed the external source, but revealed that
**our own conclusion "there is no validated runtime relock path in the firmware" is
false**.

### Why This Was Missed: Two Methodological Errors

1. The inventory of §11/§12 counted only `kaomap Read32/Write32/Modify32` calls.
   However, `power.dll` touches part of PRCM and **all** of SDRC via **direct `ldr`/`str`**,
   bypassing kaomap. Example — `str r2,[r1,#1280]` into `CM_CLKEN_PLL`.
2. The ROM scan searched for **full** 32-bit register address literals. `power.dll`
   addresses these blocks as **base+offset**, so `CM_IDLEST_PLL` (`+0x520`) and all
   SDRC registers did not appear in the scan at all.

### New Bases (Proven by Offset Matches to Known Registers)

| Base | Physical | Proof |
|---|---|---|
| **SDRC `0xC6129000`** | `0x68009000` | six offsets matched exactly: `+0x60` DLLA_CTRL, `+0x64` DLLA_STATUS, `+0x68` DLLB_CTRL, `+0x70` POWER_REG, `+0xA4` RFR_CTRL_0, `+0xD4` RFR_CTRL_1 |
| CM `0xC6008000` | `0x48008000` | `+0x500` CLKEN_PLL, `+0x520` IDLEST_PLL, `+0x540` CLKSEL1_PLL, `+0x544` CLKSEL2_PLL |
| CONTROL `0xC6014000` | `0x48014000` | `+0x204` silicon revision (cf. §4) |

Notice: SDRC mapping is **not** `+0x7E000000` (which would yield `0xE6009000`).
For this L3 block the offset is different. The rule of §4 applies to the `0x48xxxxxx` window, not globally.

Scan of all **2868** dump modules for literal `0xC6129000`: **the sole module is
`power.dll`, two occurrences**. That is, all OS interaction with the SDRAM controller consists of
exactly the two functions below.

### Chain: SRAM Mapping → Code Copying → Execution from SRAM

Everything is proven directly from code.

**1. Constructor `0x800AB24C`** (sole caller — from `0x800A8560`, i.e.
boot initialization):
```asm
bl  0x800B19C8                  ; object allocation
str <0x800B31A8>, [r4]          ; vtable
...
mov r2, #4096                   ; size
ldr r1, <0x4029E000>            ; PHYSICAL address of the OMAP2420 internal SRAM
add r0, r4, #12
bl  0x800B1BA0                  ; -> ekern ordinal 60 @0x800779E4 (HW chunk mapping)
ldr r0, [r4, #12] ; ldr r0,[r0,#36]   ; linear address of the mapping
ldr r1, <0x800A7F6C>            ; SOURCE: the routine code
mov r2, #1024
str r0, [r4, #16]               ; store the SRAM linear address
bl  0x800B1C08                  ; -> ekern ordinal 725 @0x8006F800 = memmove
```
Nearby error string in the literal pool — `"...for PowerDown"`.

**2. Runtime invocation `0x800AB714`** — sleep depth selector:
```asm
blx r2                    ; vtable+28 -> sleep depth, in r9
cmp r0, #2
bcc 0x800AB740            ; shallow idle -> ordinary kernel idle
ldr r0, [r6, #16]         ; SRAM linear address
bl  0x800A80B0            ; 0x800A80B0 == "bx r0"  -> EXECUTION FROM SRAM
```
**3. The procedure itself `0x800A7F6C..0x800A809C`** (+ literal pool up to `0x800A80AC`,
together **324 bytes**):
```asm
mcr p15,0,r3,c7,c10,4          ; DSB
[SDRC+0x70] |= 0x48            ; SDRC_POWER: self-refresh on idle
ldr r2,[CONTROL+0x204] ; >>28 ; cmp #5 ; bne skip     ; silicon revision
ldr r2,[CM+0x540] ; >>8 ; &0xF ; cmp #8 ; ble skip    ; div from CLKSEL1_PLL
  [CM+0x500] = (v & 0xFC) | 1  ; CLKEN_PLL -> low-power bypass
  poll [CM+0x520] & 3 == 1     ; IDLEST_PLL: bypass active
skip:
mcr p15,0,r2,c7,c0,4           ; WFI
  [CM+0x500] |= 3              ; relock DPLL
  poll [CM+0x520] & 3 == 2     ; IDLEST_PLL: locked
[SDRC+0x60] = [SDRC+0x68] = 0x770E      ; DLLA/DLLB_CTRL
bic #4 ; write back                      ; -> lock mode
delay 1000 ; ldr [SDRC+0x64] & 0xFF00    ; DLLA_STATUS
ldmia sp!, {...,pc}
```
This is **the same structure as in `sram242x.S`** (§16), only without writing the new
multiplier: `CLKSEL1_PLL` is only read here to decide whether to enter bypass.

**Relocatability — verified:** in the range `0x800A7F6C..0x800A80AC` there is not a single
external `bl`, only PC-relative literal loads and local branches.
The procedure is self-contained. The rest of the 1024 copied bytes is reserve;
external `bl` calls exist there (`0x800A80EC` onwards), and entering them from SRAM is not permitted.

**Guards.** On this device `div=4` (§15), so `cmp #8; ble` **skips the entire
bypass/relock block**. In reality, only SDRC_POWER, WFI, and DLL relock execute.
The bypass branch is dead code on RM-320; it is intended for silicon revision 5 and
configurations with `div > 8`.

### SDRC refresh: Value Known

`power.dll @0x800A7CAC` writes constant **`0x0004DA01`** to `SDRC_RFR_CTRL_0` (`+0xA4`)
and `_1` (`+0xD4`) → `ARCV = 0x4DA = 1242`, which is ≈ **165 MHz**.

**Clarified 2026-08-08 (from hardware).** There are actually two values, and the firmware
switches them along with OPP. Both exist as literals in `power.dll` — `0x0004DA01`
(@`0x800A7F24`) and `0x00032601` (@`0x800A7F28`), — and both were read from the live
register at different moments:

| Reading | `CM_CLKSEL2_PLL` | `SDRC_RFR_CTRL_0` | State |
|---|---|---|---|
| under load | 2 | `0x0004DA01` (ARCV 1242) | upper OPP, L3 165 MHz |
| at idle | 1 | `0x00032601` (ARCV 806) | lower OPP |

Two independent fields (`core_sel` and `ARCV`) converged on the exact same conclusion,
so this is not a reading artifact. Practical consequence for code: **an instantaneous reading of
`CM_CLKSEL2_PLL` is deceptive** — catching the device at idle yields 165 MHz CPU
instead of 330. `COcEngine::Prepare` therefore takes a series under load and takes
the maximum.

`SDRC_ACTIM_CTRLA_0` (assumed `+0x9C`) also changed between readings
(`0x8255B485` -> `0xBADDC4C6`). The offset of this register I took from the memory datasheet
and **did not cross-check against code**, so for now this is a guess — it does not affect
overclocking because we do not write SDRC timings.
Matches `SDRC_RFR_CTRL_165MHz` from `opp2420_data.c` and our reading of
`core_sel=2` (DPLL 330 → L3/CORE 660 → SDRAM 165). Thus the device sits precisely at the
stock upper OPP **330 / 660 / 165 MHz**, and the item "read SDRC parameters"
from the four prerequisites of §16 is **closed**.

### What This Changes

**Confirmed:** the requirement for the full sequence is not a Linux-ism. The same
structure exists in the firmware of this device. The §16 prohibition on WRITE remains in force.

**Disproven:** the assertion of §12/§16 that there is no validated runtime path and that
one must design it from scratch. It exists, along with SRAM mapping and DLL relock.

**New, precise blocker.** Copying to SRAM occurs **once at boot**
(sole constructor invocation). Thus a ROM-shadow patch applied at runtime to
`0x800A7F6C` **does not affect** the code that actually executes from SRAM. Our
verified patching tool strikes the wrong copy.

### Candidates Going Forward (HYPOTHESES, Unproven)

1. Obtain the linear address of the SRAM copy and patch **it**, rather than ROM. The READ patch
   (§15) already provides reading of arbitrary kernel addresses, while patcher func 105 provides writing;
   for normal memory byte-by-byte memmove is correct, so the limitation of §10 item 2
   **does not apply** here. Unknown: where the global pointer to the object resides
   (`[obj+12]` chunk, `[obj+16]` linear address). Look in power.dll data.
2. Trigger. The procedure executes only on deep idle (depth ≥ 2), which
   the OS chooses on its own. There is no controllable user trigger — this is a separate
   unresolved issue, and it makes any WRITE here poorly observable.
3. Voltage (Menelaus) and limits — remain as they were, unclosed.

**Do not touch hardware.** This work is static, solely from the ROM dump.

## 18. OVERCLOCK ACHIEVED: +5.8% on Hardware, Confirmed 2026-08-07

DPLL multiplier raised 86 → 91 at runtime on live RM-320. Confirmed by two
independent instruments, reproduced twice, fully reversible.
```
before   : CLKSEL1=00056400 mult=86  DPLL=330240 kHz   328 MHz (NOP)  17.34 Mitr/s
after    : CLKSEL1=0005b400 mult=91  DPLL=349440 kHz   347 MHz (NOP)  18.35 Mitr/s
restored : CLKSEL1=00056400 mult=86                    328 MHz (NOP)  17.34 Mitr/s
```
| Instrument | Before | After | Change |
|---|---|---|---|
| Chain of independent `add`s (absolute frequency) | 328 MHz | 347 MHz | **+5.7%** |
| Data-dependent ALU loop | 17.34 Mitr/s | 18.35 Mitr/s | **+5.8%** |
| 2 MB cache bypass (L3/SDRAM bandwidth) | 520 MB/s | 550 MB/s | **+5.7%** |
| Calculation `19.2*91/5` vs `19.2*86/5` | — | — | +5.81% |

The third row is particularly important: it proves that this is **not an overclock of the core alone, but
a rise in CORE_CLK**, from which the entire system scales. Core and bus increased by
the exact same percentage.

The three numbers converge within 0.1 percentage points. Sustained run under load: 5 rounds of ALU +
integrity check of 1 MB on each round — **0 corrupted words out of 5 MB**.
Reverting the multiplier yields exactly baseline 328 MHz / 17.34 Mitr/s, meaning the effect is not
drift or a measurement artifact. `HAL::ECPUSpeed` in this process, as always, reported
an unchanging 206438 kHz — yet another confirmation that it cannot be relied upon.

### How This Was Done: Three Primitives Assembled from Foreign Code

None of them required either a Kernel DevKit or writing to flash.

1. **Reading an arbitrary kernel address.** ROM-shadow patch of 16 bytes into the body of
   `dvfs_interface.ldd DoControl(1)` @`0x80148410` → `cmp r1,#0; ldrne r0,[r1];
   moveq r0,#0; bx lr` (§15).
2. **Executing arbitrary kernel code.** The exact same patch, but `bx r1` (4 bytes).
   The body of `DoControl(1)` is exactly 6 words `0x80148410..0x80148427`; dispatcher
   starts at `0x80148428` and remains intact. Entry contract: `r0` = this,
   `r1` = `r2` = `a1`, `r3` clobbered by dispatcher, `lr` = `0x80148440`. That is,
   exactly one 32-bit argument.
3. **Staging ground outside SDRAM.** SRAM chunk mapped by `power.dll` at boot
   (§17): phys `0x4029E000`, 4 KB. On this device linear address is
   `0xCB202000`; the first 1024 bytes are occupied by firmware, we write at offset
   `+0x800`. Patcher writes there via direct path (address outside ROM), and byte-by-byte
   memmove is correct here because this is normal memory rather than MMIO — limitation §10 item 2
   is lifted.

**Confirmed along the way:** SRAM chunk is writable, executable, and
I-cache coherent — blob `mov r0,#0x5A; bx lr` written there and
invoked returned `0x5A`. Separately verified calling stub in ROM
(`power.dll @0x800A8264`, `mov r0,#3; bx lr`) — returned 3.

### Relock Sequence (272 bytes, executed from SRAM)

Order follows `omap242x_sram_set_prcm` and the idle procedure of `power.dll`
@`0x800A7F6C` (§16, §17):
```
IRQ+FIQ off
CM_CLKEN_PLL[1:0] = 2        fast-relock bypass
wait for CM_IDLEST_PLL[1:0]==1
CM_CLKSEL1_PLL = new         only bits [21:12] change
PRCM_CLKCFG_CTRL = 1         wait for self-clear
CM_CLKEN_PLL[1:0] = 3        lock
wait for CM_IDLEST_PLL[1:0]==2
SDRC DLLA/DLLB relock        (status&0xFF00)|(ctrl&0xF)|4, then a settle delay
IRQ restore
```
Three details, each of which would have broken the result:

- **`EN_DPLL` is bits [1:0], not [2:0].** Bits [3:2] are APLL 54 MHz; mask
  `#7` would disable it together with DPLL. Mask `#3` was taken from power.dll code
  (`and r2,r2,#0xfc`).
- **No external `bl` and no access to SDRAM in the critical window.**
  Constants come from blob's own literal pool (`.ltorg` at end), so `ldr [pc,#off]`
  remains valid after copying. Stack is touched only before and after the window.
- **All three waits are bounded by counter** (`0x10000`). Interrupts are
  disabled, so an infinite loop would mean a hard hang requiring battery
  pull. On timeout, multiplier is not written at all, and DPLL is forced
  back into lock — i.e., rollback.

`SDRC_RFR_CTRL` is deliberately **not** touched: counter counts L3 cycles, so at
higher frequency refresh only becomes more frequent, which is the safe direction.

### Headroom: mult 96 Also Stable (+11.5%)

| mult | DPLL | MPU (measurement) | ALU | Change | Memory |
|---|---|---|---|---|---|
| 86 (stock) | 330.2 MHz | 328 MHz | 17.34 Mitr/s | — | — |
| 87 | 334.1 MHz | 331 MHz | 17.53 Mitr/s | +1.15% | 0 errors |
| **91** | 349.4 MHz | 347 MHz | 18.35 Mitr/s | **+5.8%** | 0 of 5 MB |
| 96 | 368.6 MHz | 366 MHz | 19.35 Mitr/s | +11.5% | 0 of 5 MB |

At each step both instruments yielded the same percentage, and each matched
the calculation `19.2*mult/5`. Restoration after each run gave exactly baseline
numbers.

**Where we stopped and why.** Beyond this lies not the CPU limit, but the SDRAM limit: `CORE` and `L3`
grow along with DPLL, so at mult=96 the bus is already 184 MHz versus stock 165
(`SDRC_RFR_CTRL_165MHz` in the official OPP table, §16). Failure there has a different
character than a hang: not a crash, but **silent data corruption** that can
reach flash through the filesystem cache. The 5 MB test does not rule it out —
it merely failed to find it. Therefore further steps (mult 104 ≈ 400 MHz, stock in
N810 on the same SoC) were deliberately not attempted: the goal was achieved, and the price of error here is
the only thing the project forbids itself.

**Working recommendation — mult 91.** Headroom to SDRAM limit is twice as large as
in 96, and the goal (≥5%) is fulfilled.

### Minor Observation

`CM_CLKSEL2_PLL` was read alternately as `2` and `1` between runs. Thus the DVFS
governor indeed moves core_sel at runtime (cf. §12, where it is stated that dividers
are scaled). This does not affect measurement — both instruments measure work, not the register.

## 19. Why the Device "Rebooted": I-Cache and Shadow Page

Four "reboots" during work on 2026-08-07/08. Investigated to completion;
the cause turned out not to be what I thought twice in a row.

### These Were NOT Kernel Reboots

`User::NTickCount()` counts nanokernel ticks from system startup and is cleared
only by a full reboot. Measurement on hardware after all four events: **43404 s
≈ 12 h**, exactly matching the time of the last genuine
reboot (with logo) that occurred the previous day.

**Therefore the kernel did not crash once.** What appeared as a reboot was
a restart of the Symbian shell after the demise of a user-space system process.
Collateral confirmation: the delivery daemon did not come up on its own after these events, because
its startup entry runs on boot rather than on shell restart.

**Lesson in methodology.** Initially I "proved" the absence of a reboot via network
availability. This is not proof: the device manages to come back up between checks. Direct
uptime measurement is proof, and it should have been put in place from the very beginning. The probe
now prints `UPTIME:` as the first line of every run.

### True Cause: Overwriting Bytes Does Not Remove the Patch

Patching the body of `DoControl(1)` is applied by writing via patcher, and **not a single path in
patcher performs `Cache::IMB`** — this was recorded as an open hypothesis back in
§14. Consequence: the patched function had already executed, its instructions reside in
I-cache, and writing *data* does not invalidate instruction cache. The processor continues executing the old,
patched copy — "restoration" occurs only in memory.

Observed directly, using the exact same test on the same hardware:

| Method of removing patch | `DoControl(1,0)` after it | What this means |
|---|---|---|
| overwrite original bytes | **0** | patch ALIVE (0 is patch's response to `a1=0`) |
| + `FreeShadowPage` | **50** | stock code, actual percentage of nominal |

Thus after every run of ours there remained an **active** hooked
function in the system. The `dvfs_interface` channel is opened not only by our code (§15:
`mmrmserver.dll`, `fmradiotunercontrol.dll`), and their invocation of `DoControl(1, a1)`
with non-zero `a1` received garbage from `ldr r0,[r1]` instead of a percentage. A system
process crashed — the shell restarted.

### Solution: Patcher Has a Standard Opcode for This

Disassembly of `patcher_code.asm` yielded the full picture of the shadow mechanism:

| Address | ekern Ordinal | Role |
|---|---|---|
| `0x8410` | **240** `AllocShadowPage` | allocates RAM page for ROM page; called automatically on every write to ROM region |
| `0x848c` | **238** `FreeShadowPage` | frees it, restores mapping to ROM |
| **channel opcode 100** (`0x8694`→`0x8724`) | — | `DoControl(100, &addr, &len)` → `0x848c` across range |

That is, the standard way to clean up after oneself was in patcher the entire time. `FreeShadowPage`
discards the RAM copy and restores mapping to true ROM — together with proper
cache maintenance, which writing by itself does not provide.

`COcEngine::EndPatchSession()` now performs both steps: first overwriting
original bytes (so page contents are stock should unshadow ever fail
to work), then `UnshadowKernelPage()`. Result verification is **after**
unshadow, not after write.

### Bisection Proving This

The probe has levels (`patcher level 0..3`), each leaving behind exactly one more thing.
After the fix, all four are clean, uptime continuous between them:

| Level | What it does | Result |
|---|---|---|
| 0 | load `patcher.ldd`, open channel | clean |
| 1 | + func 105 write to own buffer | clean |
| 2 | + READ patch, read, complete removal | clean, `restore check -> 50` |
| 3 | complete `Prepare` | clean, `restore check -> 50` |

**Collateral conclusion, important for the future:** none of this wrote to flash or
damaged the device — all four events were resolved by shell restart. The project's
safety model (everything runtime) held even though the specific
implementation was defective.

## 20. Overclocking with Live Display: SDRAM in Transition Window

A headless run of `patcher oc 96` (+11.5%) ran stably, but the exact same
engine from the application hung dead already at `mult 88`. The difference was not in the multiplier,
but in **load on SDRAM during the transition window**.

During bypass, CORE equals `sys_clk`, meaning L3 and SDRAM drop from 165 MHz to
~5 MHz — and remain so the entire time while DPLL is locking. The larger the multiplier step, the
longer the lock time, hence the longer the window. A headless run survived this because
the display controller was not running; in the application, DSS continuously reads the frame
buffer from SDRAM, and the transition breaks it.

**Fixes (both — from vendor code of this same SoC):**

1. **SDRAM into self-refresh before window.** `SDRC_POWER |= 0x48` (bit 6
   `SRFRONIDLEREQ`, bit 3 `EXTCLKDIS`) — exactly what and exactly where
   the idle procedure of `power.dll` @`0x800A7F6C` does it (§17). I copied the PLL
   sequence, but missed its SDRC preparation — this was an oversight, not an unknown.
2. **Hardware rollback inside critical section.** If DPLL fails to lock,
   the blob returns to bypass, writes the **original** multiplier, and locks on
   it — all while interrupts are still disabled. Without this, DPLL remained in bypass,
   MPU dropped to ~9.6 MHz, and the device appeared completely hung, although
   formally alive; recoverable only by pulling battery.

The blob now has **two** placeholder words: `0xDEADBEEF` — target value,
`0xFEEDFACE` — original. Both must be found during substitution, otherwise the blob
is not executed at all. Status bits: 1 = bypass timeout, 2 = CLKCFG timeout,
4 = lock timeout (rollback attempted), 8 = rollback failed as well.

### Main Cause of Hangs in GUI: EXEC Patch Was Dangerous for Foreign Calls

The hypothesis "long bypass window" was **disproven by experiment**: stepping up
by one notch at a time (each step being the same that works stably on its own) also hung
the device. Thus the issue is not the length of a single window, but the **number of windows**.

The cause is structural. The EXEC patch made the body of `DoControl(1)` equal to `bx r1` —
a branch to the address in the argument. But the `dvfs_interface` channel is opened not only
by us (§15): in an application with a live UI, `mmrmserver.dll` and the window
server are active. Any `DoControl(1, a1)` of theirs that fell into our window forced
the kernel to branch to a **foreign** address. Headless this did not happen because those
clients were not running; in GUI it did happen, and stepped climbing merely multiplied
the number of attempts.

**Fix:** the patch now branches **only to the single expected address** (our
slot in SRAM), returning 0 to all others — just as harmlessly as the READ patch on
`a1=0`:
```asm
ldr  r0, [pc, #12]     ; allowed address (patched in from outside)
cmp  r1, r0
bxeq r1                ; branch ONLY if the address is the same
mov  r0, #0
bx   lr
.word <slot>
```
Exactly 6 words = 24 bytes, which is the exact size of original body. Patch bytes
are taken from assembly template (`OcExecPatchStart/End`), rather than an array of
constants: instruction encoding is computed by assembler, not human.

**Lesson:** when hooking a kernel function, one must assume it will be called not only
by your code. A patch must be safe for any caller — otherwise the window
of exposure will sooner or later act as a landmine. This is the exact same mistake as in §15, just
from the other side: there I mistakenly assumed the function was needed by no one, here — mistakenly
assumed I would manage to remove the patch before a foreign call.

### Fixed in Application Alongside

- **Full screen.** `CAknAppUi::ENoScreenFurniture` removes status pane and
  softkey pane; rect is taken from `ApplicationRect()` rather than `ClientRect()`
  (the latter returns area beneath furniture that is no longer there).
- **Screen rotation.** `HandleResourceChangeL(KEikDynamicLayoutVariantSwitch)`
  repositions rect. Without this, in landscape a vertical rectangle remained —
  precisely why results were readable normally in only one orientation.
- **Results were not visible.** Layout required ~360 px with ~268 available:
  tests ran, but the log was drawn beyond screen boundary. Now small
  `AnnotationFont` and explicit clipping by `yMax`.
## 21. Display is clocked from CORE: field map of CM_CLKSEL1_CORE

A symmetrical observation on the device settled the question: multiplier **77 (downwards) hangs
just the same** as 88 (upwards). Therefore the issue is not "too fast" —
**any** change of CORE breaks, and specifically rendering breaks. The firmware does not do this:
`dvfs_conf.dll` sends out notifications before and after the change (§2), while we bypass
this path, so the display driver does not know that its frequency has shifted.

### Field map (inferred from power.dll setters)

`power.dll @0x800AC458..0x800AC598` — seven functions, each validates a value,
shifts it, and calls `Modify32(CM_CLKSEL1_CORE, mask, value)`. Masks and shifts
are read directly from code:

| Mask / shift | Bits | Field | Allowed values (from validation) |
|---|---|---|---|
| `0x1F` / 0 | [4:0] | L3 | **not 0, 3, 5, 9, 18** (corrected, see below) |
| `0x60` / 5 | [6:5] | L4 | 1, 2 |
| **`0x1F00` / 8** | **[12:8]** | **DSS1** | not 18 |
| `0x2000` / 13 | [13] | DSS2 | 0, 1 |
| `0xF8000` / 15 | [19:15] | VLYNQ | not 5 |
| `0x1F00000` / 20 | [24:20] | SSI | 1,2,3,4,6,8 |
| `0xE000000` / 25 | [27:25] | USB | 1, 2, 4 |

The layout matches the Linux headers for OMAP2420 (`cm-regbits-24xx.h`) —
two independent sources, just as for peripheral mapping in §4.

> **CORRECTED 2026-08-15, and this mistake was costly.** For L3 it was stated here
> "not 18", that is, "everything allowed except 18". In reality, the validator @`0x800AC424`
> reads as follows:
>
> ```
> 800ac424: cmp   r1, #5
> 800ac42c: beq   reject            ; 5
> 800ac430: bgt   0x800ac444
> 800ac434: cmp   r1, #0
> 800ac438: cmpne r1, #3
> 800ac43c: bne   accept            ; i.e. 0 and 3 - reject
> 800ac444: cmp   r1, #9
> 800ac448: cmpne r1, #18
> 800ac44c: bne   accept            ; i.e. 9 and 18 - reject
> ```
>
> Rejected are **0, 3, 5, 9, 18** — the exact same list by form as in the DSP
> divider setter (0, 5, 9, 16, 18). Consequence of the error: `CompensatedCore()`
> picked `l3div=5` on multipliers 99..111, the device **does not accept such a write
> at all**, and `CM_CLKSEL1_CORE` remained completely unchanged — along with the DSS1 field,
> which by itself was valid. That is, divider compensation on these multipliers
> **never worked**, and from the outside it looked as if someone was resetting it. Full
> breakdown with measurements — §26.

> **CORRECTED.** Initially I listed the allowed SSI values as "0,2,3,4,5,7,9".
> This is a branch table decoding error: in ARM, `pc` is read as the instruction
> address **+8**, so for `addls pc, pc, r1, lsl #2` @`0x800AC520` the target
> equals `0x800AC528 + r1*4`, not `0x800AC524 + r1*4`. Correct:
> **allowed 1,2,3,4,6,8; rejected 0,5,7.** Practical consequence below —
> SSI is not accurately compensated on either 96 or 129.

Our value `0x04360644` breaks down as follows: L3=4, L4=2, **DSS1=6**, DSS2=0,
VLYNQ=6, SSI=3, USB=2. That is, the display is clocked at `CORE/6` = **110.08 MHz**, and
by raising CORE we were raising it along with it.

### ~~Why +1 worked and +2 did not~~ — CONCLUSION ERRONEOUS, see §22

> **CORRECTED 2026-08-08.** The reasoning below ("the panel tolerates
> approximately +1%, and in the range 87..91 there is nothing to compensate with") is **disproven
> by measurements on the device.** At mult 107 the device is completely stable, while DSS1 there
> receives **+6.6%** — more than +5.8% at mult 91, which hung. Hence the display
> tolerance is at least +6.6%, and the 87/88 boundary had a different reason: most likely
> the unprotected EXEC-patch (§20), the failure of which was random, while I mistook noise
> for a threshold. A classic mistake: seeing a boundary where there was probability.
> The text is retained as an example of how one should not derive a threshold from two points.

### Initial (erroneous) reasoning about display tolerance

The divider is an integer, so at mult 87..91 it must remain 6:

| mult | DSS1 | deviation | on device |
|---|---|---|---|
| 87 | 111.36 MHz | +1.2% | works |
| 88 | 112.64 MHz | +2.3% | hangs |
| 91 | 116.48 MHz | +5.8% | hangs |

That is, the panel tolerates approximately +1%, and there is nothing to compensate with in this range —
a divider of 6.13 does not exist. This is not our defect, but the limit of hardware resolution.

### Solution: compensate dividers in the same commit

The blob now writes `CM_CLKSEL1_CORE` **together** with `CM_CLKSEL1_PLL`, before a single
`CLKCFG_CTRL` commit. This is important: a single commit latches both the multiplier and
the dividers, so L3 and DSS never see a new CORE with the old divider.
The rollback path likewise restores both registers.

The selection policy is different for the two domains, and intentionally so:

- **DSS1 — exact compensation** to the nearest. The display does not need a higher
  frequency, it needs its OWN.
- **L3/SDRAM — keep as high as possible** within +15% of stock. Compensating it
  "to the nearest" would be a mistake: at mult 96 this would lower the memory **below**
  stock, whereas the measurement showed 0 errors at +11.5%.

What comes out of this (verified by calculation, mult 96 — on the device):

| mult | CPU | L3 (div.) | DSS1 (div.) |
|---|---|---|---|
| 86 stock | 330.2 | 165.1 (4) | 110.08 (6) |
| 96 | 368.6 | 184.3 (4) **+11.6%** | 105.3 (7) **-4.3%** |
| 100 | 384.0 | 153.6 (5) -7.0% | 109.7 (7) **-0.3%** |
| **129** | **495.4** | **165.1 (6) 0.0%** | **110.08 (9) 0.0%** |

**mult 129 is special:** `129/86 = 1.5`, and both dividers scale by exactly
1.5 (4→6, 6→9). This is a "pure CPU overclock": +50% on the core, while the bus and display
remain **exactly** at stock frequencies. Whether silicon can withstand 495 MHz is a separate
question (N810 on the same SoC is 400 MHz stock), but from the clocking perspective the configuration
is ideal.

Verified on the device (mult 96): `CLKSEL1_CORE 04360644 -> 04360744`,
`status=0`, memory +11.0%, 0 corrupted words.

### Open issues

CPU instruments consistently give a **smaller** gain than calculated: at mult
96 this is +8.3..8.9% instead of +11.6%, whereas the bus scales accurately (+11.0%).
Two independent CPU tools agree with each other, so this is not a measurement error.
The most probable hypothesis is that the DVFS governor moves the MPU divider upon seeing a higher
CORE. Verified by reading `CM_CLKSEL_MPU` under load.

## 22. FINAL RESULT: +29% stable, limit is MPU silicon

Measurements were performed by the user on the device after all corrections in §19-21. It was he
who found the key configuration: for the bus to remain stock at `l3_div=5`,
CORE = `165.12 x 5` = 825.6 MHz, that is, CPU ~412 MHz, which is mult 107-108.

| mult | MPU | L3 (div.) | DSS1 (div.) | Behavior on device |
|---|---|---|---|---|
| 86 stock | 330.2 | 165.12 (4) | 110.08 (6) | — |
| **107** | 410.9 **+24.4%** | 164.35 (5) −0.5% | 117.39 (7) +6.6% | **stable; calls and WiFi work** |
| 108 | 414.7 +25.6% | 165.89 (5) +0.5% | 103.68 (8) −5.8% | stable |
| **111** | 426.2 **+29.1%** | 170.50 (5) +3.3% | 106.56 (8) −3.2% | **maximum stable** |
| 112 | 430.1 +30.2% | 172.03 (5) +4.2% | 107.52 (8) −2.3% | autoreboot after some time |
| 116 | 445.4 +34.9% | 178.18 (5) +7.9% | 111.36 (8) +1.2% | autoreboot |
| 129 | 495.4 +50.0% | 165.12 (6) **0.0%** | 110.08 (9) **0.0%** | **instant reboot** |

### Application results (SPmark 3D, on device)

```
stock mult 86 : 62 fps
      mult 107: 72 fps   (+16%)
      mult 111: 75 fps   (+21%)
```

The phone interface and emulation "work significantly faster" (user
observation). This is no longer synthetic: the gain is visible in real 3D graphics, meaning
both the GFX domain, DSS, and memory scale together with the core.

### Where the limit actually lies

The **mult 129** row is decisive here: L3 and DSS1 are set **exactly** to stock
frequencies (since `129/86 = 1.5`, and both dividers multiply by exactly 1.5), yet the device
still reboots instantly. Therefore:

**The ceiling is not the memory bus and not the display, but the ARM1136 itself at stock voltage.**

The failure profile confirms this: it holds 426 MHz (111) indefinitely, 430 MHz (112)
works and crashes after minutes, 445 MHz crashes faster, 495 MHz does not start at all.
This is exactly what a frequency-voltage limit looks like, rather than a timing violation: with
SDRC timing violations there would be data errors and application hangs, but the
user specifically noted that there are no hangs in applications — the device simply
reboots via watchdog timer.

For reference: N810 on the same OMAP2420 runs at 400 MHz stock. Our stable
426 MHz is higher than the official point of this product line, and this is consistent with the silicon
being designed for 400 MHz with headroom (§1).

### What yielded the result

| Step | Contribution |
|---|---|
| Protected EXEC-patch (§20) | removed main source of hangs in GUI |
| Divider compensation (§21) | allowed keeping L3 near stock and going higher |
| User's idea: `l3_div=5` at mult 107-108 | found operating point with zero bus shift |
| Hardware rollback in the blob (§20) | made failed attempts harmless |

### Recommendation

> **CLARIFIED 2026-08-08 (from device).** Below it previously stated "mult 107 for daily
> use, 111 — close to the limit". That wording merely reflected the
> testing order: **107 was the first successful point**, and at the time of writing
> there was no long-term experience with 111 yet. Since then, the user has been keeping **mult 111 in
> regular daily use, and it is completely stable.**
>
> Hence 111 is not an "extreme case for the maximum", but an operating point. What remains
> in effect: 112 reboots after minutes, meaning the margin above 111 is less than
> one multiplier step, and in hot weather or during long 3D load it does not exist by
> definition. The failure here is benign — watchdog timer, not data
> corruption.

**mult 111** — operating point: +29.1% on core, bus +3.3%, confirmed by long-term
daily use. **mult 107** — if margin is needed: +24.4%, bus within
0.5% of stock, also fully verified.

For **other people's** devices in a public build, leave 107 by default: there is neither
long-term verification nor guarantee of the same cooling conditions.

Everything is runtime: reboot returns the device to stock state, nothing
is written to flash.

## 23. DSP domain: how firmware calculates C55x frequency (inferred from ROM)

The prompt was a task from neighboring project `../TMS320C55x test` —
`docs/TASK_DSP_CLOCK.md`. There, ~50 MMAC/s was measured and a hypothesis was proposed that C55x
is clocked at half of nominal. Below is only what follows from ROM code;
on the device this has **not** been verified yet.

**`CM_CLKSEL_DSP` = `0x48008840` (virt. `0xC6008840`).** Address is not from datasheet:
`power.dll` itself loads it from literal `@0x800B16F4`, and as a second source —
setters in `ehwaomap.pdd`, which perform `str r1,[r0,#0x840]` from the PRCM base.

**The firmware calculates "DSP frequency" as `CORE / CM_CLKSEL_DSP[3:0]`.** Ordinal 60
of `power.dll` @`0x800B1620`:

```
bl   0x800b15b0      ; CORE, kHz
cmp  r5, #1          ; argument != 1 -> 0
ldr  r0, [pc,#176]   ; 0xc6008840
bl   0x800b1c60      ; read32
ands r1, r0, #15     ; MASK 0xF, i.e. [3:0]
bl   0x800b1c18      ; CORE / field
mul  r4, #1000, r0
```

Units aligned down to the number: `0x800B1544` returns DPLL in **MHz** (sys_clk from
`[25:23]` in units of 0.1 MHz: 0→192, 2→130, other→120; then `×mult`,
`/(div+1)`, `/10`) = 330; `0x800B15B0` multiplies by `CM_CLKSEL2_PLL[1:0]` and by
1000 → CORE = 660000 kHz; `ord60(1)` with field 6 gives 660000/6·1000, and
`ehwaomap.pdd` divides by 1000 and puts it into `_BRIDGEINIT_DSP_FREQ` → **110000**.
This is exactly the bimodal value (110000/220000) that the client observed.

**Consequence — field ambiguity is resolved.** From the PDD setters alone it is not visible which
field clocks C55x: they touch `[7]`, `[6:5]`, `[12:8]`, and `[13]`, and do not touch the low
field at all. But the firmware calculates DSP frequency specifically from the low field,
so the C55x divider is that field. The fact that setters do not touch it merely means that
firmware does not alter it at runtime (it is set at boot).

**Disproven hypothesis (mine, not the client's).** I assumed that `ord60` might
return the interface clock `[6:5]`, in which case the core would sit at 220, and 110 would be
a reporting artifact. Code rules this out: `[6:5]` does not enter the formula at all.

**A detail with consequence.** `ord60` masks `0xF`, even though the datasheet gives the field 5 bits
`[4:0]`. With a divider ≥16 the stock reporter would lie — and `_BRIDGEINIT_DSP_FREQ`
is the primary indicator for the client. Irrelevant for values 3 and 6.

**Writing to `CM_CLKSEL_DSP` does not require the `PRCM_CLKCFG_CTRL` latch:** setter in PDD
@`0x801AE900` is a regular `ldr`/`bic`/`orr`/`str`, does not commit.

**The WRITE-patch variant `strne r2,[r1]` from the task is impossible** — proven from
`dvfs_interface` dispatcher @`0x80148428`:

```
cmp  r1, #1
mov  r3, #0        ; a2 arrives in r3 and is destroyed HERE
stmdb sp!, {r4,lr}
bne  0x80148444
mov  r1, r2        ; r1 = a1
bl   0x80148410    ; body: r1 == r2 == a1
```

In the patched body `a2` never arrives, and `r2` equals `r1`, so such a
patch would write the address into itself. If it comes to writing — perform it via
the existing EXEC path (blob in SRAM + EXEC-patch, jump permitted to exactly one
address), rather than a new patch form: WRITE-patch would bring back danger for foreign
clients of `dvfs_interface` in a worse form than READ.

## 24. DSP domain on device: divider is already stock, and why "110 MHz" is a mirage

`patcher dsp` run on RM-320 (2026-08-11, device idle):

```
CM_FCLKEN_DSP @c6008800 = 00000401
CM_ICLKEN_DSP @c6008810 = 00000002
CM_IDLEST_DSP @c6008820 = 00000000
CM_CLKSEL_DSP @c6008840 = 000023c3
fields: DSP[3:0]=3 [4:0]=3  IF[6:5]=2  bit7=1  IVA[12:8]=3  bit13=1
```

12 snapshots over 3 s — **not a single change**, one single value `0x23C3`.

**DSP divider = 3, meaning C55x is at 220.16 MHz — at nominal.** The neighboring
project's hypothesis "DSP sits at half frequency" is **disproven on the device**.

### 24.1. Where "110 000" came from — and why this is not about the DSP

The chain closes without a single assumption:

1. `ord60(1)` calculates `CORE / CM_CLKSEL_DSP[3:0]` (§23);
2. `CORE = DPLL × CM_CLKSEL2_PLL[1:0]`;
3. and `CM_CLKSEL2_PLL[1:0]` **itself fluctuates between 1 and 2** — this has already been documented in
   this project (§12, §18) and encoded in `Prepare()`, which precisely for this reason takes
   a series under load and takes the maximum, rather than trusting an instantaneous reading.

Thus at `sel2 = 2` the result is `660480/3 = 220160`, at `sel2 = 1` —
`330240/3 = 110080`. Exactly those two groups that the client saw, with the exact same
ratio. The DSP divider did not change **a single time** during this.

This also matches their own control observation (`DSP_GUIDE.md`
§14.3): throughput between "110" and "220" runs **did not change**. Their
conclusion in §14.3 was correct, whereas the later §15.4 ("C55x is almost always at
110 MHz") was not; §15.4 must be considered disproven.

**Consequence for their numbers:** MAC/cycle was calculated at 220 MHz, and this assumption
was confirmed. Recalculation is not needed.

### 24.2. Firmware does not alter this register at all

The literal `0xC6008840` appears across the entire ROM image (60 MB) **exactly twice**,
both times in `power.dll`: in the setter constant pool and in the `ord60` pool. The addresses
of both setters (`0x800AC59C` in power.dll, `0x801AE900` in ehwaomap.pdd) **do not occur
a single time** in the image — neither as a `BL` target nor in export tables.
That is, both functions are dead code. Therefore, the divider sampled at start can be taken
as reference: by itself it will not "drift".

### 24.3. Working path of modification: call stock setter rather than writing register

Implemented in `COcEngine::ApplyDspDiv`. The trampoline in SRAM slot sets `r1 =
divider` and performs `blx` to `0x800AC59C` **directly in ROM** (without copying: all internal
`bl` calls of the function remain valid). The EXEC-patch, as always, permits a jump
to exactly one address.

`Modify32` (@`0x800A1C50`) disassembled and determined safe to call from our
context — it is a leaf function without locking:

```
mrs ip,cpsr / orr r3,ip,#0xC0 / msr cpsr   ; IRQ+FIQ off
ldr r3,[r0] / bic r3,r3,r1 / orr r3,r3,r2 / str r3,[r0]
mcr p15,0,r3,c7,c10,5                      ; DMB
msr cpsr,ip / bx lr
```

i.e. `Modify32(addr, clearMask, setBits)`.

**We take guard rails from the firmware.** The setter rejects values 0, 5, 9, 16, 18
(returns `-6`); accepts the rest 1..31. We additionally cut off >31: setter lets them
through, while `Modify32` masks `0x1F`, so 32 would write 0 into the field and halt
the DSP clock.

### 24.4. MAIN POINT: writing to register is NOT ENOUGH, commit is needed

Verified directly on the device. The first version of the trampoline only performed a setter
call:

```
ApplyDspDiv(2) -> r=0, firmware setter rc=0
after:  div=3      <-- the register did NOT change
MISMATCH: register did not take the value
```

The setter reported success, but the field remained old. The new value is latched
only by writing `1` to `PRCM_CLKCFG_CTRL` (`+0x80`) — the same latch with which
this project's relock blob commits `CLKSEL1_CORE` and `CLKSEL_MPU` (§21). The stock
setter does not do this — and this is most likely the reason why it is not called
anywhere: in its existing form it simply does not work.

After adding the commit (under disabled interrupts, because the DVFS governor
uses the same latch for `CM_CLKSEL2_PLL`):

```
before: div=3 -> DSP 220160 kHz
ApplyDspDiv(2) -> r=0, firmware setter rc=0
after:  div=2 -> DSP 330240 kHz
register confirmed by read-back
after 3 s: div=2 (system alive)
```

**The DSP divider changes at runtime: 3 → 2, that is, 220.16 → 330.24 MHz (+50%).**
Neither CORE, nor SDRAM, nor display, nor USB are touched. Rollback on exit
confirmed by a separate run: `0x23C3` in place.

### 24.5. Divider 2 cannot be LEFT IN PLACE: two reboots out of two

Chronology, in order of execution:

| # | what was done | result |
|---|---|---|
| 1 | `sweep.ps1 -Runs 3`, divider 3 (stock) | min 1 136 260 us, spread 5%, **ok=3/3** |
| 2 | `patcher dsphold 2` — divider 2 **left** in system | applied, confirmed by reading |
| 3 | same `sweep.ps1 -Runs 3` at divider 2 | **device lost, reboot** |
| 4 | `patcher dspset 2 120` — divider 2 **inside process**, 120 s | 24 samples of 5 s, `div=2` whole time, **device alive**, rollback on exit |
| 5 | `patcher dsphold 2` — divider 2 again **left** | applied, process exited cleanly |
| 6 | nothing was done, device idle | **reboot by itself** |

**The DSP node has nothing to do with it.** Step 6 is the exact same crash as step 3, but without
any DSP load: the base image was not loaded, the task was not created,
the benchmark was not launched. Thus the hypotheses of §24.5 in the earlier revision — "C55x cannot
hold 330 MHz" and "DSP/BIOS with incorrect timings" — both **fail to explain step
6**, because nothing was executing on the DSP there.

Continuation, with distinguishing runs:

| # | what was done | result |
|---|---|---|
| 7 | `dspset 2 240 quiet` — single sleep for 240 s, **no wakeups**, system free to enter deep retention | **clean**, `div=2` on exit |
| 8 | `dspset 2 180 child` — divider held by our process, inside window `dsptest baseonlydcdopnobench` is launched (loading DSP base image, without node and benchmark) | **device died**; log on `E:` broke off exactly at `child: started` |

### 24.6. CORRECTION OF OWN CONCLUSION: not a matter of "process ownership"

Above (revision from 2026-08-11) I wrote that living and dead runs are distinguished by
the divider lifetime relative to our process, and that the cause is idle/retention.
**Both statements are false, and step 8 disproves them:** the divider was held by our running
process, and the device still died. Step 7 separately disproves the suspicion of retention —
240 seconds of deep idle on divider 2 passed without consequence.

The correct generalization of all six points is single and simple:

> **The DSP domain at 330 MHz is alive as long as nothing executes on the C55x, and crashes as soon as
> the DSP is brought up.**

- steps 4 and 7 (120 s sampling and 240 s deep idle) — DSP is **idle**,
  not a single line of code executes on it: clean;
- step 8 — loading of the base image, that is, **start of DSP/BIOS on C55x**: death;
- step 3 — same plus node: death;
- step 6 — divider left in system, device "idle": death did not occur
  immediately. This is the same reason: sooner or later the DSP domain is brought up by something stock
  (audio, notifications, radio), and at that moment everything repeats. "Reboot by itself"
  was not spontaneous, but delayed.

That is, the reboot is caused not by an out-of-spec divider as a register write, but by **code
execution on C55x at CORE/2**.

### 24.7. Consequence: cannot overclock DSP by divider, but by multiplier — already overclocked

The divider is an integer, so from the stock 3 the only step up in frequency is 2,
that is, **+50%** right away. Intermediate values do not exist. And C55x cannot hold 330 MHz.
Hence **the divider lever is unsuitable for overclocking** — too coarse.

However, DSP is clocked from CORE, and the DPLL multiplier already raises it. At working
`mult 111` the result is `852480/3 = 284.16 MHz`, that is, **+29% to DSP** — and the device
sustains this mode daily, including audio (§22). That is:

| C55x frequency | origin | status |
|---|---|---|
| 220.16 MHz | stock, `mult 86`, divider 3 | — |
| **284.16 MHz** | `mult 111`, divider 3 | **works daily, verified** |
| 330.24 MHz | `mult 86`, divider 2 | **crashes device upon DSP operation** |

The limit of C55x at stock voltage lies between 284 and 330 MHz. Thus DSP overclock on
this device is already achieved — as a side effect of core overclock, and in exactly the
amount that the silicon withstands.

**What the divider remains useful for:** movement in the OTHER direction (4, 6, 8 — i.e. 165, 110, 82
MHz) is safe by design and yields power savings when DSP is not needed.

**The project rule worked as intended:** nothing was written to flash, the kernel patch
was removed before the end of each run, and reboot restored the device to
stock state every time. The log with `Flush` on every line survived the crash and showed the exact
second at which everything happened — without it, step 8 would have been yet another guess.

## 25. Why C55x benchmark is lower than expected: what this project contributes

The neighboring project (`CHARACTERIZATION.md` §8) measures 45–67 million machine
operations per second **regardless of code type** and concludes from this:
"real operating frequency is around 60 MHz, not the nominal 220". The hypothesized cause —
`ivapolicyserver.exe` holds DSP in a low mode because nobody requested it.

The data from this project allows ruling out one thing and adding two.

### 25.1. Ruled out: "low mode" via divider

Whatever `ivapolicyserver` does, **via `CM_CLKSEL_DSP` it does not do this**:
the field is stable (§24), and both stock setters of this register across the entire ROM
are dead — neither called nor exported (§24.2). A divider that "someone
lowered" does not exist. One must look either in voltage or in stalls.

### 25.2. "60 MHz" and "220 MHz with uniform stall" — this is the same measurement

The conclusion of §8 rests on all workloads giving the same number of
operations per second. But a uniform stall produces the **exact same signature** as
a four times lower clock: if each instruction fetch costs extra cycles, this
slows down Q15, fp32, and division equally.

The control kernel `dsp_reg_mac` does not distinguish this. It removes accesses to
**data**, but code is fetched from the exact same place as before. That is,
what was tested is "data is not the bottleneck", not "memory is not the bottleneck".

**Yet clock is now known directly from hardware:** `CM_CLKSEL_DSP[3:0] = 3`, meaning
`CORE/3` = 220.16 MHz (or 110.08 at lower OPP, see below). There is no 60 MHz in
the register. Hence the explanation "low clock" on the hardware side is **closed**, and
the remaining gap must be explained by stalls — the primary candidate remaining is
node instruction fetch.

### 25.3. Added: DSP frequency is hostage to ARM governor

`DSP = CORE / 3`, and `CORE = DPLL × CM_CLKSEL2_PLL[1:0]`, and this field **itself fluctuates
between 1 and 2** under the DVFS governor (§12, §18). That is, DSP runs at 220.16 at times and
110.08 MHz at other times — not because someone controls the DSP, but because they control the ARM.

During the DSP benchmark the ARM is **blocked waiting** for the result, meaning from the
governor's perspective it looks like idle. The most natural consequence: OPP drops, CORE
drops by half, and DSP along with it.

This also explains that peculiarity of their data which they attributed to audio stack
preemption: ratings **monotonically degrade** as workload volume increases
(88.1 → 68.2 → 55.6 MMAC/s). The longer DSP calculates alone, the larger fraction
of time the governor holds low OPP, and the lower the effective rate. Audio
preemption does not imply such monotonicity, whereas this mechanism does.

This also explains the bimodal `_BRIDGEINIT_DSP_FREQ` (§24.1): the same mechanism,
the same factor of 2.

### 25.4. Two cheap experiments that test this

1. **Keep ARM busy during DSP work** (separate thread in a loop). If part of the
   gap is from the governor, numbers must go up, by up to ×2. Done entirely on
   their side, the device risks nothing.
2. ~~Run the same benchmark at `mult 111`.~~ **DONE, see §25.5.**

### 25.5. Experiment executed: C55x operation strictly clock-dependent

The same node, same `dsptest`, same binaries (`-SkipBuild`), 3 runs
per point, summary — series minimum:

| CORE | DSP frequency | time, us | spread |
|---|---|---|---|
| `mult 86` (stock) | 220.16 MHz | **1 136 260** | 5% |
| `mult 111` | 284.16 MHz | **882 995** | 15% |

Speedup **28.7%**. Pure clock scaling predicts
`284.16/220.16 − 1 = 29.1%`. Agreement within a quarter of a percent.

**What follows from this:**

1. **Benchmark time is almost entirely determined by execution on the DSP.** If fixed
   overhead (node creation, message exchange,
   ARM side) constituted a substantial fraction, the gain would be noticeably smaller than clock-proportional.
2. **There is no hidden "performance mode" holding back the DSP.** The machine
   responds to exactly the frequency we set in `CM_CLKSEL_DSP`/DPLL.
   The hypothesis that "`ivapolicyserver` holds DSP down, there is a hidden reserve of ×3–4"
   (`CHARACTERIZATION.md` §8) is **not confirmed** by this: a policy that nobody
   requested would not scale linearly with CORE.
3. **Hence the deficit is not in frequency, but in clocks per operation.** ~67 M op/s at
   220 MHz is ~0.3 operations per cycle, and this value is constant. One must look for
   a uniform stall: the primary candidate remains node instruction fetch
   (§25.2), which their control kernel `dsp_reg_mac` does not check, because it eliminates
   accesses to data, not to code.
4. **Indirectly — a ready gain for the client:** at `mult 111` DSP runs at
   284.16 MHz, and the benchmark sees this as −22% time. No changes in their code
   are needed.

Spread on 111 is larger (15% vs 5%) — expected, because at higher CORE the governor
shifts OPP with the same steps, but from a higher base.

## 26. MPU divider and full clock layout: tool exists, measurement not yet performed

The task is `docs/TASK_MPU_DIVIDER.md` from neighboring project `../PS1Emulator`.
It requires a configuration in which the core frequency changed, while the bus frequency did not:
without such a pair, the frame time ratio does not separate "bottlenecked by CPU" from
"bottlenecked by memory", because the DPLL multiplier shifts both domains by the exact same
percentages.

Done 2026-08-15. Three runs on the device answered the questions in §5 of the task and
simultaneously found the real reason for what the client described as "dividers do not
hold". The cause turned out to be completely different from what both projects had assumed.

### 26.0. Main point: invalid divider rejects ENTIRE write to CM_CLKSEL1_CORE

Divider compensation did not "survive for seconds" — it was **not written at all**.
Read-back is done in the exact same operation as the write, so there is no window here in
which anything could have reset it.

Four measurements. Each — snapshot before, immediately after the write, then once every 10 s, and
restoration of stock state on exit.

| run | mult | l3 | dss | was written |
|---|---|---|---|---|
| 2 | 86 | 6 | 6 | **yes** |
| 3 | 107 | 5 | 7 | no |
| 5 | 107 | 6 | 7 | no |
| 6 | 107 | 6 | 6 | **yes** |

Runs 5 and 6 differ **only in the DSS1 field** — same multiplier, same
L3 divider, same ramp preceding them. Thus the invalid value here is `dssdiv=7`.

And more important than the value itself: in run 5 **nothing** was written, even though
`l3div=6` is known to be valid (runs 2 and 6 accepted it). That is, an invalid value in
one field rejects the entire write to the register, not just its own field.

> **CORRECTED in this very text.** Initially I wrote here the conclusion "to blame is
> `l3div=5`": it rested on run 3 (where there was both `l3=5` and `dss=7`) and on
> the L3 validator in ROM, which indeed rejects 5. Control run 5 disproved
> it — with valid `l3=6` and the same `dss=7`, the write likewise did not go through.
> This is precisely the mistake warned against in §5 of CLAUDE.md: two variables in a single
> measurement and a plausible explanation for one of them.

The L3 validator @`0x800AC424` was nevertheless read correctly, and §21 was corrected: it
rejects **0, 3, 5, 9, 18**, rather than 18 alone. But from this story emerges
a broader lesson: **the firmware validation table is not enough**. The DSS1 validator
@`0x800AC49C` rejects only 18, meaning `dssdiv=7` is valid according to it — yet the device
does not accept it. The hardware set of dividers is narrower than what the firmware validates.

Corrected accordingly, and at two levels. Automatic selection (`CompensatedCore`)
now operates on a knowingly narrow set `KAutoDivAllowed` = 1, 2, 4, 6, 8, 12,
16 — this is the classic OMAP2 set, containing both values used
by firmware itself (4 for L3, 6 for DSS1), and both that were successfully written in
our measurements. Manual mode remains broader, from firmware validation plus
the measured exclusion of `dss=7`, and relies on read-back: the `MISMATCH` line
catches hardware rejection immediately.

Why this matters not only for compensation accuracy: if the write fails to go through, CORE
changes with the **old** dividers. This exact situation broke rendering (§21),
and this was precisely what compensation was meant to eliminate.

### 26.1. Task §5: layout holds, nothing needs to be retained

Both halves of the question are settled, and both yielded "holds".

`CM_CLKSEL_MPU = 4` held for all 40 s under load, **without a resident
patch** (between snapshots the patch does not live in the kernel at all) and through an OPP transition —
`CM_CLKSEL2_PLL` during this time went 2 → 1 → 2, while the MPU divider did not budge.
The governor does not touch this register.

`CM_CLKSEL1_CORE` with non-stock `l3div=6` held likewise, 0 drifts. Plus
indirect, but strong evidence: the device encountered the first run with the word
`04360544` (`dssdiv=5`), which was left there by the client's run **13.6 hours ago** —
the `NOT STOCK AT START` line showed precisely this. A non-stock word lives for hours.

Therefore the hypothesis "governor rewrites dividers" is **disproven**, and along with it the
need for retention: the `ReassertDividers` blob and the `hold=1` switch remain, but
are not needed for this task.

### 26.1a. Another erroneous conclusion corrected: SDRAM refresh

In `PickDivider` the rationale stated that for equal error it is better to take a lower
bus frequency because "refresh is counted in L3 cycles, so a slower bus means
more frequent refresh, not rarer". This is **backwards**.

`SDRC_RFR_CTRL0 = 0x0004DA01`, that is, `ARCV = 0x4DA = 1242` L3 cycles. At
165.12 MHz this is 7.52 us — exactly the autorefresh window for 8192 rows over 64 ms.
The counter counts bus cycles, so **a slower bus stretches the interval in real
time**: at 82.56 MHz this is already 15 us, at 41.28 — 30 us, and the rows have time
to lose charge.

The numbers are concrete. `ARCV = 0x4DA = 1242` bus cycles; at stock 165.12 MHz this is
7.52 us — exactly 64 ms / 8192 rows, i.e. right up against specification. At `l3div=8`
the 82.56 MHz bus gives 15.0 us (nearly double the spec), and at the lower governor OPP
of 41.28 MHz — 30.1 us, i.e. quadruple.

Whether the firmware compensates its own lower OPP is **unknown**: `SDRC_RFR_CTRL0` was
previously read by us only between runs when `sel2=2`, and we always saw the same
`0x0004DA01`. If it does not move ARCV, then the stock device at lower OPP is already
operating at 2× over spec — and in that case the true retention margin is at least
twofold, while breakdown occurs somewhere between twofold and fourfold. This is an experiment, not a guess:
the register is now sampled in the same packet as dividers, and printed along with the
interval in nanoseconds.

Regardless, the firmware certainly does not know about OUR divider. Therefore, dropping the bus below
stock is a risk, not a safeguard, and the L3 selection policy has been changed accordingly:
we take the highest bus that does not exceed the **measured** ceiling of 213.12 MHz. This ceiling is not arbitrary —
it is the frequency at which the device actually operates daily at `mult 111`, with
zero SDRAM integrity errors in every log.

Practical consequence for the client: their configuration C (`l3=4` at `mult 43`,
i.e. bus 82.56 MHz, and at lower OPP 41.28) **is not risk-free**, despite being
a downclock. Configurations A, B, and D are not affected by this — there the bus is either
stock or unchanged.

### 26.2. Instrument calibrated at the same time

Both measurements provide what no previous measurement provided: movement of **one** domain.

| configuration | core | bus | NOP | MEM |
|---|---|---|---|---|
| `mpu=4`: core halved, bus stock | 325 → 161 MHz | 165.12 unchanged | **−50.4%** | **−20.8%** |
| `l3=6`: bus lower, core stock | 324 → 323 MHz | 165.12 → 110.08 | **+0.3%** | **−30.2%** |

That is, this project's own `BestMem` is **not a pure bus measurement**: half
core frequency costs it 20.8% even with an unchanged bus. This is worth keeping in mind
when re-reading all previous "RESULT MEM" entries in logs: there core and bus always moved
together, so separating their contributions was impossible in principle.

### 26.3. Consequence affecting project's working configuration

Old `CompensatedCore(107)` picked `dss=7`, and `CompensatedCore(111)` —
`l3div=5`. Neither value is accepted by hardware, so **compensation on working
multipliers was never applied**, and `hold 111` kept the stock word
`04360644` the whole time: bus ran at 852480/4 = 213.12 MHz (+29%), and display at 852480/6 =
142.08 MHz (+29%) — rather than 170.50 and 110.08 as recorded in `state.md`.

For multiplier 107 this was measured directly: in runs 3 and 5 the register remained stock, and
the `after` line showed `BUS=205440 DSS1=136960`. For 111 this is the same code and the
same mechanism, but the pair of numbers itself has not yet been acquired from the device.

Practically this is rather good news: for a week the device has held +29% not only on the core, but
also on the bus and display, with zero SDRAM integrity errors in every log. That is
why the new L3 selection policy (§26.1a) targets what actually works, rather than
theoretical "compensation": after the fix, `CompensatedCore(107)` gives `l3=4`
(bus 205.44, as it was) and `dss=8` (display 102.72), and `CompensatedCore(111)` —
`l3=4` (213.12) and `dss=8` (106.56).

Both layouts verified on the device — written, read back, hold
the dwell window:

```
mult=107 l3=4 dss=8 -> CORE=04360844  BUS 205440  DSS1 102720   CPU +20.3% MEM +20.8%
mult=111 l3=4 dss=8 -> CORE=04360844  BUS 213120  DSS1 106560   CPU +24.6% MEM +24.7%
```

That is, after the fix, daily `mult 111` leaves the bus where it actually was
all along (213.12 MHz), while the display is truly compensated for the first time: 106.56 MHz
instead of unforeseen 142.08.

### 26.4. Fixed: compensation reference point is now constant

`Prepare()` previously sampled `iStockCore`, `iStockMpu`, and `iStockClocks` from **live**
registers, meaning the reference became whichever layout the process found the device in.
While the device is stock, there is no difference. Once it is overclocked — the point drifts, and
`CompensatedCore()` targets `iStockClocks.Dss1Khz()`, that is, an already shifted
display frequency.

The consequence was reproduced in the client's logs completely: `hold 107` left the device with
`dssdiv=6` at CORE 821.76, i.e. with pixel clock 136.96 MHz; subsequent
`hold 86` read this as stock and faithfully preserved it, decrementing the divider to 5, and in
the line `vs stock: DSS1 -3.5%` reported that the display was almost untouched —
although in reality it locked it at 132.1 MHz instead of 110.08. Every "raise —
lower" cycle shifted the divider by another step and did not return.

The reference point has been extracted into constants in `ocengine.h`, sampled from the device after a cold
boot:

```
KStockClkSel1Core = 0x04360644   l3div=4 dssdiv=6  ->  BUS 165.12, DSS1 110.08
KStockClkSelMpu   = 0x00000002   mpu_div=2         ->  MPU 330.24
KStockMult        = 86           DPLL 330.24       ->  CORE 660.48
```

Live state at start remained accessible separately (`BootMult()`, `BootCore()`,
`BootMpu()`) and is printed with the line `NOT STOCK AT START` when it does not match
stock. Previously this case silently became the new normal.

At the same time `RestoreStock()` was fixed. It performed only the multiplier ramp, and that
writes **nothing** when the multiplier is already stock — so a layout of the form "mult 86,
mpu_div 4" would remain in hardware. Now following the ramp comes one explicit write of the stock
layout.

### 26.5. "Dividers live while patch lives" — this is not so

The client explained the disappearance of compensation by claiming that dividers are held by the relock blob, and
as soon as the patch is removed, the stock setter rewrites them (`TASK_MPU_DIVIDER.md` §2).
The mechanism was named incorrectly, and this is apparent directly from our code.

The patch of the `DoControl(1)` body is a **transport**, not a latch. The blob executes once,
writes registers, returns control, and that is all; then the patch is removed, and
none of our code remains in the system. The residency of the patch is completely
unrelated to what stands in `CM_CLKSEL1_CORE`. That is, if the register returned to the
stock word, someone else did it, and would have done so just the same with a live patch.

Who exactly — not determined. Most likely candidate: DVFS governor. That it is
active has already been measured in the project (`CM_CLKSEL2_PLL[1:0]` oscillates between 1 and 2, §12 and
§18), and the returned value is exactly the stock word, meaning it looks like a write from the
OPP table. This is a **hypothesis**, disassembly of the OPP transition path was not performed.

The practical consequence is opposite to what the client inferred: if this is the
governor, keeping the patch resident is useless, and a child process by itself is
also not enough. One must either ensure that the governor does not
touch the specific field (for `CM_CLKSEL_MPU` this is an open question, §5 of the task), or rewrite
dividers in that same window.

### 26.6. What client's logs actually show and what they do not

`HoldOverclock` **did not reread** `CM_CLKSEL1_CORE` after writing. The line
`CLKSEL1_CORE 04360644 -> 04360745` is printed by `LogCompensation()` **before**
application, that is, this is intent, not fact. Hence from those logs it does not even follow that
the compensated divider was ever present in the register: it might not have been written at all,
or it might have been written and disappeared in seconds. The log does not distinguish between these two cases.

Now it is known which one: **did not write at all** (§26.0). But along the way it is worth
noting an argument that was tempting and turned out to be false. In that same run
`hold 107` memory bandwidth increased by 20.5% with the intent to keep the bus
unchanged, and this looked like proof that compensation was not working. Proof it was not:
measurement §26.2 showed that `BestMem` loses 20.8% from halving the
core frequency alone, with an absolutely unchanged bus. That is, this instrument responds to both
domains, and in configurations where core and bus move together, it differentiates nothing.

The issue was resolved by reading back the register immediately after writing — this is now present in both
`patcher clocks` and `patcher hold`.

### 26.7. The `patcher clocks` command

```
ocprobe.exe patcher clocks mult=86 mpu=4 l3=4 dss=6 sec=300 [sample=10]
                           [child=ps1|dsp] [hold=1] [args:<child arguments>]
```

Default switches give exactly the stock layout, meaning the command without
arguments alters nothing, only measures and monitors.

The internal order of steps is not arbitrary. **Application in two steps**: a naive
ramp with final dividers on each step would kill the device, because the configuration
"mult 43, mpu_div 1" on intermediate multiplier 86 would give the core 660 MHz, and the bus 330 —
twice the stock and twice the 426 MHz ceiling established by this project.
Therefore, first comes a regular ramp with automatic compensation (the tested path; on
each step frequencies stay near stock), and only at the final multiplier
is one explicit write of the desired layout performed — there it is correct by design.

Pleasant coincidence: automatic compensation at `mult 43` itself yields `l3div=2` and
`dssdiv=3`, which is exactly what configurations B and C require. The explicit step changes
only the MPU divider there.

Next: read-back of all four registers with explicit `MISMATCH` line; measurement of
NOP and bandwidth before and after; dwell window with periodic snapshot of
layout; read-back after child; explicit restoration of stock state with read-back.

Child arguments come **last**, after the `args:` marker, and everything after it
is cut off from our own command-line parsing — otherwise a foreign word
(say, `power`) would be caught by our `Find` and launch a foreign branch. The command-line
buffer was increased from `TBuf<64>` to `TBuf<256>`: client string
`gui play nosnd frames6100 dsa light blend imggt.pbp` is 50 characters, and in the
previous buffer the command would silently truncate in the middle. 256 is as much as
the deployment daemon accepts (`tester_exec.cpp`).

### 26.8. `ReassertDividers`: rewrite dividers without stopping DPLL

In case the layout still does not hold, a separate blob was added: writing
`CM_CLKSEL1_CORE` and `CM_CLKSEL_MPU` plus latching `PRCM_CLKCFG_CTRL`, under
disabled interrupts, **without DPLL bypass**.

Why not use the existing relock: for the sake of that same pair of registers it drives the
DPLL into bypass every time, and that is the very window that hangs the
device when display is active (§20, §21). Here CORE does not change at all — only dividers derived from
it change. This is not a new class of operation for this SoC: the
governor itself does exactly this at each OPP transition.

**Not executed on device even once.** Enabled only with explicit `hold=1`
switch, and the command logs this with a separate line.

At the same time an outdated comment near `ApplyDspDiv` was fixed: it still stated that the
`PRCM_CLKCFG_CTRL` latch was not needed. This was disproven on device back in §24 — the setter
returned 0, but the register did not change, — yet the old comment had remained.

### 26.9. What remains to be measured

| Question | Status |
|---|---|
| Does `CM_CLKSEL_MPU` hold without resident patch | **closed: holds**, 40 s under load, through OPP transition |
| Does non-stock `CM_CLKSEL1_CORE` hold | **closed: holds**, and lives for hours between runs |
| Why compensation did not take effect | **closed**: invalid divider rejects entire write (§26.0) |
| Does device accept `dss=8` | **closed: accepts**, verified on 107 and on 111 |
| New compensation on working multipliers | **closed**: 107 and 111 are written and hold |
| Which dividers exactly device accepts | partially: 2, 4, 6, 8 are written, 7 not; rest not verified |
| Does `ReassertDividers` work | open; no longer needed for this task |
| Client's pairs A/D and B/C | open, requires their `ps1stage1.exe` on device |

Dividers are now also validated in `patcher clocks` — an invalid value is rejected
with an explanation before anything is written. And the main safeguard is that
every write is read back: it was precisely the `MISMATCH` line that caught this whole story.

### 26.10. Ramp DOWN to multiplier 43 crashes device (two out of two)

Run `mult=43 mpu=2 l3=2 dss=3 sec=15` — verification of `dss=3`, needed for
client's configurations B and C, — **failed to complete twice in a row**. Both times
the device stopped responding, and both times recovered on its own (everything runtime).

The log from device localizes the moment precisely. It breaks off immediately after the
"before" measurement, and the ramp line is missing entirely:

```
--- CLOCKS: mult=43 mpu=2 l3=2 dss=3 sec=15 child=0 hold=0 ---
  target: CORE=04360342 MPU=00000002 -> CORE=330240 MPU=165120 BUS=165120 DSS1=110080 kHz
  before: m=86 mpudiv=2 l3div=4 dssdiv=6 sel2=2 -> ...
  BEFORE: 324 MHz (NOP), 512 MB/s (MEM)
<end of file>
```

That is, the device dies **inside the ramp**, not on the final layout. The final
point itself is completely innocent here: bus 165.12 and display 110.08 — exactly
stock, core halved. It is the journey that is dangerous.

A descent below 86 was never attempted in this project: all ramps were 86 ↔ 111.
What exactly kills it is not determined; there are three candidates, none verified:

- **twice as many bypass windows**: 43 steps vs 25 in daily 86 → 111,
  each with active display (§20);
- **display on intermediate multipliers**: autoselection without divider 3 gives DSS1
  89.6 MHz (−19%) near multiplier 70 and 82.6 MHz (−25%) at 43. Divider 3 would give
  exactly stock 110.08, but for L3 it is invalid, and for DSS1 unverified;
- **lower governor OPP**: at any moment it can set `sel2=1` and
  divide CORE in half again. At multiplier 43 this is CORE 165.12, MPU 82.56, bus 82.56
  and display 55.04 — a domain where the device has never operated.

So that the next attempt localizes at least something, the ramp in `patcher clocks` is now
detailed step-by-step: each step logs its multiplier, divider word, and
derived frequencies, with flush. Emptiness after the `BEFORE` line, as above, will not
recur.

### 26.11. Client's configuration C is accessible without changing multiplier

The most useful consequence of §26.10: reaching multiplier 43 is not required
for most of what is needed.

The client needs three points: reference, "halved core with unchanged
bus", and "same core with halved bus". The third was planned as
`mult=43 mpu=1 l3=4 dss=3`, i.e. via ramp. But the exact same pair of frequencies is provided by
the L3 divider at **stock multiplier**:

| point | client | same without DPLL | core | bus | display |
|---|---|---|---|---|---|
| A | `mult=86 mpu=2 l3=4 dss=6` | same | 330.24 | 165.12 | 110.08 |
| D | `mult=86 mpu=4 l3=4 dss=6` | same | 165.12 | 165.12 | 110.08 |
| C | `mult=43 mpu=1 l3=4 dss=3` | **`mult=86 mpu=2 l3=8 dss=6`** | 330.24 | **82.56** | 110.08 |

`CORE/8 = 82.56` — that same halved bus, core and display stock, and
no relock beyond a single step. That is, all three points that actually separate the
CPU share of the budget from memory are obtained without a ramp at all.

Only configuration B requires a ramp (`CORE` halved with unchanged core and
bus) — it answers a different question, about CORE as an internal ceiling, and remains
out of reach for now.

### 26.12. C run: number obtained, device died immediately after window

`mult=86 mpu=2 l3=8 dss=6 sec=20` was written on first attempt, read-back
confirmed `CORE=04360648`, the layout held for the whole window without a single drift. And
gave the number for which the measurement was needed:

```
BEFORE: 324 MHz (NOP), 513 MB/s (MEM)
AFTER : 322 MHz (NOP), 282 MB/s (MEM)
RESULT: CPU 0.6%  MEM -45.0%
```

However, the log cuts off at `drift samples: 0`, meaning the device died **on
returning stock state or immediately prior**, after twenty seconds of operation
on the lowered bus. The last snapshot shows `sel2=1`, meaning the governor was sitting
on lower OPP: bus 41.28 MHz, display 55.04.

This **accords with §26.1a and is the strongest evidence in its favor**, though not
proof. 41.28 MHz vs stock 165.12 is a 4× stretched autorefresh interval:
30 us instead of 7.5, meaning a full cycle across 8192 rows in 256 ms
instead of 64. This is outside any charge retention specification, and twenty seconds
is plenty of time for memory to corrupt.

### 26.14. Refresh hypothesis DISPROVEN, and failure signature became more precise

The guess from §26.12 turned out to be false, and was disproven by the very experiment intended
to confirm it. `SDRC_RFR_CTRL0` and a charge **retention** check were added to the snapshot
(buffer populated once, verified on each sample, so the full interval passes between write
and verification, rather than fractions of a millisecond as in
`MemoryIntegrity()`). Repeated run `l3=8`:

```
sel2=2:  RFR=0004da01  arcv=1242 -> 15043 ns
sel2=1:  RFR=00032601  arcv= 806 -> 19525 ns
SDRAM retention over 5 s: 0 bad words in 1 MB   (three samples out of three)
```

Two conclusions, both measured.

**The firmware does move ARCV.** Upon transition to lower OPP it rewrites
`SDRC_RFR_CTRL0` from 1242 to 806. We did not see this earlier only because we read
this register between runs when governor sits at upper OPP. Thus the
assumption in §26.1a that firmware compensates its own OPP was confirmed,
but about our divider it indeed knows nothing: 806 cycles at 41.28 MHz yield
19.5 us instead of 9.8 which it intended.

**Yet memory does not notice this.** Zero corrupted words per megabyte over three
five-second dwells at 2.6× stock interval. Therefore loss of charge is
**not** the cause of device death, and the recommendation "C can be measured, but not held"
was based on false reasoning. The recommendation itself remains valid, but for
a different reason, and the cause is now known more precisely.

**The device dies on RETURNING stock state.** Both `l3=8` runs
completed the dwell window fully (`drift samples: 0`) and both cut off at
the next step — `RestoreStock`. The change `l3div` 4 → 8 succeeded cleanly twice,
the reverse 8 → 4 killed twice.

The asymmetry points to the cause. Both directions perform the same full relock,
differing only in state **before** the bypass window: in it CORE drops to `sys_clk`
19.2 MHz, so at `l3div=4` bus gets 4.8 MHz, and at `l3div=8` — 2.4 MHz. This is
a guess, but the only one explaining the asymmetry.

The fix following from this: when multiplier is already stock, there is no need to restore dividers
with a full relock. `RestoreStock()` now first tries
`ReassertDividers()` — write plus `PRCM_CLKCFG_CTRL` latch, without bypass, — reads
result back and falls back to relock only upon failure.

**Did not help.** Run with this fix died in the same place, with the
same signature: full dwell window, `drift samples: 0`, and cutoff. That is,
BOTH return paths fail — both full relock and lightweight write with latch.

This, in turn, weakens the guess about the bypass window: lightweight path does not have it
at all. Something common to both remains, and there are at least two candidates.

First: `ReassertDividers` changes bus frequency **without protecting SDRAM**. Relock blob
puts memory into self-refresh before its window (write `0x48` to `SDRC+0x70`,
findings §20), whereas the lightweight blob does not do this at all — it was written for the DSP
domain where memory is irrelevant. Restoring `l3div` 8 → 4 at `sel2=1` doubles bus frequency
live, with active SDRAM.

Second: the direction itself. Both paths double the bus frequency, whereas the forward run
(4 → 8) reduced it — and passed cleanly. Possibly the issue is specifically in raising frequency with
unchanged `SDRC_ACTIM` timings, rather than in the method of doing so.

### 26.15. Death localized: it is inside the divider write itself

To avoid guessing which call in the restoration sequence terminates execution, each
step received its own line in the log, and the run was repeated (`out/ocprobe_diag.log`).
The log ends as follows:

```
  restore: step 1, read live layout
  restore-before: PLL=00056400 SEL2=00000001 MPU=00000002 CORE=04360648 RFR=00032601
  restore-before: m=86 mpudiv=2 l3div=8 dssdiv=6 sel2=1 -> CORE=330240 MPU=165120 BUS=41280 kHz
  restore: step 2 skipped, multiplier already stock
  restore: step 3, ReassertDividers (no DPLL bypass)
```

The line `restore: reassert -> r=… rc=…`, which is written immediately upon returning from the
call, is missing. Hence the device dies **inside** `ReassertDividers`, not on
reading registers before it and not on the fallback relock after it. The fallback path
in this run was not executed at all, so §26.14 needs to be clarified: it says
"both paths fail", and this remains true across two different runs, but in
this specific one it never reached relock.

This removes remaining doubts regarding the bypass window: in the lightweight blob it is absent, yet
the device dies anyway. What remains is what the blob does: writing `CM_CLKSEL1_CORE`,
writing `CM_CLKSEL_MPU`, latching `PRCM_CLKCFG_CTRL` and waiting for it to clear.

The snapshot before death adds another detail not previously visible. At the moment of
restoration the governor sits at **lower** OPP (`SEL2=1`, CORE 330.24 MHz), meaning the
bus runs from 41.28 MHz, rather than 82.56. Restoring `l3div` to 4 multiplies it **fourfold
relative to what memory just saw**, and does so live, without self-refresh —
that safeguard exists only in the large relock blob (§20). Of the two candidates in §26.14 this
strengthens the first and almost eliminates the second: it is not the direction per se, but that
SDRAM frequency is changed without stopping accesses to it.

Hence the next step when time permits: add into `OcDivBlob` that same
pair "self-refresh before change, exit after" that already exists in the relock blob. Until
this is in place, there is no way to return from a lowered bus, and the only reliable method is
reboot, which returns the device to stock state anyway.

They can only be distinguished by measurement, and so far the log does not even show which
exact call within restoration cuts off the run: `RestoreStock()` is silent. Therefore
in `patcher clocks` restoration is now broken down step by step — read, ramp,
reassert, read back, fallback relock, — and each step logs itself **before**
execution. The next run will pinpoint the location exactly.

**Three device deaths on a single operation, so we proceed no further without a dedicated solution.**

Practical conclusion for the client: **point C yields a number, but the run terminates in
device death**. Why exactly — was uncovered by the subsequent experiment, §26.14, and the cause
is not the one indicated here.

### 26.13. Instrument decomposed into two contributions

Three measurements, in each of which exactly one domain moves, provide the full picture of what
this project's `BestMem` actually measures:

| what was moved | core | bus | NOP | MEM |
|---|---|---|---|---|
| `mpu=4` | 325 → 161 MHz | unchanged | −50.4% | −20.8% |
| `l3=6` | unchanged | −33.3% | +0.3% | −30.2% |
| `l3=8` | unchanged | −50.0% | +0.6% | −45.0% |

The last two rows fall almost onto a line: `BestMem` reflects about 0.9 of bus
frequency change. The first row gives the second contribution: about 0.42 of core
frequency change. That is, this is **not a bus measurement**, but a composite, and in configurations where core and bus
move together, it distinguishes nothing — all previous "RESULT MEM" entries in project logs
are like this.

At the same time this is a ready key for the client: a profile item behaving like
the first row is core-bound; like the third — memory-bound; intermediate is decomposed by
these two coefficients.

## 27. E90 lacks DVFS entirely: generalization is not constrained by addresses

Measured 2026-08-31. The Nokia E90 device is accessible via Bluetooth (`deploy_net.ps1
-Com COM9`), daemon responds, `INFO` yields `screen 800x352`, `ramfree 77619200`,
`boot 1 by t`. Access to protected directories on it is now full: `LS
C:\sys\bin\` returns 272 files, among them `patcher.ldd` (1568 bytes),
`patcherS3.ldd`, and `patcherShadow.ldd`. That is, the prerequisite for our engine —
the RomPatcher+ driver — is in place.

Firmware of the device was read from itself, `Z:\resource\versions\sw.txt`:

```
400.34.93
21-04-2009
RA-6
(c)Nokia
```

This is exactly the version whose MCU image is already present in
`firmware/e90-ra-6-400.34.93/ra6_400.34.93_prd.c00`, so there is something to verify against.

### What was found in the image and what was not

Module names in the ROM catalog are stored as UTF-16 and lie uncompressed in the FPSX
image — this has been verified on both devices. In RM-320 one finds
`power.dll`, `dvfs_interface.ldd`, and `ehwaomap.pdd`. In RA-6 one finds
`power.dll`, whereas `dvfs_interface.ldd` and `ehwaomap.pdd` are absent.

The search was conducted wider than just three names, because names could differ:
substrings `dvfs`, `DVFS`, `Dvfs`, `ehwa`, `EHWA` do not occur in the E90 image
a single time, whereas `omap` occurs. In RM-320 all of them are present. For
completeness: in the unpacked `z:` from RM-320 the DVFS subsystem consists of
`dvfs_conf.dll`, `dvfs_engine.dll`, `dvfs_hal.dll`, `dvfs_interface.ldd`, and
`dvfs_monitor.dll`.

### Why this matters

The memory read primitive in this project is built on patching the body of
`dvfs_interface` `DoControl(1)` (section 19). If the driver itself is absent from the
firmware, there is nothing to port: the question is not what address the
function lies at, but that the function does not exist.

Therefore generalization to E90 is not a task of finding offsets, as planned in
`ROADMAP.md` §2, but a task of finding **another entry point**. This is precisely why
E90 was chosen as a generalization test: failure here costs an evening, not a
published release.

### Limits of this statement

This is a search across the raw FPSX image, not a ROM parsing. Had the name directory
been compressed, absence of a string would prove nothing. But `power.dll` and `omap` in
that same image are found, and in the same encoding and at nearby
offsets, so the region with names is readable plainly. Therefore this is strong evidence, not
definitive proof; definitive it will become after proper parsing of the
ROM catalog.


### Same-day correction: overclock mechanism on E90 EXISTS, only reader is missing

The conclusion above ("generalization rests on finding another entry point") is correct,
but easily read too pessimistically, and a deeper search refutes that
reading. Overclocking on E90 does not run up against absence of a mechanism — the mechanism is in
place.

First, verification of the device driver list (`LS Z:\sys\bin\`, 2590 files) against
the RM-320 list. On E90 the entire DVFS subsystem is indeed absent (`dvfs_interface.ldd`,
`dvfs_engine`, `dvfs_hal`, `dvfs_conf`, `dvfs_monitor`), and two other things we could
have relied upon are also absent: `d_powerapi.ldd` and `memoryaccess.ldd`. But
`power.dll`, `ehwa.ldd`, `ehwaomap.pdd`, and `patcher.ldd` are in place.

Key are not names, but the registers themselves. Search in the E90 image for mapped PRCM
addresses (physical address plus `0x7E000000`) finds the exact registers managed by
our engine:

```
CM_CLKSEL1_PLL  0xc6008540  (DPLL multiplier - the main overclock register) x3
CM_CLKSEL2_PLL  0xc6008544                                             x4
CM_CLKSEL_MPU   0xc6008140                                             x2
CM_CLKEN_PLL    0xc6008500                                             x1
CM_CLKSEL1_CORE 0xc6008240  (bus and DSS dividers)                     x4
power-down glob 0xc80030c8                                             x1
```

And they are not scattered — in E90 at offset `0x618028..0x61804c` lies a literal
pool immediately following `str r0,[r1,#4]` / `bx lr`, in which `CLKSEL2_PLL`,
`CLKEN_PLL`, `CLKSEL1_PLL`, `CLKSEL_MPU` appear in sequence. This is the tail of a function in `power.dll` that
reads and writes the clocking register block — the same code that on N95 sits near
`0x646fxx`. The mapping offset `+0x7E000000` here holds true to the exact match, just as on
RM-320.

Thus on E90 both the register map, the code that writes them, and the RomPatcher+
driver with which we patch this code are present. A single link is missing — the **reader**:
`DoControl(1)` of the `dvfs_interface` driver, from which we sampled frequency for
verification. That is, the task narrows down from "is it possible at all" to "with what to
read back the result on this device", and that is already a different, significantly narrower task. Remaining
candidates on E90: RomPatcher+'s own channel (section 10 showed that it provides
limited reading) or direct measurement by work, as in the `ocprobe` sampler.

The boundary of the statement is the same as above: this is raw image analysis, not ROM parsing.
But finding four PRCM addresses packed into a single literal pool after a function
epilogue is much harder to dismiss as coincidence than an isolated name string.

### Practical caution: current build CANNOT be run on E90

Recorded 2026-08-31 before NOT doing this. `ocgui` is installed on E90
(`C:\sys\bin\ocgui.exe`, 39752 bytes, registration in place), but the launch itself is
dangerous. `COcEngine::Prepare()` is called directly from `ConstructL` and among
other things performs `PokeWords(KDvfsBody, KReadPatch, 4)` — writes 4 words to `0x80148410`.
This address is valid only for RM-320; on E90 different kernel code lies behind it. Subsequently
`ReopenDvfs()` fails (no driver) and `Prepare` exits with error
**WITHOUT removing the patch** (`EndPatchSession()` is not reached on this branch). That is, on
E90 a substituted foreign kernel function would remain, and its first call with non-zero r1
would dereference an arbitrary address in kernel mode — reboot in the best case.

Conclusion for E90 build: addresses (`KDvfsBody`, `KRomSramRoutine`, `KDspSetter`,
`KPwrDownGlobal`) must become parameters rather than constants, and `Prepare` must
remove the already applied READ-patch even on an error branch, prior to any
further step.

### E90 clocking code localized in image (offline, without device)

Disassembler on the E90 image (`objdump -b binary -m arm`, window before PRCM pool
`@0x618028`) revealed the clock write routine itself in `power.dll`:

```
618014: ldr r1,[pc,#64]   ; CM register address from the pool
618018: mov r0,#0
61801c: str r0,[r1]
618020: str r0,[r1,#4]
618024: bx  lr
618028: .word 0xc6008544   ; CM_CLKSEL2_PLL      <- pool
        .word 0xc6008500   ; CM_CLKEN_PLL
        .word 0xc6008540   ; CM_CLKSEL1_PLL (DPLL multiplier)
        .word 0xc6008140   ; CM_CLKSEL_MPU
```

Nearby — register reader via helper (`ldr r0,[pc,#..]; bl 0x618564; ands r1,r0,#0xf`),
meaning E90 contains both reading and writing of clocks, only not via DVFS. This confirms
§27: E90 has the complete mechanism present, and a future E90 build can either patch
this routine or call its helpers. Exact vaddrs will come from installing the image in
EKA2L1; file offsets are already in hand.

## 28. "From file, without device" method tested on 51 firmwares: what it can do and where its limit lies

Measured 2026-08-31. The question was practical: is it possible for devices not
in hand to learn both the required modules and addresses from the firmware file alone. Short
answer: presence of machinery — yes, reliably; exact address — only by body
signature where it catches, otherwise a disassembler is required.

### The tool and why it works

`tools/romscan.js` searches a raw MCU image (FPSX) for three things, without installing
it anywhere. This is possible because the kernel side of the ROM (`ekern`, `power.dll`, `*.ldd`,
`*.pdd`) resides in XIP CORE **uncompressed** — code executes directly from flash, so
its instructions and names are present in the image as is. Compressed ROFS sections
(primarily user-side) do not fall in here, and this is an intentional boundary.

The script reports:
- **dvfs_interface driver by name** — reliable test of READ primitive presence;
- **`DoControl(1)` body by signature** — exact patch site, six words, two
  address-dependent verified by mask;
- **PRCM literal pools** — clocking registers adjacent, i.e. WRITE mechanism.

### Ground truth verification

On our RM-320 `31.0.015` the script finds the body at exactly `0x8c0222` — this is
`KDvfsBody` (vaddr `0x80148410`), which `ocengine.cpp` patches. That is, the method
reproduces from file the exact same address that we obtained by working on the device. On E90
the body is absent, and the driver is absent both by name and on the live device (`LS
Z:\sys\bin\`) — two independent sources converged.

### Asymmetry that cannot be ignored

The body signature is a **positive** detector: a match is always genuine (verified), whereas
a **miss does not prove absence**. `DoControl(1)` compiles differently across versions,
and on RM-320 `20.0.016` the body is not caught by signature, even though the driver is
there — the name `dvfs_interface` is present in the image. Therefore present/absent is taken by
name, while body signature is taken only as "patch site found immediately / will need
disassembling".

Likewise absence by name is a strong indication, not a verdict: in compressed ROFS
a name is not visible. But for kernel `dvfs_interface`, whose name resides in XIP, this is
solid evidence, and on E90 the device confirmed it.

### What run across 51 images showed (`firmware/_romscan.md`)

The DVFS driver is present in 25 images out of 51, and presence is not continuous even within
a single model — it appears starting from a specific firmware version:

- **RM-320** (our N95 8GB): present in all seven versions, from `10.0.021`.
- **RM-159** (regular N95): in early `10.x`–`12.x` there is not even the substring
  `dvfs`, appears from `13.0.003` and thereafter is always present.
- **RM-160** (N95 NAM): appears from `20.2.011`.
- **RM-245** (Chinese N95, without 3G/WiFi): absent in all seven versions.
- **N82 RM-313**: present in all.
- **E90, N93, N93i**: absent everywhere.

In contrast, PRCM pools — meaning the clock writing code itself in `power.dll` — are present almost
everywhere, including all E90 (except one version) and the rest where DVFS is absent. This
means that **the overclock mechanism (writing DPLL multiplier) exists on these devices, only the
stock frequency reader is lacking**. The exception on pools is `RM-245`, `RM-159 30.0.015`
and several others where the pool did not form into ≥3 consecutive literals; this is not proof
of code absence, but merely that literals are scattered, and need to be searched for differently.

### Practical conclusion for generalization

1. For **RM-320 of any version** the method ports almost as is: the driver is
   everywhere, in four versions the patch site is found immediately, in three — a
   targeted disassembler of `DoControl(1)` body is needed.
2. For **E90, N93, N93i, and early/Chinese N95** the READ primitive via DVFS
   is unreachable. There either another reader (RomPatcher+ channel, direct measurement as in
   `ocprobe`), or "blind" writing via `power.dll` with verification by operation.
3. The exact numerical address cannot be taken from the raw file by a separate base subtraction:
   XIP does not map as a single flat offset (verified — at the expected N95 base, instead of
   a ROM header lies a pointer table). The reliable path to addresses and the complete
   file list is the same one that gave us `rm-320/`: install the image in EKA2L1,
   extract `z:`, and read `romtool.ps1`. The device is not needed for this, but neither
   will a headless subagent perform this step — installation into the emulator is manual.
## 29. Adaptation method for an unknown device: what exactly is needed from ROM and how to get it

Written 2026-09-01. The purpose of this section is to make porting overclocking to a device
not in hand a repeatable procedure rather than a one-off reverse engineering effort each time. Below is exactly
what has to be extracted from the firmware, from where, and how it is verified.

### Main method refinement (corrects a naive assumption)

There is NO single linear correspondence of "file offset → virtual address" across the entire image.
Verified: the `dvfs` anchor gives a delta of `0x7f8881ee`, but the `power.dll` code cannot be read
at this delta — in RM-320, the PRCM pool with this delta would fall at `0x80130B00`, whereas the actual
power.dll clock functions are around `0x800B15xx`. The reason: FPSX stores ROM in blocks, and
each module has its own placement. Therefore, the real address can only be obtained from
`TRomImageHeader.iCodeAddress` of a specific module, that is, through normal
ROM unpacking. We do not write our own FPSX unpacker (an error in address costs
a device) — we use a verified one: installing an image into EKA2L1 yields `z:`, then
`romtool.ps1` reads `iCodeAddress` (offset 20 in the 120-byte header) and
disassembles with the correct address. This is "without the device", although not "without EKA2L1".

### RM-320 baseline (what to verify against)

`romtool.ps1 -Module power.dll -Disasm` on RM-320: `codeAddr=0x800A7C98`,
`codeSize=46592`. The key finding is in `out/asm/power.dll.asm`:

```
800b1560: ldr r0,[pc,#388]  ; 0x800b16ec -> literal 0xc6008540 (CM_CLKSEL1_PLL)
800b1564: bl  0x800b1c60     ; register READ helper: r0=address -> returns the value
800b1568..90: extract the multiplier field and multiply (this is DPLL = sysclk*mult/(div+1))
```

That is, power.dll contains: (1) a helper for reading a mapped register, (2) the multiplier
`0xc6008540` itself in the pool, (3) further — a write helper (the pair `str r0,[r1]; str r0,[r1,#4]`
from findings §22 / section 27). Registers — mapped address = physical `0x48008xxx` +
`0x7E000000` = `0xc6008xxx`, identical across all of OMAP2420.

### What to obtain for EACH device (adaptation checklist)

1. **Mapped base multiplier register** — expected `0xc6008540` (verify that it is
   present in the power.dll pool; present on E90 — section 27).
2. **Register read helper** in power.dll (vaddr) — to read clocks WITHOUT dvfs.
3. **Register write helper** in power.dll (vaddr) — or write directly from the stub.
4. **Whether dvfs_interface is present** (romscan by name): if present — the old READ path
   via `DoControl(1)` is available, and the offset of its body is needed; if absent (E90, early/Chinese N95) —
   read via the power.dll helper or do not read the register at all, but MEASURE the work.
5. **DPLL relock SRAM routine** (`KRomSramRoutine`) and **power-down global**
   (`KPwrDownGlobal`) — for safe multiplier change bypassing the DPLL; obtained from
   power.dll using the same disassembler.
6. **DSP divider setter** (`KDspSetter`) — optional, for the DSP domain.

### Implication for reading strategies

The method splits into two branches according to item 4:
- **dvfs is present** (all RM-320, N82, late RM-159/RM-160): port as is, only different
  addresses.
- **dvfs is absent** (E90, N93/N93i, early/Chinese N95): the READ primitive is different. Most reliably
  — do not read the register at all, but measure the actual frequency by work (like `ocprobe`
  `BestMem`): this is both stronger proof (measuring work, not a label — §5), and does not depend
  on any read address. Multiplier write remains via power.dll/relock.

That is, "adaptation for a third-party device" = fill in the profile from items 1–6 from unpacked
`z:`, and where there is no reader — rely on measuring work. The numbers are taken offline; the device
is needed only at the end to confirm that we guessed correctly.

## 30. XIP is extracted directly from FPSX without EKA2L1 GUI: `tools/fpsx_extract.js`

§29 relied on unpacked `z:`, which was provided by the EKA2L1 firmware installer
via File → Install → Device. This installer crashes on E90 and N82 (separate
frontend thread, writes no crash to the log), and the build has no command line for installing
firmware at all — the binary only has `--run`, `--listapp`,
`--device`, `--kbp`, while the installation itself is tied to a GUI dialog with
interactive variant selection. That is, mass or headless firmware installation in
EKA2L1 is not possible.

Therefore, XIP is now extracted directly from the firmware image, bypassing the emulator. The
FPSX/BB5 format is open in the EKA2L1 sources (`src/emu/loader/{include,src}` and
`src/tools/fpsxdump`), and `tools/fpsx_extract.js` is its exact port to Node.
This is extraction, not a guess: all addresses are taken from the ROM headers themselves after
reassembly, so there is no risk to the device.

Three things it rested upon, which had to be corrected compared to EKA2L1 master code:

- **Contiguous blocks, without alignment to 512.** The master code does
  `seek(align(data_size,512))` between blocks; in these images, the next block is located
  exactly at `data_offset_in_stream_ + data_size_`. Verified on the RM-320
  chain: `0x281+0xa0=0x321`, `0x334+0x240=0x574` — exact matches. All
  multi-byte FPSX fields are big-endian.
- **Determine CORE root by content, not by name.** In N95/N82, uncompressed XIP
  resides in a root described as `SOS+CORE`, in E90/N93/N93i — in `SOS+UPDAPP`, while
  the `SOS*CORE` there (asterisk!) is compressed and exhibits high entropy. The selector
  now searches for a root whose bytes start with an ARM vector table
  (`byte[3]==0xEA`, followed by branches `EA`/`EB`), and keeps the bootstrap preamble before it
  (in N95 this is 0xC00, in E90 — 0).
- **Split of OS into resident and paged parts.** N95/N82 keep the entire OS
  uncompressed (`SOS+CORE`, ~43 MB). N93/N93i/E90 keep uncompressed only
  a small resident XIP (`SOS+UPDAPP`, 1–1.8 MB), compressing the rest. Clocking code
  (power.dll) is in all cases in the uncompressed part, which is sufficient for overclocking;
  demand-paged applications are not needed by us.

**Ground-truth verification.** The image extracted from `RM-320_31.0.015_PR.C0R` matches
`SYM.ROM`, which EKA2L1 once unpacked itself, byte-for-byte in the code area; most importantly,
the multiplier field `0xc6008540` is located precisely at `vaddr 0x800b16ec`, just as in §29, and
the DPLL function prologue `0xe92d4010` — at `0x800b1544`. The mapping
`file_offset = vaddr − 0x80000000` holds. (The full EKA2L1 `SYM.ROM` is larger:
it has a tail appended up to the CORE partition size and several words overwritten in
the ROM header — checksums and timestamp; this does not affect the code area.)

**Run across all six OMAP2420 devices** (`out/roms/*.rom`) — each yielded
a valid XIP (`v0=0xea00007e`) with the multiplier register inside:

| device | XIP root | size | multiplier `0xc6008540` at vaddr |
|---|---|---|---|
| N95 RM-320 | SOS+CORE | 0x2959200 | 0x800b16ec |
| N95 RM-159 | SOS+CORE | 0x28e9200 | 0x800b16ec (same codebase) |
| N82 RM-313 | SOS+CORE | 0x29eb200 | 0x800b16fc |
| N93 RM-55 | SOS+UPDAPP | 0x115000 | 0x80045314, 0x800832e8 |
| N93i RM-156 | SOS+UPDAPP | 0x13e000 | 0x8003fdbc … 0x80097658 |
| E90 RA-6 | SOS+UPDAPP | 0x1b0000 | 0x800580e0, 0x8005eba8, 0x800b76dc |

E90 around its pool `0x800b76dc` disassembles into coherent ARM code operating on
PRCM registers — confirmed by `objdump`. Thus the E90 addresses from §29 can now be
derived offline, without installing firmware into the emulator and without the device.

Invocation: `node tools/fpsx_extract.js <image .C0R/.c00/.fpsx> [out.rom]`;
`--root=N` forces taking a specific root (diagnostics).

## 31. Clocking addresses across the entire line; and where offline adaptation of E90 reaches its limit

With offline XIP of all six devices (§30) available, it was possible to derive clocking
addresses from code and verify them. Three code anchors — the multiplier reader function,
DPLL relock SRAM routine (`iRomSramRoutine`), and DSP divider setter (`iDspSetter`) —
were found by signature (not by address) and **independently verified with a byte test**
in RM-320, RM-159, N82, N93, N93i; for E90, the SRAM routine and setter were confirmed.
Analysis of four devices was conducted in parallel by `agy` subagents, E90 and the baseline —
by the orchestrator. Summary table — `out/analysis/_summary.md`, step-by-step evidence —
`out/analysis/<dev>.md`.

Key numbers: RM-159 matches RM-320 byte-for-byte (the same N95 codebase).
N93 has the SRAM routine prologue `{r1-r6,lr}` instead of `{r1-r7,lr}` — r7 is unused,
the relock logic is the same. E90 setter has a mask of `0x1f`, like RM-320 (the adjacent twin
with a mask of `0x1f00` is a different field, not our setter):

| device | reader | pool 0xc6008540 | iRomSramRoutine | iDspSetter |
|---|---|---|---|---|
| RM-320 / RM-159 | 0x800b1544 | 0x800b16ec | 0x800a7f6c | 0x800ac59c |
| N82 | 0x800b1554 | 0x800b16fc | 0x800a7f7c | 0x800ac5ac |
| N93 | 0x800451e4 | 0x80045314 | 0x8003e6d0 | 0x80040c80 |
| N93i | 0x80046248 | 0x80046408 | 0x8003eb3c | 0x80041630 |
| E90 | (paged) | 0x800b76dc | 0x80056874 | 0x80059aa4 |

### Where offline E90 reaches its limit (and why this is not a tool bug)

The engine profile has four firmware-dependent fields. For E90, two of them
(`iRomSramRoutine`, `iDspSetter`) were derived offline above. The remaining two — were not, and the reason
is structural:

- **`iDvfsBody = 0`.** The stock READ primitive of RM-320 is a patch of the body of
  `dvfs_interface DoControl(1)`. The strings "dvfs" in the resident XIP of E90 exist, but they all
  pertain to **display** DVFS ("DvfsDisplay", "Driver_Tv_for_DVFS"), rather than
  CPU `dvfs_interface`. This confirms §27: there is no CPU dvfs on E90.
  (Re-verified on the extracted XIP — the conclusion of §27 is correct.)
- **`iPwrDownGlobal = 0xc8004194` — found (correction).** Initially it seemed that
  this global could not be derived offline. In reality, it can: in the power area of E90 there is a clean getter/setter pair
  — `str r0,[0xc8004194,#4]` (0x8005ebec) puts `obj` into `global+4`,
  `ldr r0,[0xc8004194,#4]` (0x8005ebf8) returns it. This is the exact equivalent of what
  the engine reads (`[iPwrDownGlobal+4] -> obj -> [obj+16] -> sram`). The init constructor
  sets `[obj+16]` to 0x800587d0, byte-for-byte like RM-320 0x800ab2c0. Thus three out of
  four fields of the E90 profile (`iRomSramRoutine`, `iDspSetter`, `iPwrDownGlobal`)
  were derived offline.
- **What remains is the READ primitive for runtime SRAM resolution.** The chain itself is known,
  but there is nothing to read `[obj+16]` on the device: dvfs is absent, and a suitable
  user-accessible resident driver whose body could be patched as
  a reader is not visible in the uncompressed XIP of E90.

**Conclusion.** The WRITE primitive (`patcher.ldd`, shadow pages) on E90 is available and
does not depend on dvfs. The READ primitive — is not. Per the user's decision, bootstrapping
read via write was chosen: patch a tiny kernel stub on the device that
traverses the already known chain `0xc8004194+4 -> obj -> obj+16` and returns the SRAM address;
from there, the full E90 WRITE path functions like on RM-320. It remains to find a user-accessible
E90 function whose body is safe to temporarily patch as the stub executor
(analogous to the role of dvfs DoControl on RM-320). All the rest of E90 has already been derived offline; the device
is needed only for this step — as planned.

## 32. E90 read/exec primitive: host found — DLddUsbcChannel::DoControl

The final field for the E90 WRITE path is a read/call primitive in kernel mode (on
RM-320 it is provided by dvfs, which is absent on E90, §27). Bootstrapping
via write was chosen (Pavlo's decision): using a RomPatcher+ write to patch the body of a user-openable
XIP driver. **The carrier is the standard USB client driver `DLddUsbcChannel`
(LDD name "Usbc").**

Why this one. The host must be (1) resident XIP (fixed address; a driver
from ROFS lands in dynamic kernel RAM, its address cannot be hardcoded), (2) user-openable,
(3) quiet during our window. Usbc satisfies all three: code is in the base
OMAP port (XIP), opens via `RBusLogicalChannel::DoCreate("Usbc")` with
CommDD, and without a cable the USB channel is unserviced — meaning that in our brief patch
window no one invokes its `DoControl`.

How it was found (without guesses). Dispatchers `HandleMsg` of all message-based LDDs in XIP
were enumerated with the signature `ldr rX,[r1,#24]; cmp rX,#0x80000000` (reading
`iValue` and checking for close). The Usbc dispatcher on E90 is `0x80095be8`, on
RM-320 — `0x8010ce3c`; they are **byte-for-byte identical in structure**, and on RM-320
it is densely surrounded by "Usbc*" strings — hence it is the same `DLddUsbcChannel`. DoControl
branch of the dispatcher (opcode ≥ 0):

```
ldr r2, [r4, #28]      ; a1  (msg->iArg[0])
ldr r3, [r4, #32]      ; a2
mov r1, r5             ; aFunction
mov r0, r7             ; this
bl  <DoControl>        ; RM-320 0x8010bd04, E90 0x80094ac4
```

that is, in `DoControl(this, aFunction, a1, a2)`, **a1 (address) is in r2**. The prologue
of both `DoControl` implementations is also byte-for-byte (`e92d4ff0 e24dd044 e1a04000 e1a06002 …`,
`mov r6,r2` preserves a1), so the patch site and register are identical on both devices.

Patches (r2 variant, unlike the r1 versions of dvfs §14):

| purpose | words | assembly |
|---|---|---|
| READ | `e3520000 15920000 03a00000 e12fff1e` | `cmp r2,#0; ldrne r0,[r2]; moveq r0,#0; bx lr` |
| EXEC | `e12fff12` (+ rest of prologue left intact) | `bx r2` |
| RESTORE (4 words) | `e92d4ff0 e24dd044 e1a04000 e1a06002` | stock prologue |

The READ stub does not touch sp (stock `stmdb/sub` is not executed), `bx lr` returns
r0 via `HandleMsg` into the user `DoControl` result. Removing the patch —
writing the original + `FreeShadowPage` (§19), as on RM-320.

Host addresses and checks:

| device | DoControl | multiplier pool (READ test) | expected `[pool]` |
|---|---|---|---|
| RM-320 | 0x8010bd04 | 0x800b16ec | 0xc6008540 |
| E90 | 0x80094ac4 | 0x800b76dc | 0xc6008540 |

Validation (instrument first, §discipline): RM-320 has an independent oracle in dvfs.
The `usbcboot` probe opens Usbc, installs the READ stub, reads `[pool]` via
`RDevUsbcClient::DoControl(0, pool)` and verifies against `dvfs.ReadOrCall(pool)` and the
known `0xc6008540`; then roll back and check rollback. Only after a match on
RM-320 does the same mechanism go to E90, where the known
`0xc6008540` itself serves as verification.

**Verified on RM-320 (2026-09-03).** `ocprobe usbcboot` (daemon, WiFi):
The Usbc channel opened (`DoCreate v0.0.0 -> 0`), the READ stub landed at `0x8010bd04`,
and all three known words were read via the host correctly —
`[0x80000000]=0xea00007e`, `[0x800b16ec]=0xc6008540`, `[0x800a7f6c]=0xe92d40fe`
(3/3), then rollback and `FreeShadowPage` both `-> 0`, device did not reboot
(uptime intact). The mechanism is proven on hardware with an independent oracle. E90 has
byte-for-byte the same `DoControl` prologue and dispatcher, host `0x80094ac4`.

## 33. Stock clocking of E90: bootstrap yields 266 MHz, not 330; multiplier reader is in XIP after all

Derived from `out/roms/e90.rom` (resident XIP, base `0x80000000`) and **verified
with our own `objdump`**, not merely a subagent report. The motivation was the obligation to "populate
stock E90 layout in `KProfileE90`": before placing numbers into the profile,
it was necessary to know where they came from and how much they could be trusted.

**Cold start sequence found in bootstrap code** (`0x80001910`–
`0x800019ac`). CM base is taken from pool `[0x80001e30]`, followed by direct writes to its
registers (offsets — from physical `0x48008000`):

```
80001924: ldr r1,[pc,#1316]  ; = 0x08301044 (pool 0x80001e50)
80001928: str r1,[r0,#0x240] ; CM_CLKSEL1_CORE <- 0x08301044
8000192c: mov r1,#2
80001930: str r1,[r0,#0x140] ; CM_CLKSEL_MPU   <- 2
80001920: str r1,[r0,#0x544] ; CM_CLKSEL2_PLL  <- 2 (CORE = DPLL*2)
80001940: str r1,[r0,#0x840] ; CM_CLKSEL_DSP   <- 0x000023c3
80001968: mov r1,#97 / mov r2,#6 / ... / str r7,[r0,#0x540] ; CM_CLKSEL1_PLL
```

The multiplier is selected via sysclk detection (bits [25:23] of `CM_CLKSEL1_PLL`): branches
yield `mult=133/div=5`, `mult=41/div=1` and — the typical one — `mult=97, div=6`. The latter,
with a 19.2 MHz crystal, is **DPLL ≈ 266 MHz**, meaning the cold start of E90 rests at
266, rather than 330. `CM_CLKSEL1_CORE = 0x08301044` here is also ITS OWN (L3=4, L4=2,
DSS1=16, SSI=3, USB=4) and is not equal to the stock runtime value of RM-320
`0x04360644`.

**However, this is a bootstrap value, not the OS runtime stock state.** The same OMAP2420 on
RM-320 starts just as early, and it is brought to 330 MHz (`mult 86`)
by the power manager later — precisely the mechanism that this project intercepts.
The transition of E90 to the operating frequency (and whether it is 330 at all) lies in the **compressed
ROFS part**, which is not present in the resident XIP: an exhaustive scan of XIP for a secondary
write to `CM_CLKSEL1_PLL` yielded nothing. Therefore:

- `iStockClkSel1Core` and `iStockClkSelMpu` for E90 remain **not populated from
  ROM** — the runtime layout is read from the device once the Usbc primitive is up.
  The bootstrap value must NOT be put into the profile: it is a description of an early phase, not
  where the device operates.
- `iStockMult=86` in `KProfileE90` — only a working guess for a 330 MHz device;
  bootstrap shows 97/266, so before any overclocking the multiplier must be
  confirmed by reading field [21:12] of `CM_CLKSEL1_PLL` on live E90. Until then,
  `Prepare()` for E90 refuses anyway (§27, `EOcStepUnsupported`).

**Correction of previous conclusion.** In `out/analysis/_summary.md` it was stated that
the E90 multiplier reader function resides in the paged part. This is false: in resident
XIP it **is present**, at `0x8005ea00`, with prologue `e92d4010 stmdb sp!,{r4,lr}` —
structurally identical to the RM-320 reader `0x800b1544`, — and loads the same pool
`0xc6008540` (`0x8005eba8`), calling reader veneer `0x8005f0c4`. Practical
consequence: E90 has a stock resident way to READ the multiplier; what is missing is only
a carrier capable of calling arbitrary kernel code, and the Usbc bootstrap remains that carrier.

## Reference: ROM images

Files from a ROM dump have `TRomImageHeader`, not `E32ImageHeader`. The header is
120 bytes, followed by code. Breakdown:

| Offset | Field |
|---|---|
| 0 / 4 / 8 | UID1 / UID2 / UID3 |
| 16 | iEntryPoint |
| 20 | iCodeAddress |
| 28 | iCodeSize |
| 60 / 64 | iExportDirCount / iExportDir |

Disassembly:

```
objdump -D -b binary -m arm --adjust-vma=<iCodeAddress> <file without the first 120 bytes>
```

In doing so, PowerShell parses an 8-digit hex literal as `Int32` and silently makes
it negative — take all constants via `[Convert]::ToUInt32($h,16)`.

## 34. XIP is readable from user mode; offline E90 layout disproven by hardware

This section corrects §31 and §33 regarding E90 addresses and removes the gate that held up the entire
E90 branch: a ROM reading primitive is no longer needed because ROM can be read directly.

**What was measured.** `ocprobe usbcboot e90` with addresses derived offline returned exactly
`0x00000000` three times in a row instead of the expected words, without rebooting
the device. Three probes separated the possible causes.

First — the signature stub `mov r0,#0x5a; bx lr`, which dereferences nothing. It
returned 0, meaning that what was written to `0x80094ac4` did not execute at all; thus the issue
was not with the assumption about register `r2`.

Second — a canary in the memory of its own process: `WriteBytes(&canary, 0xdeadbeef)`
was written and read back (`WRITE PATH OK`). Thus the `patcher.ldd` write path on E90
is alive, and suspicion is removed from the `ekern` export ordinals.

Third — direct reading of XIP from user mode (`ocprobe romread`). It
not only did not crash, but also gave an answer: `[0x80000000] = 0xea00007e` matches,
while `[0x800b76dc]` and `[0x80056874]` give completely different, but entirely meaningful ARM code
(`ldr r0,[r4,#52]`, `mov r0,#1`). The base `0x80000000` is correct, but the layout is not.

**Cause.** E90 addresses in §31 and §33 were derived from `out/roms/e90.rom` under the assumption
`vaddr = 0x80000000 + file offset`. For RM-320, this linearity was once verified against
unpacked `SYM.ROM`, for E90 — never, and §29 itself had warned that FPSX
stores ROM in blocks. `HAL EMemoryROM` on the device reports **19776 KB**, whereas
the extracted image is 1.7 MB, so offline we saw a smaller portion and in the wrong
layout. Because of this, the patch landed in someone else's page: the write succeeded, there was nothing
to execute, and nothing crashed — exactly the picture observed.

**Implication, more important than the mistake itself.** XIP is readable from user mode, meaning
firmware-dependent addresses can be found ON THE DEVICE by signature scanning, without
a kernel read primitive. This resolves the cyclic dependency that blocked the entire
offline branch ("to scan ROM, a read primitive is needed, and to obtain it — one must already know the first address").

Three modes added to the probe, all read-only:

```
ocprobe romread            three check words, base verification
ocprobe romscan            full scan of the live XIP for signatures
ocprobe romdump <a> <len>  dump a ROM window to E:\romdump.bin
```

**Scan result on live E90** (entire 19.3-megabyte XIP, without crashes):

| address | what it is |
|---|---|
| `0x8005a610`, `0x800610d8`, `0x800e640c` | pool with `CM_CLKSEL1_PLL` (`0xc6008540`) |
| `0x8005c338`, `0x800610dc` | pool with `CM_CLKSEL_MPU` (`0xc6008140`) |
| `0x800bf874` | prologue of `DLddUsbcChannel::DoControl` |
| `0x800c09a4` | `HandleMsg` dispatcher of the same driver |

Dumped windows were disassembled with our own `objdump`. The code around `0x8005a404`
loads `CM_CLKSEL1_PLL` into `r9` and operates via helpers `0x800615f4` (read),
`0x80061604` and `0x80061614` (write) — structurally the same as on RM-320, just at
different addresses. The dispatcher at `0x800c09a4` matches the offline one down to the instruction:
`cmp r5,#69` passes our opcode past the USB readiness check, and `ldr r2,[r4,#28]`
puts the address into `r2` before `bl 0x800bf874`.

**Primitive on E90 proven (2026-09-09).** With measured host `0x800bf874`,
`ocprobe usbcboot e90` returned signature `0x5a` (stub executes) and **3/3** check
words: `[0x80000000]=0xea00007e`, `[0x8005a610]=0xc6008540`,
`[0x800c09a4]=0xe5915018`. Rollback and `FreeShadowPage` both `0`, `startTick`
unchanged throughout the entire session — the device did not reboot a single time.

**What follows from this for §31 and §33.** All numbers in those sections obtained from
`out/roms/e90.rom` via a linear map must be considered invalid until
re-measured on hardware. This applies to `iRomSramRoutine`, `iDspSetter`,
`iPwrDownGlobal` (zeroed out in code) and the conclusion of §33 regarding bootstrap with `mult=97`.
The method of §30 (extracting XIP from FPSX) is not itself disproven — what is disproven is the linear
offset→vaddr map on top of it, specifically for E90 where XIP is split.

### 34.1. Stock clocking of E90, measured on hardware

Using the same primitive, read-only, the PRCM register file of a live E90 was dumped
(2026-09-09, Bluetooth):

```
CM_CLKSEL1_PLL  = 0x00056400   mult=86  div=4  sys_clk_sel=0 (19.2 MHz)
CM_CLKSEL2_PLL  = 0x00000002   core_mult=2
CM_CLKSEL_MPU   = 0x00000002   mpudiv=2
CM_CLKSEL1_CORE = 0x04360626
CM_CLKEN_PLL    = 0x0000000f   DPLL in lock
```

According to the formula of §4: `19.2 × 86 / (4+1) = 330.24 MHz` on DPLL, `× 2` on CORE, and `/ 2`
on MPU, that is, **E90 stock runs at the same 330.24 MHz as RM-320**.
The working guess `iStockMult = 86` was confirmed — but now it is a measurement, not a guess,
and in the profile it stands as a measurement.

The layout `CM_CLKSEL1_CORE = 0x04360626`, which was not visible offline at all
(initialization code lies in the compressed part), was obtained completely. `KProfileE90`
was populated with these three numbers.

Caveat: this is a single point-in-time snapshot. There is no CPU DVFS on E90 (§27),
so the layout should be constant, but before a WRITE run it must be re-read —
the rule "do not believe a label without measurement" applies to one's own previous measurements as well.

The numbers `mult=97` / 266 MHz from §33 do not pertain to the working stock state of the OS, and the
addresses of §33 themselves were derived from the layout disproven by hardware (§34).

## 35. Carrier of the primitive moved to profile; E90 reboot bought with a single missed line

For overclocking to work not only on RM-320, the carrier of the READ/EXEC primitive was moved into
the profile (`TOcHost`): on RM-320 this is `dvfs_interface` with `DoControl(1, a1)` and
address in `r1`, on E90 — `Usbc` with `DoControl(69, a1)` and address in `r2`. Patch mechanics
are common; what differs is the channel name, opcode, argument register, rollback words,
and rollback verification method. Duplicating the relock sequence is unacceptable — two copies
diverging would mean hangs — hence parameterization, not a second engine.

**E90 reboot, and why it happened.** The first run of the new path took down the device
instantly. The cause — exactly one missed line: `KReadPatch` (r1 variant)
was substituted according to the profile in `ApplyReadPatch()`, but in `Prepare()` there is a SECOND call
applying the same patch, and it was left with the old constant. On the Usbc carrier,
the dispatcher puts the opcode into `r1`, which is 69, while the stub does `ldrne r0,[r1]` — reading
at address 69 in kernel mode. The device rebooted and came back up on its own.

The lesson is narrower and more useful than "be attentive": **when a constant becomes profile-dependent,
one must search for ALL its occurrences, not just the one encountered first**. Here even a trivial
`grep` for the constant name after editing would have helped — and that should be made
a rule.

**Three safeguards added after this.**

First — `Prepare()` refuses when the profile has zero not only in `iDvfsBody`, but also in
`iRomSramRoutine` or `iPwrDownGlobal`. Zero means "not yet measured", and patching
at a zero address or checking SRAM against zero is a guaranteed way to bring down
the device.

Second — verifying that the patch is ALIVE before trusting it. On the Usbc carrier, zero from
stock `DoControl(69)` is indistinguishable from zero read by the primitive, and that was
precisely where the bootstrap was once lost (§34). Now `PrepareProbe()` first reads
the reset vector and requires `0xea00007e`.

Third — rollback verification no longer calls the kernel. For `dvfs`, it relied on the fact
that `DoControl(1,0)` returns a percentage of nominal; for `Usbc` there is no such oracle,
and calling stock `DoControl(69)` with non-zero `a1` is dangerous — it is an actual
driver function, and what it might do with a foreign pointer is unknown. Instead,
six words of the body are verified by direct reading from user mode, because XIP on this device
is readable (§34). This is stronger proof than any indirect one: we are looking at the bytes
themselves.

**Regression on baseline (2026-09-09, N95 RM-320 over WiFi).** Both paths intact.
`patcher dsp` — `engine ready: SRAM @cb202000, stock mult 86`, `CM_CLKSEL_DSP =
0x000023c3`, `restore check -> 50`. `patcher oc 107` — full write cycle:
328 → 408 MHz (`RESULT NOP 24.3%`, `ALU 24.4%`, `MEM 24.4%`), five soak
rounds with zero corrupted words, return to multiplier 86 with `status=0`.
Uptime 722 minutes, device did not reboot.

**E90 status.** Profile populated with measured addresses (`iDvfsBody = 0x800bf874`,
`iRomSramRoutine = 0x80058da4`, `iPwrDownGlobal = 0xc8003068`, stock layout
from §34.1), code fixed, but verification on hardware could not be performed: after the reboot,
`Open4all` patches were disabled on E90, and without them writing to `E:\sys\bin` returns `-46`.
They can only be enabled manually on the device itself. The next step, as soon as this
is done — `ocprobe patcher dsp e90`: it runs the entire new path without writing
a single bit to PRCM.

### 35.1. Stock E90 bus is twice as slow, and why this changes the overclocking plan

Decoded stock layout of both devices at the same core frequency:

| device | CM_CLKSEL1_CORE | l3div | dss1div | CORE | L3 bus | DSS1 |
|---|---|---|---|---|---|---|
| RM-320 | `0x04360644` | 4 | 6 | 660.48 | **165.12** | 110.08 |
| E90 | `0x04360626` | 6 | 6 | 660.48 | **110.08** | 110.08 |

That is, E90 has the same core at 330.24 MHz, but the memory bus is one-third lower.
This is not a detail: the divider compensator targets the ceiling `kL3Max = 213120`, and this
number was MEASURED on RM-320, representing precisely the bus at which RM-320 operates daily.
On E90, the same ceiling would allow `l3div = 4`, i.e. 205 MHz compared to stock 110 —
almost twice as high, without a single measurement. This would almost certainly bring down the device.

Fixed by a relative limit instead of an absolute one: **the L3 divider never
drops below the stock value for this device**. There are three consequences, and all are desirable.
The bus rises in exactly the same proportion as the core — a mode proven on
RM-320 by daily operation. Return does not touch the L3 divider at all, so
we bypass the exact operation that brought down the device three times on RM-320 (§26.12, §26.14,
§26.15). And on RM-320 itself nothing changes: there the ceiling already yields stock 4,
as confirmed by a repeat run of `patcher oc 107` — the same `l3div=4 dssdiv=8`
and the same 328 → 408 MHz.

For E90, this means a cautious first step: with multiplier 107 the bus becomes 136.96 MHz,
which is lower than RM-320 has at STOCK, while the divider remains unchanged at 6.

### 35.2. E90 write path: what has been verified offline and what remains for hardware

Before the first overclocking run on E90, everything that could be verified without hardware was verified,
because every unverified assumption here costs a reboot.

Blob `OcRelockBlob` carries two hardcoded register bases. Both are confirmed by the pool
of the E90 relock routine itself (`0x80058ed8..0x80058ee4`): `SDRC = 0xC6129000`,
`CM = 0xC6008000`, and nearby `CONTROL = 0xC6014000` and DLL constant `0x770E` —
exactly the same set as on RM-320. That is, peripheral mapping on E90 is the
same, and the rule `+0x7E000000` for the `0x48xxxxxx` window applies here too.

Bridgehead in SRAM: the E90 constructor maps a chunk with `mov r2,#4096` and copies into it
`mov r2,#1024` bytes of the procedure — both numbers are the same as on RM-320. Therefore
`KSramSlot = 0x800` falls inside the chunk and does not overwrite the procedure copied by the firmware,
as intended.

What remains unverified and can only be verified on hardware: the linear SRAM address itself
(resolved at runtime via `iPwrDownGlobal`), matching of the first word of the copy with
ROM (engine checks this itself and refuses on mismatch), and, most importantly, whether the
E90 silicon will withstand the multiplier increase. Run order — first `patcher dsp e90` (no writes to PRCM), then
`patcher oc 95` as the first write. Specifically 95, not 107: on a new device, the first
step must be the smallest one that proves anything at all. At 95 the bus will become
121.6 MHz, i.e. a tenth higher than stock 110, while the ramp proceeds one
step at a time anyway and halts at the first refusal. If 95 is clean — proceed to 107.

## 36. E90 OVERCLOCK ACHIEVED: +29% on the second device, portability proven

Nokia E90 (RA-6, 400.34.93) overclocked at runtime with the same engine as
RM-320. This is the first device added to the project not by rewriting code, but by
populating a profile, which is why the result is more important than the megahertz alone: it
proves that the method is portable.

| multiplier | MPU | L3 bus | measurement | status |
|---|---|---|---|---|
| 86 stock | 330.24 | 110.08 | 328 MHz, 17.34 Mitr/s, 344 MB/s | — |
| 95 | 364.80 | 121.60 | 362 MHz (+10.3%) | clean |
| 107 | 410.88 | 136.96 | 408 MHz (+24.3%) | clean |
| **111** | **426.24** | **142.08** | **423 MHz (+28.9%)** | **clean** |

Each run — ramp by one step, `status=0` on all, five soak
rounds with zero corrupted memory words, return to stock multiplier and
a control measurement that yielded exactly the initial 328 MHz each time. Across all three
runs the device did not reboot: `startTick` remained the same as before them.

Three numbers scale together and in the same proportion — 28.9% on NOP, 29.1% on ALU,
29.3% on memory, — meaning this is a genuine clock frequency change, not an indicator
shift. The instrument here is the same as on RM-320, and there is no reason
to doubt it.

**What this took, besides the multiplier itself.** The E90 profile has no number
taken out of thin air: host `0x800bf874`, relock routine `0x80058da4`, and power-down
global `0xc8003068` were measured by signature scanning of live ROM (§34), while stock
layout was read from registers (§34.1). Carrier of the primitive became a profile field
(§35), and the L3 divider no longer drops below stock for the device (§35.1) —
without this limit, E90 would have received a 205 MHz bus against stock 110.

**Side observation about the hardware itself.** E90 has the same core at 330.24 MHz,
but a stock bus of 110.08 MHz versus 165.12 on RM-320, and this is visible in measurement: 344 MB/s
versus 519 at stock. At the same time, the core overclocking ceiling is identical on both — 111 holds
cleanly, and in both cases what reaches the limit is not the bus, but ARM1136 itself. That is, the narrower E90 bus
limits bandwidth, but does not prevent the core from reaching the same +29%.

What was NOT tested: long-term daily operation at 111 (on RM-320 this is a week
of observations) and behavior above 111. The E90 ceiling was deliberately not sought — there was no
need, and each attempt above costs a reboot.

## 37. Automatic profile selection by device; GUI works on both

The application has no command line, so the method used by the probe to select a profile
(the word `e90` in arguments) is unsuitable for it. And a profile consists of four addresses
patched into the kernel: picking a wrong one means bringing down the device.

Profile is now selected by `HAL EMachineUid`:

| device | EMachineUid |
|---|---|
| N95 8GB (RM-320) | `0x20002D84` |
| Nokia E90 (RA-6) | `0x20002496` |

Both taken from devices, not from documentation. There is deliberately NO fallback
option of "unrecognized — use baseline": an unknown device receives an honest refusal before
any touch to the kernel. This is not theoretical caution — the very first check
triggered in this exact way because I made an error converting decimal values to
hexadecimal, and the device was not recognized. Instead of patching wrong addresses, a line
in the log was produced.

Autoselection also became the default in the probe. Previously, `patcher oc 111` run on E90
without the word `e90` would have taken RM-320 addresses — a separate landmine eliminated by this; the word
remained as an explicit override for checks.

**Capability `CommDD` required.** On E90, the carrier of the primitive is the USB client driver,
and its `DoCreate` without `CommDD` returns `-46`. The probe had it, the application
did not, so the first attempt yielded `NOT READY (-46)` on perfectly working code. Added to
`ocgui.mmp`. The trap here is not in the capability itself, but in the fact that ABLD does not rebuild
the target after editing `.mmp` — the E32 header must be verified (`elftran -dump h`), not
assumed.

**Verified on both devices.** E90: `stock m86 CPU 330 BUS 110 DSS 110`,
`CLKSEL1_CORE 04360626`, `BUSDIV /6`, `DSPDIV /3 DSP 220`, engine ready.
N95: `stock m86 CPU 330 BUS 165 DSS 110`, `CLKSEL1_CORE 04360644`, `BUSDIV /4`.
That is, each device sees its own layout, and neither sees the other's.

Known cosmetic limitation: interface layout was designed for a screen 320 pixels
wide, so on 800x352 on E90 it occupies a narrow column on the left. Readable, but
space is used poorly.

### 37.1. Latent flaw in `.gitattributes`: `.pkg` checked out with LF

Found during package build, not at runtime: `.gitattributes` described `*.pkg` as
`text eol=lf`, meaning both in the index and **in the working copy upon checkout**. And
`makesis` silently corrupts a file with LF — this is explicitly stated in a comment inside
`ocgui.pkg` itself and in the header of `tools/build_sis.cmd`.

On this machine this did not surface only because the working copy was never
re-checked out after `.gitattributes` appeared. A fresh clone would have received a broken
package, and the symptom would have been precisely the kind hardest to trace to the root cause: the build
succeeds, signing succeeds, but the package behaves abnormally.

Fixed to `*.pkg text eol=crlf`. Verified not by reasoning, but by action: file was
deleted, re-checked out from git, and bytes were counted — 42 CRLF, not a single lone
LF, not a single byte outside ASCII; afterwards the package was rebuilt from this very copy.

## 38. CORE dividers most likely not written; §22 conclusion loses support as a result

This section corrects §22 and weakens §26. Source — experiments by Pavlo on both devices
(2026-09-09), conducted using the memory bandwidth benchmark integrated into the GUI.

**Observation.** Changing the system bus divider yields no change in memory speed
— neither on N95 nor on E90. The display divider likewise does not affect the behavior
of the external E90 screen: beyond a certain frequency threshold it simply goes black, and
below the threshold it flashes black once every few seconds, with movements of `DSS1` not changing this.
The internal screen is stable across all modes.

**Why this breaks §22.** There, the limit was attributed to the core silicon based on
a configuration with multiplier 129, in which "both the bus and the display are set EXACTLY to
stock frequencies". This assertion rested on the INTENT to write, rather than reading back.
If the write was rejected, the bus at multiplier 129 was tracking CORE at
`990.72/4 = 247.68` MHz instead of the stock 165.12, and the immediate reboot is explained
by the bus, not the ARM1136. Thus the conclusion "the limit is MPU silicon itself" is no longer
supported; it is not disproven, but there is no proof beneath it either.

**Independent confirmation from the second device.** E90 takes multiplier 129 (495 MHz)
without issue. On E90 stock `l3div=6`, so at 129 its bus yields
`990.72/6 = 165.12` MHz — exactly the frequency that is STOCK on N95. That is, at one
and the same multiplier the devices differ precisely by bus: 165 versus 248 MHz. This is
direct evidence in favor of the bus being the limiter rather than the core, and it aligns
with E90, having a lower baseline bus, reaching further.

**A contradiction we do not gloss over.** §26.12 recorded a run of `l3=8` at
stock multiplier yielding `MEM -45.0%`, meaning the divider TOOK EFFECT there. Together with
the observations above, this means the correct phrasing is not "dividers never
work", but "writing to `CM_CLKSEL1_CORE` is rejected silently and more often
than we thought". Hardware rejects the ENTIRE write if even one field is invalid (§26),
and the set accepted by the hardware is narrower than what the firmware validates. The configuration
in §22 included `dss=9` — a highly probable candidate for refusal.

**Flaw in the instrument that let this survive a month.** The engine never read
`CM_CLKSEL1_CORE` back after applying. All logs reported what we
intended to write, and none — what ended up in hardware. Fixed: added
`COcEngine::LiveCore()`, and now each probe run and each `CONFIRM` in
the application print the live register alongside the intent and the verdict
`DIVIDERS APPLIED` / `DIVIDERS REJECTED`.

**Method rule (from Pavlo, adopted).** Divider functionality is verified by
MEASUREMENT — the integrated memory bandwidth benchmark — rather than a line in
a log. Reading the register back complements this, but does not replace it: a register may accept
a value while the frequency does not change. Both pieces of evidence must converge.

**What follows for future plans.** The question of "where the RM-320 limit is" reopens
anew, and the cheapest experiment is now different: rather than seeking the multiplier ceiling, check
whether N95 holds a high multiplier with the bus brought to 165 MHz by a higher divider.
However, it can only be performed after reading back confirms that
the divider is written at all.

### 38.1. Cause found: application did not write layout if multiplier was unchanged

Pavlo's clarification narrowed the search decisively: his observations pertain to the GUI
PATH, whereas `MEM -45%` in §26.12 was obtained via the probe. Thus
there was no contradiction — the write paths themselves were different.

The application applies the layout with a single call to `ApplyRampWithCore()`, and the entire
write in that method resides inside the loop

```
while (cur != aTarget) { ... ApplyMultAndCore(cur, ...) ... }
```

When the user changes the dividers ALONE without touching the multiplier — which is the primary way
to use them, — `cur` already equals `aTarget`, the loop does not execute a single
time, and NOTHING is sent to hardware. Externally this looks exactly like "dividers do not
work": neither bus nor display reacts because no write took place.

The probe did not show this because `patcher clocks` after the ramp performed a SEPARATE explicit
write of the final layout (step 2) and even checked it by reading back. That is, the two
paths diverged, and it was this divergence that hid the bug.

Fixed with two changes, the second being more important than the first.

First, `ApplyRampWithCore()` now performs an explicit final write after the loop
when dividers are specified. The write is idempotent: if there was a ramp, the last step already
set the same values.

Second, `patcher clocks` no longer has its own write path — it calls the
same `ApplyRampWithCore()` as the application. Now a probe run is a true
verification of what the GUI does, rather than an adjacent implementation. Duplication here was
the actual flaw: one branch worked, the other did not, and no log distinguished between them.

**What this means for previous conclusions.** All of Pavlo's observations regarding dividers
are explained by this bug, and §38 should be read with an amendment: the question of "does the
hardware accept these values" remains OPEN, because through the GUI it was never
set in the first place. The conclusion of §22 is not reinstated by this — there, too, no one
verified the write by reading back — but it is not definitively disproven either.

**Not verified on hardware.** At the time of the fix, E90 returned `-46` (disabled
`Open4all` patches), and the N95 daemon was not listening in its subnet, so measurement
was deferred. First run after restoring communication: `patcher clocks mult=86
l3=<stock> dss=8` — changes only the display divider, is safe for SDRAM, and
reading back will immediately reveal whether writing now takes place.

### 38.2. Bus divider works on both devices; restoring bus back is fatal

Following the fix in §38.1, the bus divider was finally measured using the instrument that
Pavlo described as the only honest one — the integrated memory bandwidth
benchmark. Both devices gave an unambiguous result: the divider **works**.

```
E90   l3div 6 -> 8:  CPU  0.0%   MEM -21.0%   layout confirmed by read-back
N95   l3div 4 -> 8:  CPU +0.6%   MEM -44.9%   layout confirmed by read-back
```

Domain separation here is complete: core frequency did not change on either E90 or N95
(0.6% is NOP measurement noise), whereas memory dropped exactly as dictated by
the divider. This simultaneously closes the question of §38 and confirms the method: logs of write
intent proved nothing, while two numbers side by side do.

The layout holds under load: ten seconds of observation, zero
corrupted words in one megabyte of SDRAM on both devices, no drift. That is, lowering
the bus itself is a clean operation.

> CORRECTED 2026-09-11 (§39): the general conclusion of this paragraph — "the bus cannot
> be raised" — is incorrect. On E90 the bus was raised from stock to above stock, the device
> survived and yielded +41% in 3D. What remains true is the fact of four deaths and that all
> of them occurred upon RESTORING the bus from an already lowered state.

**The reverse operation kills the device.** Restoring `l3div` to its stock value
yielded four deaths out of four: three times on RM-320 (§26.12, §26.14, §26.15) and once
on E90 today, through both paths — full relock and lightweight
`ReassertDividers` without DPLL bypass. The log breaks off at this exact
write every time.

Today's death of E90 simultaneously **disproved the explanation in §26.15**. There, the cause
was sought in the DVFS governor supposedly interfering during the bypass window. On E90
there is no governor at all, `sel2` equaled 2 the entire time, the bus stood at exactly
82.56 MHz, and the increase was only 1.33x versus 2x on N95 — yet the device
perished anyway. Thus, it is not about the governor or the magnitude of the jump, but about the very
asymmetry of the operation: lowering the bus is possible, raising it is not.

The practical conclusion is simple and has already been implemented in code. `RestoreStock`
refuses to raise `l3div` back, sets flag `RestoreNeedsReboot()`,
and leaves the device on the lowered bus — this state has been measured as safe. The probe
and GUI tell the user to reboot, and rebooting restores stock clocking
guaranteed, because nothing is written to flash. The safeguard lives in the engine, not the
application: the probe previously bypassed it and called `ReassertDividers`
directly with RM-320 constants, which on E90 was doubly wrong.

Verified on hardware: N95 passed the full cycle of `patcher clocks l3=8 sec=10` to
the end and **stayed alive** — for the first time on this operation. The log shows the line
`BUS LEFT LOWERED ON PURPOSE`, followed by stock final state reading and
`restore check: stock DoControl(1,0) -> 50`, meaning kernel code was restored in place.

Side observation from the same run: during the ten-second soak,
the N95 governor switched `sel2` from 2 to 1, halving CORE (330 MHz
instead of 660) and with it the bus down to 41 MHz. The device survived this. This does
not happen on E90, which once again confirms §35.1.

### 38.3. Bus guard is placed at the write point, not only in restore

Refusing to raise the bus in `RestoreStock` closed only one path. The same
fatal write is easy to produce via regular apply: it is enough to lower the
bus in GUI and then return the divider back — that is no longer "restore", but a routine
`APPLY`, and it would go into hardware without any caveat.

Therefore, the safeguard was moved into `ApplyMultAndCore` — the single point through which
any layout write passes. It does not refuse the write: refusal
would block overclocking after any bus change. It **clamps** field `l3`
to the live value, so the multiplier changes as requested, while the bus remains lowered.

```
E90, live l3div=8, requested l3=6:
  bus clamp: l3 6 -> 8 (raising the bus is 4/4 fatal; reboot for stock)
  layout confirmed by read-back
```

At the same time, the instruments themselves had to be fixed. The probe and GUI verified the live register against
what was ORDERED, so a deliberate clamp would have been shown by them as "hardware rejected
the write" — the instrument would be lying about its own operation. Now both compare against
`LastCoreWritten()`, that is, what the engine actually issued to hardware, and indicate with a separate
line that a clamp occurred.

The display divider on E90 was also measured in the process: `dssdiv` 6 → 8 takes
(`DSS1 110080 → 82560 kHz`, read-back matches), and the reverse change 8 → 6
also takes. That is, this divider, like the bus divider, works — the question of the external
screen flashing pertains not to register writes, but to the behavior of the panel itself.

### 38.4. N95 reaches 491 MHz when keeping the bus below stock

Pavlo's hypothesis — that N95 was hitting a limit not in silicon, but in its own stock bus of 165 MHz
versus 110 on E90 — was confirmed on the very first run.

With the lowered divider (`l3div=8`), the device completed the ramp 86 → 129 one
step at a time without a single non-zero status and sustained 15 seconds of load:

```
AFTER : 491 MHz (NOP), 432 MB/s (MEM)
RESULT: CPU 50.1%  MEM 50.5%
SDRAM retention over 10 s: 0 bad words in 1 MB
drift samples: 0
```

Until then, the N95 ceiling had been 111 (426 MHz). Now it is 129 — exactly the multiplier that
works on E90 with stock dividers, and the same frequency of 495 MHz.

The numbers converge if the governor is taken into account. Snapshots taken at idle show
`sel2=1`, from which the formula yields CORE 495 MHz and MPU 248 MHz. But NOP measures 491 MHz,
while memory yields +50%, and both numbers match `sel2=2`, that is, CORE 990.72 MHz,
MPU 495.36 MHz, bus 123.84 MHz. This is the same discrepancy between snapshot and measurement
discussed in §12 and §18: under load, the governor raises OPP, while the snapshot
catches the lower one. The instrument here is not contradicting itself — it is measuring two different moments.

That is, the operating bus during the run was 123.84 MHz, lower than stock 165.12, while
the multiplier was 18 points higher than the previous ceiling. The limiter was the bus.

**What has not yet been done.** The threshold has not been narrowed: it is unknown whether N95 will take multiplier 129
with `l3div=6`, that is, with the bus at exactly 165 MHz, as on E90. This experiment requires
a reboot (raising the bus at runtime is prohibited by the safeguard), and it is worth
doing as the first next step.

### 38.5. Why "dividers do not work": safeguard clamps silently, while UI displayed intent

Pavlo reported on 2026-09-10 that on both devices forcing a memory divider change
does nothing, while N95 above a multiplier of about 111 also raises
the divider on its own without showing this in the UI. Both observations are correct, and
they share a single cause — our own deliberately added behavior that was not visible.

First — the safeguard of §38.3. `ApplyMultAndCore` clamps field `l3` to the live
value as soon as the ordered divider is SMALLER than what is set in hardware, because
a smaller divider means raising the bus, which killed the device four times out of four.
Both devices remained on `l3div=8` after the experiments of §38.2 and had not been
rebooted since, so any attempt to lower the divider (i.e. raise the bus)
was silently replaced with 8. Externally this looks exactly like "the divider does not work".
Cleared only by a reboot: after it the bus is stock, and the divider is again controllable
downward — until the first raise within the session.

Second — bus ceiling in `CompensatedCore`. The divider is selected such that the bus does
not exceed 213120 kHz, so on RM-320 when stepping from multiplier 111 to 112,
selection automatically shifts `l3div` 4 → 6: `990720/4 = 247680` is already above the ceiling. In
auto mode, line `BUSDIV` showed this value, but when the divider was set
manually, the set value was displayed while the clamped one went into hardware.

Fixed in 1.7.0 by display rather than changing behavior: line `BUSDIV` prints
`BUSDIV /4 CLAMPED /8` along with the frequency that will actually be produced by
the clamped divider; line `armed` separately states `bus stays /8 clamped`; after
CONFIRM the old comparison with `LastCoreWritten()` remains. The safeguard itself was not
touched — it rests on four deaths and is cleared by a reboot, not by code.

### 38.6. DSS1 divider on E90 resolved the issue with the external screen

Pavlo confirmed on 2026-09-10 that changing the display divider on E90 helped. This
resolves the doubt in §38.3: there, it had only been measured that writing to field DSS1 is accepted
in both directions, leaving open whether it produced the desired effect on the panel.
Now it is known that it does.

### 38.7. Slider ceiling raised 129 → 255

The limit of 129 was ours, not hardware: the multiplier field `CM_CLKSEL1_PLL[21:12]` has
ten bits, step — 3.84 MHz. 129 was once chosen as exactly +50% from stock 86,
a value convenient because 129/86 = 1.5 and all accompanying dividers scale
by an integer.

Raised at Pavlo's request to 255, because the former N95 ceiling of 111 turned out to be the bus rather
than the core (§38.4), meaning the true silicon limit is unknown. Above 129 nothing has been verified,
and the GUI with the probe state this aloud in a separate line. What exactly could
stop it there: DPLL lock (safe — the blob returns a non-zero status and
rolls back on its own), silicon at stock voltage, bus (above 129 selection can
lower it only in coarse steps 8 → 12 → 16), and the SDRAM autorefresh window, which
we only read and never compensate: at `l3div=8` it is already stretched from
7520 ns to 13016–19525 ns.

### 38.8. Bus guard became a toggle: `BUS GUARD`

Pavlo reported on 2026-09-11 that lowering the divider to 4 on E90 still does not
work, while on N95 the memory divider still resets above multiplier 111. Both
observations are correct, and §38.5 explained the reason, but changed nothing in essence:
displaying the clamp is not controlling it. The prohibition remained absolute, whereas
the user needs the ability to experiment on their own device.

Therefore, the safeguard was made a toggle, enabled by default. In the GUI it is a separate line
`BUS GUARD: ON / OFF  raising bus died 4 of 4` immediately below `BUSDIV`; in the probe —
flag `guard=0` to `patcher clocks`. Disabling removes both the clamp in
`ApplyMultAndCore` and the refusal in `RestoreStock`. This is exactly the same scheme as
the rest of the project's dangerous paths: the dangerous is accessible, but only by explicit
choice, and never by default.

What remains true in doing so, and which the toggle does not change: raising the bus is still
four deaths out of four (§38.2), on two devices and by both paths, and not a single
time was the bus raised successfully — even above stock was never attempted at all.
The price of failure is known and limited: nothing is written to flash, the worst is an immediate
reboot.

Separately, it should be kept in mind that a useful target on N95 at multiplier 129 is
`l3div=6`, that is, exactly stock 165.12 MHz bus, rather than `l3div=4`: a four with
CORE at 990.72 MHz would give 247.68 MHz, i.e. 1.5x stock, which no one has attempted.

### 38.9. Limit of ARM1136 without voltage increase — around 500 MHz

Pavlo stated this figure on 2026-09-11 as a known characteristic of this processor. It
aligns with our own measurement: N95 stably held 491 MHz at multiplier
129 (§38.4), which is effectively right up against it. Thus raising the slider ceiling to 255
(§38.7) by itself will not yield frequency — without voltage control there is nothing
to expect above 500 MHz, and practical sense lies with the bus rather than the multiplier.

## 39. BUS RAISING WORKS: E90 at +50% memory yields +41% in 3D

On 2026-09-11 Pavlo turned off `BUS GUARD` (§38.8) and raised memory frequency on E90 by
50%. The device survived, and the gain proved to be the largest of anything measured in this project:

```
E90, large screen
  stock             16.33 fps   memory 339 MB/s
  bus +50%          23.00 fps   memory 463 MB/s
  bus overclock     24.55 fps   memory 509 MB/s
```

The arithmetic converges exactly and confirms by which exact path this was done. Stock
E90 bus is 110.08 MHz at `l3div=6` and CORE 660.48 MHz (§35.1); `l3div=4` yields
exactly 165.12 MHz, which is the same +50%. Thus it was raised by the divider, via the very same
`L3` field in `CM_CLKSEL1_CORE`, and in the very direction previously considered
fatal.

**This corrects §38.2.** There, it was recorded that raising the bus is completely impossible, and
the conclusion was phrased as an asymmetry of the operation itself: "lowering the bus is possible,
raising it is not". It is now known that this is not true in the general case. What remains
true — all four deaths occurred upon RESTORING the bus from an already lowered state, rather than upon
raising from stock; none of them was a raise above stock. That is, what kills is not the direction
per se, but something in the transition specifically from a lowered state, and what exactly — is as yet
unknown. This is a guess, not a conclusion.

**The main takeaway from the result is not frequency, but where the device hits its limit.** A gain
of 41% in 3D from memory alone, with an unchanged core, means that ARM1136 in 3D
is almost entirely memory-bound. This aligns with the measurements of §38.2, where changing
the bus divider shifted `MEM` by 21–45%, while not shifting `CPU` at all. And this casts
doubt on project priorities: the multiplier hits a silicon limit around 500 MHz
(§38.9), while the bus just yielded more than any core overclock before it.

### 39.1. E90 scale: bandwidth follows the bus almost exactly, 3D follows suit

Clarification from Pavlo on 2026-09-11: 509 MB/s is pure 50% bus overclocking, without
the multiplier; 463 MB/s is the same state in another run. Plus one more point,
bus +50% together with multiplier 105.

```
E90, large screen         bus        CORE     MPU     MB/s    fps
  stock                  110.08     660.48  330.24     339   16.33
  l3div 6 -> 4           165.12     660.48  330.24     463   23.00
  l3div 6 -> 4 (repeat)   165.12     660.48  330.24     509   24.55
  l3div=4 + mult 105     201.60     806.40  403.20     569   28.20
```

**The two middle rows are one and the same state measured twice.** Hence
the primary figure of this table, and it is not about frequency: the spread of repeated runs
of the same configuration is 10% in memory and 6.7% in fps. The cause is not
measured. The most probable candidate is the display controller sharing the same
bus, so the share it consumes depends both on the DSS1 divider and on
what is on the screen; this is a guess, and it is to be verified in a separate run.

The practical implication concerns everything we measure going forward: a single point does not
distinguish anything smaller than 10%, so any deviation of that magnitude
requires repeats rather than an explanation.

The better of the two runs falls onto the line exactly: bus 1.500x, bandwidth
509/339 = 1.501x. That is, in this mode memory is literally the only thing the
benchmark measures, and there is no other bottleneck nearby.

**CORRECTED right here.** In the first revision of this paragraph I wrote that the step
to 201.6 MHz lagged behind the line by 8% (569 MB/s instead of the expected 621) and
explained this by SDRAM latencies. That was premature: 8% lies within the just
demonstrated 10% spread, meaning deviation from the line is not proven
at all. The correct formulation is: bandwidth follows the bus linearly
within measurement accuracy, and to observe a deviation, repeats are needed.

3D follows memory almost one-to-one: +50% bus yielded +50% fps
(16.33 -> 24.55). The next step added another 15% fps (to 28.20), but there the core grew
along with the bus (multiplier 86 -> 105, MPU 330 -> 403 MHz), so separating contributions
at this point is impossible: two things were changed at once.

Overall summary for E90 — 16.33 -> 28.20 fps, i.e. +73%, with the bulk
of this gain coming from the bus rather than the core.

## 40. C5-00.2 as a measuring instrument; cache test does not measure cache

First measurements on C5-00.2 (ARM11 600 MHz with FPU, not OMAP 2420, overclocking does not
apply to it — §38 and above):

```
C5-00.2   memory 256 MB/s   ALU 38.2 Mitr/s   cache 90 MB/s
```

Pavlo pointed out that cache turns out slower than memory, and that this occurs
on all devices. This is physically impossible, hence a flaw in the instrument — and it
was found in the test itself.

The two tests compute MB/s incompatibly with each other:

```cpp
// TestMemBandwidth: one touch per cache line, 64 bytes counted
for (TInt i = 0; i < aWords; i += 16)
    acc += aBuf[i];

// TestCache: two LCG multiplies, a store, a load and a compare for EVERY 4 bytes
x = x * KLcgA + KLcgC;
aBuf[i] = x;
...
if (aBuf[i] != x) ++bad;
```

That is, for every 64 bytes the memory test performs one addition and one miss into
DRAM — it is indeed bus-bound. The cache test for the same 64 bytes performs
sixteen multiplications in a serial dependency chain plus sixteen
writes, reads, and comparisons. The working set of 4 KB in doing so resides in L1 and
costs almost nothing, so the limiter becomes the ALU rather than the cache.

The arithmetic confirms this quantitatively. In the cache test, exactly 4 credited bytes
correspond to one LCG iteration, so at the measured 38.2 Mitr/s the ceiling of this
loop is 152.8 MB/s. Measured is 90 MB/s, that is, 23.4 Mitr/s, or 61% of pure
ALU speed; the remainder goes to writes, reads, and comparisons that are absent in the ALU
test. Thus the figure "cache 90 MB/s" is entirely explained by ALU speed and tells
nothing about cache.

Conclusion for the method: only two tests that perform the same work per byte can be
compared with each other. For the cache test to truly measure cache, its loop must be
identical to that in the memory bandwidth test, differing ONLY in the working
set size — smaller than L1 instead of larger than it. Until this is done, line
`CACHE` in the application should be read as a second ALU test.

Side observation, as yet without explanation: C5 memory (256 MB/s) is slower than
stock E90 memory (339 MB/s), even though the core in C5 is twice as fast. What this is — narrower
bus, different memory type, or different latencies — has not been measured.

### 40.1. E90 at 503 MHz yields 37 Mitr/s — C5 level at 100 MHz lower frequency

Pavlo 2026-09-11: on E90, 37 Mitr/s was reached in ALU at core frequency 503 MHz,
memory at the same time being 534 MB/s.

The multiplier is derived from frequency unambiguously. On E90 `sel2` is always 2, MPU divider
is stock 2 (§35.1), so MPU frequency equals the multiplier multiplied by 3.84 MHz:
`503 / 3.84 = 131.0`. Thus multiplier 131, which is two steps above the old slider ceiling
of 129 — the limit raised to 255 (§38.7) was needed immediately in practice, not
merely on paper.

Memory at 534 MB/s is consistent with `l3div=6` at CORE 1006.08 MHz, i.e. bus
167.68 MHz: linear scaling from stock 339 MB/s at 110.08 MHz gives 516 MB/s, and
the 3% difference lies within the measured spread of repeated runs (§39.1).
This is consistency rather than an established configuration: the exact layout line from that
run was not recorded.

**Comparison with C5 is more interesting than the figure itself.** C5-00.2 yields 38.2 Mitr/s at
600 MHz (§40), E90 — 37 Mitr/s at 503 MHz. Normalized per clock, this is 0.064 and
0.074 Mitr/s per MHz respectively, meaning the E90 core on this loop is approximately 15%
more performant per clock. Explanation has not been established; the ALU test is a serial
chain of dependent multiplications, so it measures multiplier pipeline latency rather than
throughput, and different ARM11 revisions may well differ here.
An alternative explanation that cannot be ruled out without measurement is that C5 at the moment
of measurement was not running at the claimed 600 MHz; per the project rule, what
the system reports about frequency cannot be trusted.

Practical summary: the overclocked 2007 E90 in this test effectively matched
a noticeably newer device, trailing by only 3% at a 100 MHz lower frequency.

## 41. Test profiles for other OMAP 2420 devices and finding the global on hardware

Previously (§40 and below), profiles for N82 and RM-159 were not added because subagents
returned machine UIDs without a trace of proof, while the "disassembly" turned out to be offset
extrapolation. This remains correct regarding THOSE numbers. But the refusal itself was broader
than necessary: some addresses can be proven from images, and this was done below with our own
hands, not via a subagent report.

### 41.1. What was proven byte-by-byte

Comparison of six words at each address directly in images `out/roms/*.rom`:

```
                           RM-320 ref  RM-159      N82
relock routine 0x800a7f6c  e92d40fe...  IDENTICAL   IDENTICAL at +0x10
DSP setter     0x800ac59c  e3510009...  IDENTICAL   IDENTICAL at +0x10
mult reader    0x800b1544  e92d4010...  IDENTICAL   IDENTICAL at +0x10
dvfs body      0x80148410  e92d4010...  DIFFERENT   DIFFERENT
```

Thus offset `+0x10` for N82 applies to `power.dll` code, and within those limits it is
proven. To `iDvfsBody` it does NOT extend: that is a separate module, and the subagent's
assertion of `+0x10` for it was an error.

### 41.2. `iDvfsBody` found by signature, and method verified on known answer

The body of `DoControl(1)` was searched using six words, of which three are taken as is, while three
(bl, ldr, b) are masked because their offsets depend on placement. In each of the
three images, EXACTLY ONE match was found across the entire 43-megabyte file:

```
rm320  0x80148410   <- matches the known profile address
n159   0x8014b5f0
n82    0x801411e0
```

The fact that on the baseline image the method reproduces exactly the answer we know
independently serves as the basis to trust the two other numbers. Six words for rollback
furthermore turned out to be identical in all three images, so `KDvfsOriginal` is suitable
for all.

### 41.3. Power-down global could NOT be found offline, and this is important

Signature of the getter/setter pair (`str r0,[r1,#4]` / `ldr r0,[r0,#4]`) yields in all
three images a pool with value `0xc80030ec`. But the proven RM-320 global is
`0xc80030c8`, meaning the signature finds an ADJACENT pair. The method does not reproduce the known
answer, therefore its results cannot be used, and none of these numbers are in the profile.

Instead, the address is searched ON THE DEVICE. The engine traverses a narrow window of
`landmark ± 0x200` and accepts only the address whose chain
(`global+4 -> object -> object+16 -> SRAM copy`) yields a copy byte-for-byte identical to
the original in ROM. Verification was simultaneously strengthened from one word to eight: one
is insufficient specifically for SEARCHING, because `0xe92d40fe` occurs frequently in memory.

The window is small intentionally. Each probe is a read at an arbitrary address IN KERNEL
MODE, and an unmapped page would cost a reboot rather than an error.

**The mechanism was verified on two devices with known answers:**

```
RM-320  search -> found=c80030c8  sram=cb202000   MECHANISM OK
E90     search -> found=c8003068  sram=cb102000   MECHANISM OK
```

Both times the search reproduced the exact address known independently by the profile, moreover
on different primitive carriers (dvfs and Usbc).

### 41.4. What test profiles do NOT know and how this is covered

There is no machine UID, so they are never selected automatically: zero is set in the profile,
and `ProfileForThisDevice` skips anything marked with `iExperimental`. Selecting
such a profile is only possible manually, via a dedicated menu item, after two warnings.

There is no build version either: no version string was found in resident XIP (checked
by searching images — it is also absent for RM-320 whose version is known). Therefore the firmware
gate for these profiles is empty, and its role is taken over by verifying the SIGNATURE of
the relock routine: after applying the READ patch, the engine reads eight words at
`iRomSramRoutine` and compares against what is in the image. This proves two things
at once — that the patch is alive and that the address points specifically to the relock routine of this build.

Stock clocking layout is unknown, so for such profiles the reference point becomes
whatever was found at startup. Because of this, the device must be freshly rebooted, and
the application states this right before the attempt.

**What remains an unavoidable risk.** The first write to `iDvfsBody` occurs before
any verification — there is no way to read this address without patching it. If the
device runs a different build than the one from whose image the address was taken, this write will land
on the wrong code. That is precisely why the profile is test, selection is manual, and the warning is explicit.
Nothing is written to flash in doing so, so the worst is a reboot.

> **Correction (§42).** This paragraph is flawed in its premise. The assertion that "there is no way to read this
> address without patching it" contradicts §34, where XIP is read directly from
> user mode, which was measured right there. Thus verifying bytes BEFORE writing
> is possible, and that is now exactly how it is done. The risk is not "unavoidable", but eliminated.
## 42. Original DoControl body is captured from live ROM, not taken from a constant

While verifying test profiles, a defect was found that was unrelated to address
searching and was worse than everything being searched for. Patch rollback
(`RestoreKernelCode`) wrote back the constant `KDvfsOriginal` — six words
captured from RM-320. But three of these six words are `bl`, `ldr [pc,#imm]`, and
`b`, that is, offsets unique to a specific build. For an N82 or RM-159, such a
"rollback" would write foreign offsets into their body and leave the driver
corrupted until a reboot — silently, moreover, because the write itself succeeds.

Therefore, the original is now captured from the live ROM of this device before
the first write (`CaptureOriginal`) and stored in the engine; rollback uses
precisely these words. Such rollback is correct by construction for any build.

The same operation at the same time resolved the device identification
question. A test profile has neither a machine uid nor a firmware gate, so the
bytes at the patch address are the only thing distinguishing the target device
from any other Symbian phone. Verification uses the same mask as the search in
`tools/romscan.js`: words 0, 1, and 4 are exact, while for words 2, 3, and 5 only
the opcode is verified, since the offsets there must differ. If there is a
mismatch — for a test profile this is a rejection with the step `verify body`,
and nothing is written to the kernel.

For a profile with a firmware gate, a mismatch is interpreted differently: there
the constant is proven to correspond to this exact build, and inconsistent bytes
are almost certainly a trace of a previous run that did not have time to roll
back. In this case the constant is used, so behavior remains as it was, and the
device is recovered rather than rejected.

Verified on N95 8GB on 2026-09-11: live words at `0x80148410` turned out to be
`e92d4010 e3a00001 eb00014c e59f11d0 e8bd4010 ea000135`, that is, exactly the
same as in the constant; the mask matched, rollback was confirmed by stock
`DoControl(1,0) -> 100`. On E90, the Usbc bearer branch verifies all six words
exactly, because the constant for it was captured from live ROM (§34); on device,
this run has not yet been performed.

At the same time, smaller issues were resolved: the patch is not applied at all
until the original is captured (the check is located in `ApplyReadPatch` and
`ApplyExecPatch` themselves, not just in `Prepare`), and the global search checks
that the hint from the profile lies in kernel data and is aligned — otherwise
reading would proceed at a random address in kernel mode.

## 43. Overclock survives application exit; HAL candidates for autostart kill switch

### 43.1. Why overclock was resetting on exit

A user reported (2026-09-11) that after exiting the application, the overclock
disappears. This was confirmed in the code, and the cause turned out to be not
where expected. The engine has long had `KeepOnExit`, and the destructor honors
it:

```cpp
if (iApplied && !iKeep && iSram)
    { TUint32 st = 0; RestoreStock(st); }
```

However, the GUI never set this flag, and most importantly — exit proceeds not
through the destructor, but through `COcView::ShutdownL()`, where restoration
was unconditional. That is, the flag would have worked, but execution simply
never reached it.

Now both paths query `iKeep`, and the flag itself is exposed in the
`KEEP ON EXIT` row and enabled by default. The rationale for such a default is
simple: frequency survives the process on its own; this is a hardware property,
not an application service; a closed window is no reason to cancel what a person
applied deliberately. A reboot restores stock clocking in any case.

Removal of the kernel patch remained unconditional, and this is fundamental.
Frequency is a hardware state that a reboot fixes; a replaced `DoControl` body is
a landmine for any other driver client, and leaving it is inadmissible under any
settings.

A side effect turned out to be better than the fix itself: the single truly
dangerous operation vanished from the exit path. Restoring an underclocked bus
to stock killed the device four times out of four (§38.2), and until then every
application exit with an underclocked bus did exactly that.

### 43.2. What this device's HAL knows about power and case

For the hardware autostart kill switch (`docs/AUTOSTART.md` §7), a signal is
needed that can be read without capabilities and does not depend on USB mode or
the presence of a memory card. Three HAL candidates were checked, measurement on
N95 8GB:

```
EPowerExternal = 1
EPowerBatteryStatus -> err -5
ECaseState -> err -5
```

Case state and battery status are not supported by the device
(`KErrNotSupported`), so as a kill switch they are eliminated completely.
`EPowerExternal` is read normally.

**This is so far one point out of two.** A value of one does not prove that the
attribute tracks the cable: a constant would look exactly the same. The same row
is needed with power disconnected, and until then this is a guess, marked as a
guess.

## 44. E90 confirmed on device: live ROM, frequency persistence, fonts

Date: 2026-09-12. Device — Nokia E90 (RA-6, 400.34.93), transport Bluetooth
COM9. Everything below was captured from the device, not deduced from code.

### 44.1. Usbc branch in CaptureOriginal works, and a constant here would have been incorrect

`ocprobe patcher hold 96` on E90 read the original body of `DoControl(69)` from
live ROM:

```
orig body (6 words, live ROM): e92d4ff0 e24dd044 e1a04000 e1a06002 e1a07001 e3a01000
```

Compare with RM-320, where the first word is `e92d4010`, and the third is `bl`.
The bodies are entirely different, not just in offsets. This directly confirms
§42: a single restoration constant for two devices would have been not "almost
correct", but garbage written into the kernel.

### 44.2. Frequency survives process exit — measured

Measurement procedure: `patcher hold 96` (process applied and exited), then
twice `patcher engine` — as separate processes, with an interval of about five
minutes of device idle time.

```
after hold 96, +1 min : CM_CLKSEL1_PLL = 00060400, m=96   (NOT STOCK AT START)
same,          +5 min : CM_CLKSEL1_PLL = 00060400, m=96
HAL ECPUSpeed        : 368000 (stock 330000)
```

Thus the system itself does not restore the multiplier: neither process death
nor several minutes of idle time touch it. The complaint "after exit overclock
resets" is therefore not explained by OS behavior, and the reason must be sought
in the application version on the device.

**Root cause found.** On E90, `C:\sys\bin\ocgui.exe` held a build of
80 936 bytes versus 101 544 in the current one: the device had a version older
than the `KEEP ON EXIT` row (§43.1), and it removed the overclock in
`ShutdownL()` unconditionally. Fresh binary uploaded on 2026-09-12.

### 44.3. Ukrainian letters exist in device fonts; apostrophe U+02BC is missing

The command `ocprobe font` rasterizes a character into a monochrome 24×24 bitmap
via `CFbsBitGc` and verifies the pixel checksum against the known missing
`U+4E00`. A checksum match means that the same "square" was drawn, that is, the
glyph is absent. On E90:

```
Nokia Hindi S60:  U+0041 px=19  U+0430 px=14  U+0456 px=9  U+0457 px=11
                  U+0454 px=11  U+0491 px=10  U+02BC MISSING  U+4E00 MISSING
Series 60 ZDigi:  U+0041 px=22  rest MISSING             (digit font)
```

That is, Ukrainian letters U+0456, U+0457, U+0454, U+0491 exist and are drawn
in the text typefaces of E90. But the modifier apostrophe `U+02BC`, with which
"pam'yat" was written in the source, has no glyph in any typeface and came out as
a square. In `src/strings.json` it was replaced with ASCII `U+0027` (eight
occurrences).

**What this measurement does not prove.** It was made on E90, and the list of
typefaces there is unusual (three items, two "Hindi"), because a third-party
`FontZoomerRasterizer` is installed on the device. For N95 8GB the same
measurement has not yet been made, and if the complaint specifically concerns
N95, the question regarding `U+0456` remains open until running `ocprobe font`
on it.

## 45. N95: same complaint, same cause - old build was running

Date: 2026-09-12, Nokia N95 8GB, WiFi.

The device maintains two directories with our executable. From the menu the
phone launches `C:\sys\bin\ocgui.exe`, and there lay a build of
**80 936 bytes** — the same pre-relict one as on E90. Yesterday's 2.0.1
(**101 544 bytes**) went to `E:\sys\bin\ocgui.exe`, from where the device does
not launch it. That is, the complaint "overclock resets after exit" concerned
code older than the `KEEP ON EXIT` row.

The fresh binary was uploaded to `C:\sys\bin`, and the copy from `E:` was
deleted: two copies with the same UID are a ready-made trap, and that is
precisely what was triggered here. For future deployments the rule is one:
`ocgui.exe` goes only to `C:\sys\bin`.

`ocprobe font` on N95 gave exactly the same as on E90:

```
Nokia Hindi S60: U+0456 px=9  U+0457 px=11  U+0454 px=11  U+0491 px=10
                 U+02BC MISSING  U+4E00 MISSING
```

Thus Ukrainian letters in N95 fonts are present, and modifier apostrophe
`U+02BC` is absent there as well. Replacement with ASCII (§44.3) closes the
issue on both devices.

**Not resolved:** `patcher hold 96` on N95 executed (328 -> 366 MHz, 518 ->
580 MB/s), but immediately after this the daemon stopped responding, and a
repeated reading of the multiplier could not be performed — meaning an analogue
of measurement §44.2 for N95 is not yet available. The device remained at
multiplier 96; reboot returns 330 MHz.

## 46. Instruments measured themselves: cache line, memory bandwidth, operation cost

Date: 2026-09-12, Nokia N95 8GB. Occasion - owner's remark that memory bandwidth
(500-600 MB/s) looks overstated, and the cache test is unclear.

### 46.1. Cache line is 32 bytes, and memory test overstated twofold because of this

The bandwidth test in the application traversed the buffer with a 64-byte
stride and counted read bytes as "reads × 64", that is, assuming that one read
fetches a 64-byte line. The assumption was never verified.

Verification by direct measurement (`ocprobe membench`): if the stride is
smaller than the line, multiple reads share one fetched line, and the time per
read grows with the stride until the stride equals the line length; beyond that,
each read costs a separate miss, and the number reaches a plateau. Warm run:

```
stride    4 B:  43.625 ns/read      stride   32 B:  93.597 ns/read
stride    8 B:  63.619 ns/read      stride   64 B:  96.389 ns/read
stride   16 B:  60.882 ns/read      stride  128 B: 101.501 ns/read
                                    stride  256 B: 111.755 ns/read
```

The plateau begins at a 32-byte stride. Therefore the cache line is 32 bytes,
as is proper for ARM1136, not 64. The test counted twice as many bytes as it
actually read, so all memory bandwidth numbers cited up to that point
(518-580 MB/s) were **overstated exactly twofold**.

Fixed differently than by simply replacing a constant: now EVERY word is read,
and the buffer size goes into the byte count, so the assumption about cache line
size disappeared from the calculation entirely.

### 46.2. True memory numbers

Contiguous read and write of 4 MB, loop unrolled by eight (in rolled form, the
measurement hit its own loop counter and branch rather than memory):

```
seq read  4 MB: 199 MB/s
seq write 4 MB: 403 MB/s
cache read 4 KB (within L1): 541 MB/s
```

The bus in this run was at 165 MHz (`l3div=4`), meaning the theoretical ceiling
of 32-bit SDRAM is about 660 MB/s. The measured 199 MB/s for read is 30% of the
ceiling, which is expected for a single-pipeline core without prefetching: the
next miss does not begin until the previous one has completed.

The first run after allocating the buffer is systematically twice as slow as
subsequent ones (39855 versus 20996 µs). The reason is the DVFS governor: it
raises the OPP only after seeing workload. Therefore, a measurement without
warmup is meaningless.

### 46.3. Cache test was measuring arithmetic, not cache

Until then, under the name "cache", an LCG generator was spinning with writes
and verification of every word. One word accounted for multiplication, addition,
and comparison, so the number reflected the ALU and was fundamentally not
comparable with the memory test. Now exactly the same loop runs there as in the
memory test, with the sole difference that the 4 KB working set fits in L1
(16 KB). The two numbers became comparable, and their ratio (541 versus
199 MB/s, that is, 2.7 times) has physical meaning.

### 46.4. Operation cost in cycles

`ocprobe alubench`: each kernel is written in assembly with `.rept`, so the
instruction count per iteration is known precisely, and frequency is taken from
an independent NOP measurement. Run at 421 MHz:

```
add dep    : 1.04 cycles/op      dependent chain of adds
add indep  : 1.03 cycles/op      independent adds
mul dep    : 4.94 cycles/op      dependent chain of multiplies
eor+lsr dep: 1.99 cycles/op      exclusive OR with a shift
lcg mul+add: 2.98 cycles/op      mul+add pair, i.e. 5.96 cycles per pair
```

Three conclusions. The core is single-issue: dependent and independent addition
chains yield the same one cycle, meaning there is no instruction-level
parallelism here. Multiplication costs about 5 cycles of latency, and a
`mul+add` pair consists of exactly those five plus one. A shifted operand adds a
cycle.

This also explains the old ALU test number. An LCG iteration costs about
6 cycles, which means at 328 MHz the ceiling would be about 55 Mitr/s, whereas
the test showed 17.34. The difference is consumed by the loop counter and
constant reloads generated by the compiler: the number was measuring a C loop,
not arithmetic. Therefore, two direct metrics were added to the application —
absolute frequency via a NOP chain and cycles per operation via an assembly
LCG kernel.

## 47. Firmware image does not provide addresses for E90, N93, and N93i; live XIP scanner does

### 47.1. Offset in file does not equal address, and this is measured

For RM-320, RM-159, and N82, the extracted image is a full linear XIP, and the
address equals `0x80000000 + offset`. For E90, N93, and N93i - no: the image is
a fragment (1.8, 1.1, and 1.3 MB versus almost 20 MB of live XIP on the
device).

The difference for E90 is measured directly. The prologue of
`DLddUsbcChannel::DoControl` lies in the image at offset `0x94ac4`, so a linear
mapping would yield `0x80094ac4`. On the device the same function is located at
`0x800bf874`. The discrepancy is `0x2adb0`.

Therefore, for such devices offline constants are invalid **in principle**, not
merely "pending verification". Not a single number for N93 and N93i can be taken
from their images.

### 47.2. Live XIP scanner reproduces both known answers

Instead of constants, the profile can search for addresses on its own by
scanning live XIP from user mode (safety of this path - §34). Two signatures are
sought: prologue of `DLddUsbcChannel::DoControl` (four words) and the relock
routine of `power.dll` (eight words, of which two are `ldr` with PC-relative
offset, hence matched by mask). There must be exactly one match: two matches mean
the signature does not discriminate the required function.

The method was verified on two devices where the correct answers are known
independently (`ocprobe romfind`):

```
E90 : body 0x800bf874  relock 0x80058da4   - both match the profile
N95 : body 0x8010bd04  relock 0x800a7f6c   - relock matches the profile,
                                             body matches the offset within the image
```

On N95 the match is twofold and illustrative: the RM-320 image is linear, so the
body address found on the device had to equal the offset `0x10bd04` in the image
— and it does.

### 47.3. N93 and N93i profiles

Added `RM-55 N93 (TEST, auto)` and `RM-156 N93i (TEST, auto)` with empty
addresses and a search flag. There is no version of the `dvfs` driver in the
firmwares of these devices (scan across six images), so the primitive bearer is
set to `Usbc`, as on E90.

**Not tested on hardware even once.** Not matched automatically: machine uid is
zero. If the scanner fails to find a signature or finds it twice, the engine
halts at the `EOcStepDiscover` step and writes nothing to the kernel.

What to expect honestly. The `DoControl` prologue did not occur in the N93
image fragment at all, meaning the USB driver there is either of a different
build or resides in the compressed portion that is absent from the fragment. The
relock routine was found in the N93i fragment, but not in the N93 fragment. Thus
the probability of finding both signatures on a live device is unknown, and the
profile exists precisely so that a device owner can determine this and report the
result.

## 48. Pre-publication code review: what was confirmed

Date: 2026-09-13. Review performed by subagent `ag-reviewer` in read-only mode;
each finding was manually verified against the code, as a subagent report is not
evidence. Below is only what was confirmed.

### 48.1. DSP watchdog timer deleted itself from within its own callback

```
DspRevertTick  ->  RestoreDspNow  ->  CancelDspRevert  ->  delete iDspTimer
```

`DspRevertTick` is a callback of `CPeriodic` itself. Deleting the object inside
its own callback leaves `CActive::RunL` operating on freed memory immediately
upon return. It triggers on the single path intended as standard: the user set a
non-stock DSP divider and simply did nothing for 120 seconds.

Fixed: `CancelDspRevert` only cancels the timer, and it is destroyed once, in the
destructor. A cancelled `CPeriodic` can be started again, so nothing is lost.

### 48.2. Combined test was never run

`RunTest` lacked a branch for `ERowTestCombined`, and the row fell through to
`default: break`. The function `RunCombined` was written but not called from
anywhere — `grep` across the project found only its declaration and definition.

### 48.3. Digit on MULT row on an unrecognized device yielded multiplier 0

`StockMult()` returns zero prior to engine readiness, and zero went straight
into `iMult`, below `KOcMultMin`.

### 48.4. Repeated `Prepare` opened patcher on top of an open channel

The path is entirely ordinary: on an unrecognized device the user browses test
profiles and presses OK on each. The first failed profile left
`iPatcherOpen == ETrue`, and the subsequent call invoked `iPatcher.Open()` once
more, losing the previous handle.

### 48.5. Probe computed memory bandwidth using a disproven formula

`SpinMem` in `ocprobe.cpp` remained with a 64-byte stride and a "reads × 64"
calculation after §46.1 showed a 32-byte line, while the application had already
been fixed. Thus, two tools of the same project were reporting numbers that
differed twofold.

Fixed and verified on device: `bench` yielded 196-199 MB/s versus 199 MB/s in
`membench`, meaning both paths now converge.

### 48.6. Minor, but also confirmed

The view was not on the cleanup stack while `AddToStackL` could leave. A ROM
size of exactly `0x10000`, reported in kilobytes, was read as bytes, and
signature scanning would have been limited to the first 64 KB. A failed check of
the reset vector on the Usbc bearer was reported as a failure to open the
driver.

### 48.7. What was determined NOT to be a defect

The ramp passes target dividers at each step rather than selecting them step by
step. The reviewer identified this as a defect; this is deliberate behavior
described in a comment at `ApplyRampWithCore`. After the bus safeguard was
removed, it aligns with the rest: the user specifies values, the device receives
them.

Writing zero to `CM_CLKSEL1_CORE` when `aCoreReg == 0` does not occur on live
paths — `ApplyMult` always supplies `CompensatedCore`, and `ApplyRampWithCore`
when zero invokes `ApplyMult` specifically.
