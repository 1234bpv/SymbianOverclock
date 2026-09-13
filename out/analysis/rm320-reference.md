# RM-320 Reference: Signatures of Three Clocking Routines in XIP

Goal — find the same three routines in other OMAP2420 devices by their code
(signature), rather than by address: each firmware has its own addresses. All vaddr below
are for RM-320 (`out/roms/rm320.rom`), XIP base `0x80000000`, so
`file_offset = vaddr − 0x80000000`.

Disassemble window: `node tools/disrom.js <rom> <vaddrHex> <lenHex>`
(ARM objdump, automatically substitutes base). The OMAP2420 multiplier register
`CM_CLKSEL1_PLL = 0xc6008540` is present in EVERY image — this is the main anchor,
search for the rest starting from it.

## 1. Multiplier Reader Function (Near Pool 0xc6008540)

RM-320: pool `0x800b16ec` contains the word `0xc6008540`. Function `0x800b1544`
reads it and computes the multiplier:

```
800b1544: e92d4010  stmdb sp!, {r4, lr}
800b1548: e59f0198  ldr r0, [pc, #408]   ; -> register address
800b154c: eb0001c3  bl 0x800b1c60        ; reader veneer (e51ff004 ldr pc,[pc,#-4])
800b1560: e59f0184  ldr r0, [pc, #388]   ; = 0x800b16ec (pool 0xc6008540)
800b1564: eb0001bd  bl 0x800b1c60
800b156c: e1a00300  mov r0, r0, lsl #6    ; extract multiplier CLKSEL bits
800b1570: e1b00ea0  movs r0, r0, lsr #29
800b1574: 03a000c0  moveq r0, #192        ; 0xc0 = 192 (multiplier value)
800b1580: 03a00082  moveq r0, #130        ; 0x82
800b1584: 13a00078  movne r0, #120        ; 0x78
```

What to search for in another device: `ldr rX,[pc]` to a pool with the word `0xc6008540`,
followed immediately by `bl` to the veneer `e51ff004` (`ldr pc,[pc,#-4]`). Record: pool vaddr,
reader function vaddr, veneer vaddr.

## 2. SRAM Relock Routine (KRomSramRoutine)

RM-320: `0x800a7f6c`. This is the routine whose copy the kernel keeps in SRAM and executes when
idle; we check the first word of its copy against ROM.

```
800a7f6c: e92d40fe  stmdb sp!, {r1,r2,r3,r4,r5,r6,r7, lr}  ; UNTYPICAL mask {r1-r7,lr}
800a7f70: e59f1128  ldr r1, [pc, #296]
800a7f7c: ee073f9a  mcr 15, 0, r3, cr7, cr10, {4}          ; write buffer barrier
800a7f80: e5914070  ldr r4, [r1, #112]
800a7f84: e3844048  orr r4, r4, #72       ; 0x48 - sets bits in register
800a7f88: e5814070  str r4, [r1, #112]
```

Signature to search for: prologue `e92d40fe` (push {r1-r7,lr}) TOGETHER with nearby
`mcr p15, 0, rX, cr7, cr10, {4}` (`ee..3f9a`) — this combination is almost unique.
Record the prologue vaddr as `iRomSramRoutine`.

## 3. DSP Divider Setter (KDspSetter)

RM-320: `0x800ac59c`. Validates the divider argument and sets it.

```
800ac59c: e3510009  cmp r1, #9
800ac5a0: e92d4010  stmdb sp!, {r4, lr}
800ac5bc: e3510010  cmp r1, #16           ; 0x10
800ac5c0: 13510012  cmpne r1, #18         ; 0x12
800ac5c8: e3e00005  mvn r0, #5            ; returns -6 (KErrArgument)
800ac5cc: e8bd8010  ldmia sp!, {r4, pc}
```

Signature to search for: `cmp r1,#9` immediately before `stmdb {r4,lr}`, followed by a sequence of
`cmp/cmpne r1` against values 9,16,18 and `mvn r0,#5`. Record the vaddr of `cmp r1,#9`
as `iDspSetter`.

## Report Format (`out/analysis/<dev>.md`)

For each item: found vaddr + 6–10 lines of disassembly as evidence. If
signature is not found — state "not found" explicitly, do not guess. Multiplier pool
`0xc6008540` is already confirmed in every image (see `findings.md` §30); take its
vaddr from there as the starting point.
