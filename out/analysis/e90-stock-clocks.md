# Independent Analysis of Nokia E90 XIP: Address Verification and Stock Clocking Scheme

- **ROM file:** `out/roms/e90.rom` (size 1,769,472 bytes = 0x1B0000, uncompressed XIP `SOS+UPDAPP`)
- **Load base:** `0x80000000` (file offset `offset = vaddr - 0x80000000`)
- **Disassembler:** `arm-none-symbianelf-objdump.exe -D -b binary -m arm --adjust-vma=0x80000000`

---

## 1. Byte Verification of Prologues and Function Bodies

### 1.1. `iRomSramRoutine` = `0x80056874` (SRAM idle/relock routine of power.dll)

- **Expected prologue:** `push {r1-r7,lr}` (`e92d40fe`) or `{r1-r6,lr}` (`e92d407e`).
- **File offset:** `0x00056874`.

Disassembly of window `0x80056870`–`0x800568dc` (~0x6C bytes):

```arm
80056870:	e12fff11 	bx	r1
80056874:	e92d40fe 	stmdb	sp!, {r1, r2, r3, r4, r5, r6, r7, lr}
80056878:	e59f1128 	ldr	r1, [pc, #296]	; 0x800569a8 -> 0xc6129000 (SDRC)
8005687c:	e59f5128 	ldr	r5, [pc, #296]	; 0x800569ac -> 0x0000770e
80056880:	e3a03000 	mov	r3, #0
80056884:	ee073f9a 	mcr	15, 0, r3, cr7, cr10, {4} ; write-buffer drain barrier
80056888:	e5914070 	ldr	r4, [r1, #112]
8005688c:	e3844048 	orr	r4, r4, #72	; 0x48
80056890:	e5814070 	str	r4, [r1, #112]
80056894:	e3a05000 	mov	r5, #0
80056898:	e59f1110 	ldr	r1, [pc, #272]	; 0x800569b0 -> 0xc6014000
8005689c:	e5912204 	ldr	r2, [r1, #516]
800568a0:	e202220f 	and	r2, r2, #-268435456	; 0xf0000000
800568a4:	e1b02e22 	movs	r2, r2, lsr #28
800568a8:	e3520005 	cmp	r2, #5
800568ac:	1a00000f 	bne	0x800568f0
800568b0:	e59f10fc 	ldr	r1, [pc, #252]	; 0x800569b4 -> 0xc6008000
800568b4:	e5912540 	ldr	r2, [r1, #1344]	; CM_CLKSEL1_PLL (+0x540)
800568b8:	e1b02422 	movs	r2, r2, lsr #8
800568bc:	e202200f 	and	r2, r2, #15	; 0xf
800568c0:	e3520008 	cmp	r2, #8
800568c4:	da000009 	ble	0x800568f0
800568c8:	e3a05001 	mov	r5, #1
800568cc:	e59f10e0 	ldr	r1, [pc, #224]	; 0x800569b4 -> 0xc6008000
800568d0:	e5912500 	ldr	r2, [r1, #1280]	; CM_CLKEN_PLL (+0x500)
800568d4:	e20220fc 	and	r2, r2, #252	; 0xfc
800568d8:	e3822001 	orr	r2, r2, #1
800568dc:	e5812500 	str	r2, [r1, #1280]
```

- **First instructions:**
  1. `80056874: e92d40fe  stmdb sp!, {r1, r2, r3, r4, r5, r6, r7, lr}`
  2. `80056878: e59f1128  ldr r1, [pc, #296]`
  3. `8005687c: e59f5128  ldr r5, [pc, #296]`
  4. `80056880: e3a03000  mov r3, #0`
  5. `80056884: ee073f9a  mcr 15, 0, r3, cr7, cr10, {4}`
- **Verdict:** **CONFIRMED.** Prologue exactly matches `push {r1-r7,lr}` (`e92d40fe`). Routine logic is identical to the RM-320 reference `0x800a7f6c`.

---

### 1.2. `iDspSetter` = `0x80059aa4` (DSP Divider Setter)

- **Expected marker:** `cmp r1, #9` (`e3510009`), nearby mask `0x1f` (`#31`).
- **File offset:** `0x00059aa4`.

Disassembly of window `0x80059aa0`–`0x80059b04` (~0x64 bytes):

```arm
80059aa0:	e8bd8010 	ldmia	sp!, {r4, pc}
80059aa4:	e3510009 	cmp	r1, #9
80059aa8:	e92d4010 	stmdb	sp!, {r4, lr}
80059aac:	0a000007 	beq	0x80059ad0
80059ab0:	ca000003 	bgt	0x80059ac4
80059ab4:	e3510000 	cmp	r1, #0
80059ab8:	13510005 	cmpne	r1, #5
80059abc:	1a000005 	bne	0x80059ad8
80059ac0:	ea000002 	b	0x80059ad0
80059ac4:	e3510010 	cmp	r1, #16	; 0x10
80059ac8:	13510012 	cmpne	r1, #18	; 0x12
80059acc:	1a000001 	bne	0x80059ad8
80059ad0:	e3e00005 	mvn	r0, #5	; returns -6 (KErrArgument)
80059ad4:	e8bd8010 	ldmia	sp!, {r4, pc}
80059ad8:	e1a02001 	mov	r2, r1
80059adc:	e59f032c 	ldr	r0, [pc, #812]	; 0x80059e10 = 0xc6008840 (CM_CLKSEL_DSP)
80059ae0:	e3a0101f 	mov	r1, #31	; 0x1f - DSP divider mask
80059ae4:	eb00157e 	bl	0x8005f0e4	; RMW register write
80059ae8:	e3a00000 	mov	r0, #0	; KErrNone
80059aec:	e8bd8010 	ldmia	sp!, {r4, pc}
80059af0:	e3510001 	cmp	r1, #1
```

- **First instructions:**
  1. `80059aa4: e3510009  cmp r1, #9`
  2. `80059aa8: e92d4010  stmdb sp!, {r4, lr}`
  3. `80059aac: 0a000007  beq 0x80059ad0`
  4. `80059ab0: ca000003  bgt 0x80059ac4`
- **Action:** validates the divider, rejecting 0, 5, 9, 16, 18, places `r1` into `r2`, loads the `CM_CLKSEL_DSP` register address (`0xc6008840` from pool `0x80059e10`), sets the clear mask `0x1f` (`mov r1, #31`), and calls the RMW function `0x8005f0e4`.
- **Verdict:** **CONFIRMED.** The `e3510009` marker, validation, `0x1f` mask, and target register fully match the expected DSP divider setter (analogue of RM-320 `0x800ac59c`).

---

### 1.3. Multiplier Pool `CM_CLKSEL1_PLL`: Addresses `0x800b76dc`, `0x800580e0`, `0x8005eba8`

Checked 4 bytes little-endian for each address:

| Virtual address | File offset (`vaddr - 0x80000000`) | Bytes in file | Read 32-bit word | Expected | Verdict |
|---|---|---|---|---|---|
| `0x800b76dc` | `0x000b76dc` | `40 85 00 c6` | `0xc6008540` | `0xc6008540` | **MATCHES** |
| `0x800580e0` | `0x000580e0` | `40 85 00 c6` | `0xc6008540` | `0xc6008540` | **MATCHES** |
| `0x8005eba8` | `0x0005eba8` | `40 85 00 c6` | `0xc6008540` | `0xc6008540` | **MATCHES** |

#### Code references to these three pools:

1. **`0x8005eba8` is accessed by `0x8005ea1c` in function `0x8005ea00`:**
   This is the **stock DPLL multiplier reader function** (previously considered missing from XIP / "in paged part" in `_summary.md`):
   ```arm
   8005ea00:	e92d4010 	stmdb	sp!, {r4, lr}
   8005ea04:	e59f0198 	ldr	r0, [pc, #408]	; 0x8005eba4 -> 0xc6008500 (CM_CLKEN_PLL)
   8005ea08:	eb0001ad 	bl	0x8005f0c4	; register read veneer
   8005ea0c:	e3a01003 	mov	r1, #3
   8005ea10:	e1d10000 	bics	r0, r1, r0
   8005ea14:	13a00000 	movne	r0, #0
   8005ea18:	18bd8010 	ldmneia	sp!, {r4, pc}
   8005ea1c:	e59f0184 	ldr	r0, [pc, #388]	; 0x8005eba8 -> 0xc6008540 (CM_CLKSEL1_PLL)
   8005ea20:	eb0001a7 	bl	0x8005f0c4	; register read veneer
   8005ea24:	e1a01000 	mov	r1, r0
   8005ea28:	e1a00300 	mov	r0, r0, lsl #6
   8005ea2c:	e1b00ea0 	movs	r0, r0, lsr #29	; extract bits [25:23] (sys_clk)
   8005ea30:	03a000c0 	moveq	r0, #192	; 19.2 MHz
   8005ea34:	0a000002 	beq	0x8005ea44
   8005ea38:	e3500002 	cmp	r0, #2
   8005ea3c:	03a00082 	moveq	r0, #130	; 13.0 MHz
   8005ea40:	13a00078 	movne	r0, #120	; 12.0 MHz
   8005ea44:	e1a02501 	mov	r2, r1, lsl #10
   8005ea48:	e1a01a01 	mov	r1, r1, lsl #20
   8005ea4c:	e1a02b22 	mov	r2, r2, lsr #22	; r2 = DPLL_MULT (bits [21:12])
   8005ea50:	e0000092 	mul	r0, r2, r0	; freq = sys_clk * mult
   8005ea54:	e3a02001 	mov	r2, #1
   8005ea58:	e0821e21 	add	r1, r2, r1, lsr #28 ; r1 = 1 + DPLL_DIV (bits [11:8])
   8005ea5c:	eb000188 	bl	0x8005f084	; freq / (div + 1)
   ```
   *Finding:* The DPLL multiplier reader function on E90 is **found in uncompressed XIP** at address `0x8005ea00`. It is a byte-for-byte twin of RM-320 `0x800b1544` (reader veneer is `0x8005f0c4`).

2. **`0x800b76dc` is accessed by `0x800b7220` in function `0x800b7204`:**
   ```arm
   800b7204:	e59f0498 	ldr	r0, [pc, #1176]	; 0x800b76a4 -> 0xc6008240 (CM_CLKSEL1_CORE)
   800b7208:	e92d4070 	stmdb	sp!, {r4, r5, r6, lr}
   800b720c:	e5900000 	ldr	r0, [r0]	; reads CM_CLKSEL1_CORE
   ...
   800b7220:	e59f24b4 	ldr	r2, [pc, #1204]	; 0x800b76dc -> 0xc6008540 (CM_CLKSEL1_PLL)
   800b722c:	e5920000 	ldr	r0, [r2]	; reads CM_CLKSEL1_PLL
   800b7234:	e1a03b20 	mov	r3, r0, lsr #22	; extract mult [21:12]
   800b7244:	e3a01001 	mov	r1, #1
   800b7248:	e0811e20 	add	r1, r1, r0, lsr #28 ; 1 + div
   800b724c:	e3a00c4b 	mov	r0, #19200	; sys_clk 19.2 MHz (kHz)
   ```

3. **`0x800580e0` is accessed by `0x80057ed4` in power.dll procedure `0x80057ec0`:**
   Reads `CM_CLKSEL1_PLL` via `bl 0x8005f0c4` and checks configuration bits.

---

### 1.4. Fourth Derived Address: `0x80094ac4` (`DLddUsbcChannel::DoControl`)

- **Purpose:** Host for read-via-write bootstrap in the E90 kernel.
- **File offset:** `0x00094ac4`.

Disassembly of window `0x80094ac0`–`0x80094b00`:

```arm
80094ac0:	eafffff4 	b	0x80094a98
80094ac4:	e92d4ff0 	stmdb	sp!, {r4, r5, r6, r7, r8, r9, sl, fp, lr}
80094ac8:	e24dd044 	sub	sp, sp, #68	; 0x44
80094acc:	e1a04000 	mov	r4, r0
80094ad0:	e1a06002 	mov	r6, r2		; argument a1 saved in r6
80094ad4:	e1a07001 	mov	r7, r1
```

- **First instructions:**
  1. `80094ac4: e92d4ff0  stmdb sp!, {r4, r5, r6, r7, r8, r9, sl, fp, lr}`
  2. `80094ac8: e24dd044  sub sp, sp, #68`
  3. `80094acc: e1a04000  mov r4, r0`
  4. `80094ad0: e1a06002  mov r6, r2`
- **Verdict:** **CONFIRMED.** Exactly matches the expected host prologue.

*(Note: for the address `iPwrDownGlobal = 0xc8004194`: setter `0x8005ebec` `str r0, [r1, #4]`, getter `0x8005ebf8` `ldr r0, [r0, #4]`, pool `0x8005ec00` were also verified and confirmed).*

---

## 2. Searching for the Stock E90 Clocking Scheme

### 2.1. Analysis of Virtual Address Pools `0xc6008240` (CORE) and `0xc6008140` (MPU)

In uncompressed XIP, search for literals `40 82 00 c6` (`0xc6008240`) and `40 81 00 c6` (`0xc6008140`) was performed:

1. **`0xc6008240` (`CM_CLKSEL1_CORE`):** exactly 4 occurrences found:
   - `0x80059e0c`: used by 7 setters `0x80059960`–`0x80059a94` in `power.dll` (configuring bitfields `L3`, `L4`, `DSS1`, `DSS2`, `VLYNQ`, `SSI`, `USB`). Each setter takes an argument in `r1` from the caller and performs RMW via `0x8005f0e4`. No full constant is written there.
   - `0x800b76a4`: used by DSS2 bit clear (`0x800b6cf8`) and frequency reader (`0x800b7204`).
   - `0x800bdd24`: used by DSS2 bit clear (`0x800bdb24`) and reading field [6:5] (`0x800bdc48`).
   - `0x800c307c`: used by reading (`0x800c281c`) and DSS2 bit clear (`0x800c2f54`).
2. **`0xc6008140` (`CM_CLKSEL_MPU`):** exactly 2 occurrences found:
   - `0x80059e08`: setter `iMpuSetter` (`0x80059914`), validates argument `r1` and writes it to register via `0x8005f0e4`.
   - `0x8005ebac`: function `GetMpuClockFrequency` (`0x8005eab4`), exclusively reads register to calculate frequency.

**Conclusion for virtual space (`power.dll`):** Writing a complete stock scheme to virtual registers `0xc6008240` / `0xc6008140` **is absent in resident XIP**.

---

### 2.2. Finding: Hardware Clocking Init Sequence in Bootstrap (Physical Addresses)

In early SoC initialization code (bootstrap, execution without MMU from PRCM physical base `0x48008000`, pool at address `0x80001e30`), **direct programming of all clocking registers was found**:

Disassembly of window `0x80001910`–`0x800019a8`:

```arm
80001910:	e59f0518 	ldr	r0, [pc, #1304]	; 0x80001e30 = 0x48008000 (PRCM physical base)
80001914:	e3a01001 	mov	r1, #1
80001918:	e5801500 	str	r1, [r0, #1280]	; CM_CLKEN_PLL (0x500) = 1 (bypass)
8000191c:	e3a01002 	mov	r1, #2
80001920:	e5801544 	str	r1, [r0, #1348]	; CM_CLKSEL2_PLL (0x544) = 2 (CORE_CLK_SRC = 2 -> CORE = DPLL * 2)
80001924:	e59f1524 	ldr	r1, [pc, #1316]	; 0x80001e50 = 0x08301044
80001928:	e5801240 	str	r1, [r0, #576]	; CM_CLKSEL1_CORE (0x240) = 0x08301044
8000192c:	e3a01002 	mov	r1, #2
80001930:	e5801140 	str	r1, [r0, #320]	; CM_CLKSEL_MPU (0x140) = 2
80001934:	e3a01002 	mov	r1, #2
80001938:	e5801340 	str	r1, [r0, #832]	; CM_CLKSEL_GFX (0x340) = 2
8000193c:	e59f1510 	ldr	r1, [pc, #1296]	; 0x80001e54 = 0x000023c3
80001940:	e5801840 	str	r1, [r0, #2112]	; CM_CLKSEL_DSP (0x840) = 0x000023c3
```

Bootstrap literal pool (`0x80001e50`–`0x80001e54`):
- `0x80001e50`: `44 10 30 08` -> `0x08301044` (`CM_CLKSEL1_CORE`)
- `0x80001e54`: `c3 23 00 00` -> `0x000023c3` (`CM_CLKSEL_DSP`)

#### Breakdown of register values written by Bootstrap:

1. **`CM_CLKSEL1_CORE` = `0x08301044`** (instruction `0x80001928: str r1, [r0, #576]`):
   - `L3` [4:0] = `0x04` = **4** (memory bus: `CORE / 4`)
   - `L4` [6:5] = `0x02` = **2** (peripheral bus: `L3 / 2`)
   - `DSS1` [12:8] = `0x10` = **16** (display: `CORE / 16`) *(on RM-320 it was 6; E90 has different displays 800x352 / 240x320)*
   - `DSS2` [13] = `0`
   - `VLYNQ` [19:15] = `0`
   - `SSI` [24:20] = `0x03` = **3** (modem link: `CORE / 3`)
   - `USB` [27:25] = `0x04` = **4**

2. **`CM_CLKSEL_MPU` = `0x00000002`** (instruction `0x80001930: str r1, [r0, #320]`):
   - `MPU_DIV` [4:0] = **2** (`CORE / 2`)

3. **`CM_CLKSEL2_PLL` = `0x00000002`** (instruction `0x80001920: str r1, [r0, #1348]`):
   - `CORE_CLK_SRC` [1:0] = **2** (`CORE = DPLL * 2`)

4. **DPLL multiplier and divider (`CM_CLKSEL1_PLL`)**:
   At addresses `0x80001840`–`0x800018ac`, bootstrap measures reference oscillator (crystal) clock frequency using hardware 32-kHz timer (`0x48028000`):
   - if counter > 500: reference frequency **19.2 MHz** (`sysSel = 0`);
   - if 390 < counter <= 500: reference frequency **13.0 MHz** (`sysSel = 2`);
   - if counter <= 390: reference frequency **12.0 MHz** (`sysSel = 3`).

   Next, at address `0x80001944`–`0x800019a4`, DPLL multiplier is computed and written:
   ```arm
   80001944:	e5907540 	ldr	r7, [r0, #1344]	; CM_CLKSEL1_PLL
   80001948:	e207250e 	and	r2, r7, #58720256 ; 0x3800000 (bits [25:23])
   8000194c:	e1a02ba2 	mov	r2, r2, lsr #23
   80001950:	e3a05003 	mov	r5, #3
   80001954:	e1550002 	cmp	r5, r2
   80001958:	0a000007 	beq	0x8000197c	; 12 MHz branch
   8000195c:	e3a05002 	mov	r5, #2
   80001960:	e1550002 	cmp	r5, r2
   80001964:	0a000009 	beq	0x80001990	; 13 MHz branch
   ; 19.2 MHz branch (standard for Nokia BB5 OMAP2420):
   80001968:	e3a01061 	mov	r1, #97	; 0x61 = 97 -> mult
   8000196c:	e3a02006 	mov	r2, #6	; 0x06 = 6  -> div (div + 1 = 7)
   80001970:	e1a01601 	mov	r1, r1, lsl #12
   80001974:	e0811402 	add	r1, r1, r2, lsl #8
   80001978:	ea000008 	b	0x800019a0
   ; 12 MHz branch:
   8000197c:	e3a01085 	mov	r1, #133 ; mult = 133, div = 5 (div+1 = 6) -> DPLL = 266.0 MHz
   ...
   ; 13 MHz branch:
   80001990:	e3a01029 	mov	r1, #41  ; mult = 41,  div = 1 (div+1 = 2) -> DPLL = 266.5 MHz
   ...
   800019a0:	e1877001 	orr	r7, r7, r1
   800019a4:	e5807540 	str	r7, [r0, #1344]	; CM_CLKSEL1_PLL (+0x540)
   ```

   **For stock 19.2 MHz crystal:**
   - `DPLL_MULT` field [21:12] = **97** (`0x61`)
   - `DPLL_DIV` field [11:8] = **6** (`0x06`), divider `div + 1` = **7**
   - Calculated DPLL frequency at boot: `19.2 MHz * 97 / 7 =` **266.057 MHz** (base 266 MHz frequency).
   - CORE frequency: `266.057 * 2 =` **532.114 MHz**.
   - MPU frequency: `532.114 / 2 =` **266.057 MHz**.
   - L3 bus frequency: `532.114 / 4 =` **133.028 MHz**.
   - DSS1 frequency: `532.114 / 16 =` **33.257 MHz**.

---

### 2.3. Is Transition to 330 MHz (mult = 86) Programmed in Resident XIP?

- **In uncompressed XIP `out/roms/e90.rom`, no secondary transition to 330 MHz was found.**
  Exhaustive search across the entire binary file showed:
  * Value `0x00056400` (`mult=86, div=4`, which yields 330.24 MHz) is present only as a bitmask constant at `0x800b76b4`.
  * Any other writes to `CM_CLKSEL1_PLL` (`offset 0x540`), besides `0x800018b0` and `0x800019a4` in bootstrap, **are absent in the entire XIP image**.
  * Virtual setters in `power.dll` contain no hardcoded constants and only process external calls.
- **Conclusion:** If Nokia E90 firmware subsequently switches the processor to 330 MHz (or another target ramp frequency), this code is located in the **compressed/paged part of ROM (ROFS)**, which is not present in the raw XIP image.

---

## 3. Summary Table

| Address / Register | Expected | Found in XIP | Evidence (instruction / dump) | Verdict |
|---|---|---|---|---|
| `0x80056874` | `iRomSramRoutine`: prologue `push {r1-r7,lr}` (`e92d40fe`) | `stmdb sp!, {r1-r7, lr}` (`e92d40fe`) | `80056874: e92d40fe` | **CONFIRMED** |
| `0x80059aa4` | `iDspSetter`: marker `cmp r1, #9` (`e3510009`), mask `0x1f` | `cmp r1, #9`, validation, `mov r1, #31`, RMW to `0xc6008840` | `80059aa4: e3510009`, `80059ae0: e3a0101f` | **CONFIRMED** |
| `0x800b76dc` | Pool `CM_CLKSEL1_PLL` = `0xc6008540` | `0xc6008540` (read in `0x800b7220`) | `800b76dc: 40 85 00 c6` | **CONFIRMED** |
| `0x800580e0` | Pool `CM_CLKSEL1_PLL` = `0xc6008540` | `0xc6008540` (read in `0x80057ed4`) | `800580e0: 40 85 00 c6` | **CONFIRMED** |
| `0x8005eba8` | Pool `CM_CLKSEL1_PLL` = `0xc6008540` | `0xc6008540` (read in `0x8005ea1c`) | `8005eba8: 40 85 00 c6` | **CONFIRMED** |
| `0x8005ea00` | DPLL multiplier reader function (previously considered missing) | Found: twin of RM-320 `0x800b1544` | `8005ea00: e92d4010`, `8005ea1c: ldr r0, [pc, #388]` | **FOUND IN XIP** |
| `0x80094ac4` | Bootstrap host `DLddUsbcChannel::DoControl` | `stmdb sp!, {r4-r9,sl,fp,lr}`, `sub sp, sp, #68` | `80094ac4: e92d4ff0`, `80094ac8: e24dd044` | **CONFIRMED** |
| `CM_CLKSEL1_CORE` | Write stock word to virtual pool `0xc6008240` | **Not found in XIP virtual pool** (only field RMW-setters) | Search `40 82 00 c6`: setters `0x80059960`–`0x80059a94` | **ABSENT IN power.dll XIP** |
| `CM_CLKSEL1_CORE` | Write in bootstrap (physical address `0x48008240`) | **`0x08301044`** | `80001924: ldr r1, [pc, #1316] ; 0x80001e50`, `80001928: str r1, [r0, #576]` | **FOUND (Bootstrap)** |
| `CM_CLKSEL_MPU` | Write stock divider to virtual pool `0xc6008140` | **Not found in XIP virtual pool** (only `r1` setter and getter) | Search `40 81 00 c6`: `0x80059914`, `0x8005eab4` | **ABSENT IN power.dll XIP** |
| `CM_CLKSEL_MPU` | Write in bootstrap (physical address `0x48008140`) | **`0x00000002`** (divider 2) | `8000192c: mov r1, #2`, `80001930: str r1, [r0, #320]` | **FOUND (Bootstrap)** |
| `CM_CLKSEL1_PLL` | Write DPLL multiplier in bootstrap | **`mult = 97 (0x61), div = 6`** (for 19.2 MHz -> 266.057 MHz) | `80001968: mov r1, #97`, `8000196c: mov r2, #6`, `800019a4: str r7, [r0, #1344]` | **FOUND (Bootstrap)** |
| 330 MHz runtime | Multiplier 86 (330 MHz) / ramp in XIP | **Not found in resident XIP** | Exhaustive ROM scan for `0x00056400` and `str` to `0x540` | **IN PAGED PART (ROFS)** |
