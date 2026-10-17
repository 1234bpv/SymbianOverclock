# Offline analysis: North American N95 RM-160 and N95 8GB RM-421

Raw output behind findings.md §55 (2026-10-17). Images: `out/roms/rm160-<build>.rom`,
`out/roms/rm421-<build>.rom`, unpacked by `node tools/fpsx_extract.js <MCU> <out>` from
`../firmware/n95-rm-160-*` and `../firmware/n95-8gb-rm-421-*`. Reference: `out/roms/rm320.rom`
(31.0.015). Nothing here was checked on a phone. The three scripts that produced it are at
the end, so the run can be repeated: `node namscan.js rm320.rom <images...>`,
`node namdetail.js rm320.rom <images...>`, `node namdiff.js <a.rom> <b.rom> <module regex>`.

## 1. Instrument check on images with known answers

```

==== rm320.rom  size 0x2959200
reset vector 0xea00007e
iRomBase 0x80000000 iRomSize 0x3a00000 rootList 0x8004a748 files 2719
dvfs files in ROM dir: \Sys\Bin\dvfs_conf.dll, \Sys\Bin\dvfs_monitor.dll, \Sys\Bin\dvfs_hal.dll, \Sys\Bin\dvfs_engine.dll, \Sys\Bin\dvfs_interface.ldd
ascii "dvfs_interface": 0x80148998
power: \Sys\Bin\power.dll hdr 0x800a7c20 code 0x800a7c98 codeSize 0xb600 text 0xb600 entry 0x800a7c98 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\power.dll code 0x800a7c98 codeSize 0xb600 -> same address: true, same size: true
   whole code section: IDENTICAL (46592 bytes)
dvfs: \Sys\Bin\dvfs_interface.ldd hdr 0x801480b0 code 0x80148128 codeSize 0xa24 text 0xa24 entry 0x80148128 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\dvfs_interface.ldd code 0x80148128 codeSize 0xa24 -> same address: true, same size: true
   whole code section: IDENTICAL (2596 bytes)
usbc: \Sys\Bin\eusbc.ldd hdr 0x8010a020 code 0x8010a098 codeSize 0x4244 text 0x4244 entry 0x8010a098 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\eusbc.ldd code 0x8010a098 codeSize 0x4244 -> same address: true, same size: true
   whole code section: IDENTICAL (16964 bytes)
ekern: \Sys\Bin\ekern.exe hdr 0x80069f88 code 0x8006a000 codeSize 0x378cc text 0x378cc entry 0x8006a110 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\ekern.exe code 0x8006a000 codeSize 0x378cc -> same address: true, same size: true
   whole code section: IDENTICAL (227532 bytes)
dvfs DoControl(1) body signature hits (1): 0x80148410
Usbc DoControl prologue signature hits (1): 0x8010bd04
relock routine signature hits (1): 0x800a7f6c
@RM-320 relock 0x800a7f6c: 80/80 words equal
@RM-320 dsp setter 0x800ac59c: 24/24 words equal
@RM-320 mult reader 0x800b1544: 24/24 words equal
@RM-320 pool 0x800b16ec: 1/1 words equal
@RM-320 dvfs body 0x80148410: 6/6 words equal
@RM-320 usbc DoControl 0x8010bd04: 16/16 words equal
body@0x80148410 vs RM-320 body: 6/6 exact; words 0xe92d4010 0xe3a00001 0xeb00014c 0xe59f11d0 0xe8bd4010 0xea000135

==== rm320-35.0.001.rom  size 0x2959200
reset vector 0xea00007e
iRomBase 0x80000000 iRomSize 0x3a00000 rootList 0x8004a748 files 2719
dvfs files in ROM dir: \Sys\Bin\dvfs_conf.dll, \Sys\Bin\dvfs_monitor.dll, \Sys\Bin\dvfs_hal.dll, \Sys\Bin\dvfs_engine.dll, \Sys\Bin\dvfs_interface.ldd
ascii "dvfs_interface": 0x80148998
power: \Sys\Bin\power.dll hdr 0x800a7c20 code 0x800a7c98 codeSize 0xb600 text 0xb600 entry 0x800a7c98 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\power.dll code 0x800a7c98 codeSize 0xb600 -> same address: true, same size: true
   whole code section: IDENTICAL (46592 bytes)
dvfs: \Sys\Bin\dvfs_interface.ldd hdr 0x801480b0 code 0x80148128 codeSize 0xa24 text 0xa24 entry 0x80148128 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\dvfs_interface.ldd code 0x80148128 codeSize 0xa24 -> same address: true, same size: true
   whole code section: IDENTICAL (2596 bytes)
usbc: \Sys\Bin\eusbc.ldd hdr 0x8010a020 code 0x8010a098 codeSize 0x4244 text 0x4244 entry 0x8010a098 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\eusbc.ldd code 0x8010a098 codeSize 0x4244 -> same address: true, same size: true
   whole code section: IDENTICAL (16964 bytes)
ekern: \Sys\Bin\ekern.exe hdr 0x80069f88 code 0x8006a000 codeSize 0x378cc text 0x378cc entry 0x8006a110 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\ekern.exe code 0x8006a000 codeSize 0x378cc -> same address: true, same size: true
   whole code section: IDENTICAL (227532 bytes)
dvfs DoControl(1) body signature hits (1): 0x80148410
Usbc DoControl prologue signature hits (1): 0x8010bd04
relock routine signature hits (1): 0x800a7f6c
@RM-320 relock 0x800a7f6c: 80/80 words equal
@RM-320 dsp setter 0x800ac59c: 24/24 words equal
@RM-320 mult reader 0x800b1544: 24/24 words equal
@RM-320 pool 0x800b16ec: 1/1 words equal
@RM-320 dvfs body 0x80148410: 6/6 words equal
@RM-320 usbc DoControl 0x8010bd04: 16/16 words equal
body@0x80148410 vs RM-320 body: 6/6 exact; words 0xe92d4010 0xe3a00001 0xeb00014c 0xe59f11d0 0xe8bd4010 0xea000135

==== n159.rom  size 0x28e9200
reset vector 0xea00007e
iRomBase 0x80000000 iRomSize 0x3a00000 rootList 0x8004a6fc files 2698
dvfs files in ROM dir: \Sys\Bin\dvfs_conf.dll, \Sys\Bin\dvfs_monitor.dll, \Sys\Bin\dvfs_hal.dll, \Sys\Bin\dvfs_engine.dll, \Sys\Bin\dvfs_interface.ldd
ascii "dvfs_interface": 0x8014bb78
power: \Sys\Bin\power.dll hdr 0x800a7c20 code 0x800a7c98 codeSize 0xb600 text 0xb600 entry 0x800a7c98 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\power.dll code 0x800a7c98 codeSize 0xb600 -> same address: true, same size: true
   whole code section: 19 bytes differ of 46592, first at +0xa00c
dvfs: \Sys\Bin\dvfs_interface.ldd hdr 0x8014b290 code 0x8014b308 codeSize 0xa24 text 0xa24 entry 0x8014b308 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\dvfs_interface.ldd code 0x80148128 codeSize 0xa24 -> same address: false, same size: true
   whole code section: 84 bytes differ of 2596, first at +0x10
usbc: \Sys\Bin\eusbc.ldd hdr 0x8010d240 code 0x8010d2b8 codeSize 0x4244 text 0x4244 entry 0x8010d2b8 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\eusbc.ldd code 0x8010a098 codeSize 0x4244 -> same address: false, same size: true
   whole code section: 276 bytes differ of 16964, first at +0x10
ekern: \Sys\Bin\ekern.exe hdr 0x80069f88 code 0x8006a000 codeSize 0x378cc text 0x378cc entry 0x8006a110 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\ekern.exe code 0x8006a000 codeSize 0x378cc -> same address: true, same size: true
   whole code section: IDENTICAL (227532 bytes)
dvfs DoControl(1) body signature hits (1): 0x8014b5f0
Usbc DoControl prologue signature hits (1): 0x8010ef24
relock routine signature hits (1): 0x800a7f6c
@RM-320 relock 0x800a7f6c: 80/80 words equal
@RM-320 dsp setter 0x800ac59c: 24/24 words equal
@RM-320 mult reader 0x800b1544: 24/24 words equal
@RM-320 pool 0x800b16ec: 1/1 words equal
@RM-320 dvfs body 0x80148410: 0/6 words equal  first diffs [[0,"0xe92d4010","0xe1811300"],[1,"0xe3a00001","0xe59f00b0"],[2,"0xeb00014c","0xe0810000"]]
@RM-320 usbc DoControl 0x8010bd04: 0/16 words equal  first diffs [[0,"0xe92d4ff0","0xe0800182"],[1,"0xe24dd044","0xe5900114"],[2,"0xe1a04000","0xe3500000"]]
body@0x8014b5f0 vs RM-320 body: 6/6 exact; words 0xe92d4010 0xe3a00001 0xeb00014c 0xe59f11d0 0xe8bd4010 0xea000135
```

## 2. NAM images: ROM directory, modules, signatures, fixed RM-320 addresses

```

==== rm421-1.2.011.rom  size 0x281a200
reset vector 0xea00007e
iRomBase 0x80000000 iRomSize 0x3900000 rootList 0x8004267c files 2667
dvfs files in ROM dir: \Sys\Bin\dvfs_conf.dll, \Sys\Bin\dvfs_monitor.dll, \Sys\Bin\dvfs_hal.dll, \Sys\Bin\dvfs_engine.dll, \Sys\Bin\dvfs_interface.ldd
ascii "dvfs_interface": 0x80140b98
power: \Sys\Bin\power.dll hdr 0x8009fb10 code 0x8009fb88 codeSize 0xb600 text 0xb600 entry 0x8009fb88 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\power.dll code 0x800a7c98 codeSize 0xb600 -> same address: false, same size: true
   whole code section: 1597 bytes differ of 46592, first at +0x10
dvfs: \Sys\Bin\dvfs_interface.ldd hdr 0x801402b0 code 0x80140328 codeSize 0xa24 text 0xa24 entry 0x80140328 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\dvfs_interface.ldd code 0x80148128 codeSize 0xa24 -> same address: false, same size: true
   whole code section: 175 bytes differ of 2596, first at +0x11
usbc: \Sys\Bin\eusbc.ldd hdr 0x80101dc0 code 0x80101e38 codeSize 0x4230 text 0x4230 entry 0x80101e38 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\eusbc.ldd code 0x8010a098 codeSize 0x4244 -> same address: false, same size: false
ekern: \Sys\Bin\ekern.exe hdr 0x80061f88 code 0x80062000 codeSize 0x37820 text 0x37820 entry 0x80062110 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\ekern.exe code 0x8006a000 codeSize 0x378cc -> same address: false, same size: false
dvfs DoControl(1) body signature hits (1): 0x80140610
Usbc DoControl prologue signature hits (1): 0x80103aa4
relock routine signature hits (1): 0x8009fe5c
@RM-320 relock 0x800a7f6c: 0/80 words equal  first diffs [[0,"0xe92d40fe","0xeb000669"],[1,"0xe59f1128","0xe1a00006"],[2,"0xe59f5128","0xe8bd81fc"]]
@RM-320 dsp setter 0x800ac59c: 0/24 words equal  first diffs [[0,"0xe3510009","0xe1a04000"],[1,"0xe92d4010","0xe3a00000"],[2,"0xa000007","0xe5840018"]]
@RM-320 mult reader 0x800b1544: 0/24 words equal  first diffs [[0,"0xe92d4010","0xe8bd8010"],[1,"0xe59f0198","0xe12fff1e"],[2,"0xeb0001c3","0xe59f21b4"]]
@RM-320 pool 0x800b16ec: 0/1 words equal  first diffs [[0,"0xc6008540","0x15901000"]]
@RM-320 dvfs body 0x80148410: 0/6 words equal  first diffs [[0,"0xe92d4010","0xe5820000"],[1,"0xe3a00001","0xe12fff1e"],[2,"0xeb00014c","0xe92d40f8"]]
@RM-320 usbc DoControl 0x8010bd04: 0/16 words equal  first diffs [[0,"0xe92d4ff0","0xbb000db6"],[1,"0xe24dd044","0xe59d0008"],[2,"0xe1a04000","0xeb000dde"]]
body@0x80140610 vs RM-320 body: 6/6 exact; words 0xe92d4010 0xe3a00001 0xeb00014c 0xe59f11d0 0xe8bd4010 0xea000135

==== rm421-20.2.005.rom  size 0x281e200
reset vector 0xea00007e
iRomBase 0x80000000 iRomSize 0x3900000 rootList 0x8004267c files 2667
dvfs files in ROM dir: \Sys\Bin\dvfs_conf.dll, \Sys\Bin\dvfs_monitor.dll, \Sys\Bin\dvfs_hal.dll, \Sys\Bin\dvfs_engine.dll, \Sys\Bin\dvfs_interface.ldd
ascii "dvfs_interface": 0x80140cc8
power: \Sys\Bin\power.dll hdr 0x8009fbc0 code 0x8009fc38 codeSize 0xb600 text 0xb600 entry 0x8009fc38 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\power.dll code 0x800a7c98 codeSize 0xb600 -> same address: false, same size: true
   whole code section: 1479 bytes differ of 46592, first at +0x10
dvfs: \Sys\Bin\dvfs_interface.ldd hdr 0x801403e0 code 0x80140458 codeSize 0xa24 text 0xa24 entry 0x80140458 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\dvfs_interface.ldd code 0x80148128 codeSize 0xa24 -> same address: false, same size: true
   whole code section: 169 bytes differ of 2596, first at +0x10
usbc: \Sys\Bin\eusbc.ldd hdr 0x80101ee0 code 0x80101f58 codeSize 0x4244 text 0x4244 entry 0x80101f58 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\eusbc.ldd code 0x8010a098 codeSize 0x4244 -> same address: false, same size: true
   whole code section: 377 bytes differ of 16964, first at +0x10
ekern: \Sys\Bin\ekern.exe hdr 0x80061f88 code 0x80062000 codeSize 0x378cc text 0x378cc entry 0x80062110 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\ekern.exe code 0x8006a000 codeSize 0x378cc -> same address: false, same size: true
   whole code section: 3252 bytes differ of 227532, first at +0xa5
dvfs DoControl(1) body signature hits (1): 0x80140740
Usbc DoControl prologue signature hits (1): 0x80103bc4
relock routine signature hits (1): 0x8009ff0c
@RM-320 relock 0x800a7f6c: 0/80 words equal  first diffs [[0,"0xe92d40fe","0xe3500001"],[1,"0xe59f1128","0x1a000015"],[2,"0xe59f5128","0xe5951008"]]
@RM-320 dsp setter 0x800ac59c: 0/24 words equal  first diffs [[0,"0xe3510009","0x0"],[1,"0xe92d4010","0xe92d4070"],[2,"0xa000007","0xe59f629c"]]
@RM-320 mult reader 0x800b1544: 0/24 words equal  first diffs [[0,"0xe92d4010","0xe5c02004"],[1,"0xe59f0198","0xe3a01000"],[2,"0xeb0001c3","0xe5c03006"]]
@RM-320 pool 0x800b16ec: 0/1 words equal  first diffs [[0,"0xc6008540","0xe2853020"]]
@RM-320 dvfs body 0x80148410: 0/6 words equal  first diffs [[0,"0xe92d4010","0xe59f6d10"],[1,"0xe3a00001","0xe3e0400a"],[2,"0xeb00014c","0xe5960000"]]
@RM-320 usbc DoControl 0x8010bd04: 0/16 words equal  first diffs [[0,"0xe92d4ff0","0xe59f0458"],[1,"0xe24dd044","0xe3a01009"],[2,"0xe1a04000","0xeb000dfe"]]
body@0x80140740 vs RM-320 body: 6/6 exact; words 0xe92d4010 0xe3a00001 0xeb00014c 0xe59f11d0 0xe8bd4010 0xea000135

==== rm421-35.2.002.rom  size 0x2970200
reset vector 0xea00007e
iRomBase 0x80000000 iRomSize 0x3a00000 rootList 0x8004a738 files 2714
dvfs files in ROM dir: \Sys\Bin\dvfs_conf.dll, \Sys\Bin\dvfs_monitor.dll, \Sys\Bin\dvfs_hal.dll, \Sys\Bin\dvfs_engine.dll, \Sys\Bin\dvfs_interface.ldd
ascii "dvfs_interface": 0x80148998
power: \Sys\Bin\power.dll hdr 0x800a7c20 code 0x800a7c98 codeSize 0xb600 text 0xb600 entry 0x800a7c98 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\power.dll code 0x800a7c98 codeSize 0xb600 -> same address: true, same size: true
   whole code section: 10 bytes differ of 46592, first at +0xaa61
dvfs: \Sys\Bin\dvfs_interface.ldd hdr 0x801480b0 code 0x80148128 codeSize 0xa24 text 0xa24 entry 0x80148128 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\dvfs_interface.ldd code 0x80148128 codeSize 0xa24 -> same address: true, same size: true
   whole code section: IDENTICAL (2596 bytes)
usbc: \Sys\Bin\eusbc.ldd hdr 0x8010a020 code 0x8010a098 codeSize 0x4244 text 0x4244 entry 0x8010a098 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\eusbc.ldd code 0x8010a098 codeSize 0x4244 -> same address: true, same size: true
   whole code section: IDENTICAL (16964 bytes)
ekern: \Sys\Bin\ekern.exe hdr 0x80069f88 code 0x8006a000 codeSize 0x378cc text 0x378cc entry 0x8006a110 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\ekern.exe code 0x8006a000 codeSize 0x378cc -> same address: true, same size: true
   whole code section: IDENTICAL (227532 bytes)
dvfs DoControl(1) body signature hits (1): 0x80148410
Usbc DoControl prologue signature hits (1): 0x8010bd04
relock routine signature hits (1): 0x800a7f6c
@RM-320 relock 0x800a7f6c: 80/80 words equal
@RM-320 dsp setter 0x800ac59c: 24/24 words equal
@RM-320 mult reader 0x800b1544: 24/24 words equal
@RM-320 pool 0x800b16ec: 1/1 words equal
@RM-320 dvfs body 0x80148410: 6/6 words equal
@RM-320 usbc DoControl 0x8010bd04: 16/16 words equal
body@0x80148410 vs RM-320 body: 6/6 exact; words 0xe92d4010 0xe3a00001 0xeb00014c 0xe59f11d0 0xe8bd4010 0xea000135

==== rm160-20.2.011.rom  size 0x286c200
reset vector 0xea00007e
iRomBase 0x80000000 iRomSize 0x3900000 rootList 0x8003cef4 files 2696
dvfs files in ROM dir: \Sys\Bin\dvfs_conf.dll, \Sys\Bin\dvfs_monitor.dll, \Sys\Bin\dvfs_hal.dll, \Sys\Bin\dvfs_engine.dll, \Sys\Bin\dvfs_interface.ldd
ascii "dvfs_interface": 0x8013ed38
power: \Sys\Bin\power.dll hdr 0x8009ab10 code 0x8009ab88 codeSize 0xb600 text 0xb600 entry 0x8009ab88 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\power.dll code 0x800a7c98 codeSize 0xb600 -> same address: false, same size: true
   whole code section: 1678 bytes differ of 46592, first at +0x10
dvfs: \Sys\Bin\dvfs_interface.ldd hdr 0x8013e450 code 0x8013e4c8 codeSize 0xa24 text 0xa24 entry 0x8013e4c8 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\dvfs_interface.ldd code 0x80148128 codeSize 0xa24 -> same address: false, same size: true
   whole code section: 274 bytes differ of 2596, first at +0x10
usbc: \Sys\Bin\eusbc.ldd hdr 0x800fffa0 code 0x80100018 codeSize 0x4230 text 0x4230 entry 0x80100018 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\eusbc.ldd code 0x8010a098 codeSize 0x4244 -> same address: false, same size: false
ekern: \Sys\Bin\ekern.exe hdr 0x8005cf88 code 0x8005d000 codeSize 0x37820 text 0x37820 entry 0x8005d110 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\ekern.exe code 0x8006a000 codeSize 0x378cc -> same address: false, same size: false
dvfs DoControl(1) body signature hits (1): 0x8013e7b0
Usbc DoControl prologue signature hits (1): 0x80101c84
relock routine signature hits (1): 0x8009ae5c
@RM-320 relock 0x800a7f6c: 0/80 words equal  first diffs [[0,"0xe92d40fe","0xeb0000d4"],[1,"0xe59f1128","0xe5941010"],[2,"0xe59f5128","0xe3a00003"]]
@RM-320 dsp setter 0x800ac59c: 0/24 words equal  first diffs [[0,"0xe3510009","0xeb00038a"],[1,"0xe92d4010","0xe8bd401c"],[2,"0xa000007","0xe3a01008"]]
@RM-320 mult reader 0x800b1544: 0/24 words equal  first diffs [[0,"0xe92d4010","0xe28d805c"],[1,"0xe59f0198","0xe5900008"],[2,"0xeb0001c3","0xe89801e0"]]
@RM-320 pool 0x800b16ec: 0/1 words equal  first diffs [[0,"0xc6008540","0x24c2004"]]
@RM-320 dvfs body 0x80148410: 0/6 words equal  first diffs [[0,"0xe92d4010","0xeb000403"],[1,"0xe3a00001","0xe5840004"],[2,"0xeb00014c","0xe5951004"]]
@RM-320 usbc DoControl 0x8010bd04: 0/16 words equal  first diffs [[0,"0xe92d4ff0","0xe5c4000a"],[1,"0xe24dd044","0xe1a00007"],[2,"0xe1a04000","0xeb000656"]]
body@0x8013e7b0 vs RM-320 body: 6/6 exact; words 0xe92d4010 0xe3a00001 0xeb00014c 0xe59f11d0 0xe8bd4010 0xea000135

==== rm160-35.2.001.rom  size 0x286c200
reset vector 0xea00007e
iRomBase 0x80000000 iRomSize 0x3900000 rootList 0x8003cef4 files 2696
dvfs files in ROM dir: \Sys\Bin\dvfs_conf.dll, \Sys\Bin\dvfs_monitor.dll, \Sys\Bin\dvfs_hal.dll, \Sys\Bin\dvfs_engine.dll, \Sys\Bin\dvfs_interface.ldd
ascii "dvfs_interface": 0x8013ed38
power: \Sys\Bin\power.dll hdr 0x8009ab10 code 0x8009ab88 codeSize 0xb600 text 0xb600 entry 0x8009ab88 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\power.dll code 0x800a7c98 codeSize 0xb600 -> same address: false, same size: true
   whole code section: 1678 bytes differ of 46592, first at +0x10
dvfs: \Sys\Bin\dvfs_interface.ldd hdr 0x8013e450 code 0x8013e4c8 codeSize 0xa24 text 0xa24 entry 0x8013e4c8 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\dvfs_interface.ldd code 0x80148128 codeSize 0xa24 -> same address: false, same size: true
   whole code section: 274 bytes differ of 2596, first at +0x10
usbc: \Sys\Bin\eusbc.ldd hdr 0x800fffa0 code 0x80100018 codeSize 0x4230 text 0x4230 entry 0x80100018 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\eusbc.ldd code 0x8010a098 codeSize 0x4244 -> same address: false, same size: false
ekern: \Sys\Bin\ekern.exe hdr 0x8005cf88 code 0x8005d000 codeSize 0x37820 text 0x37820 entry 0x8005d110 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\ekern.exe code 0x8006a000 codeSize 0x378cc -> same address: false, same size: false
dvfs DoControl(1) body signature hits (1): 0x8013e7b0
Usbc DoControl prologue signature hits (1): 0x80101c84
relock routine signature hits (1): 0x8009ae5c
@RM-320 relock 0x800a7f6c: 0/80 words equal  first diffs [[0,"0xe92d40fe","0xeb0000d4"],[1,"0xe59f1128","0xe5941010"],[2,"0xe59f5128","0xe3a00003"]]
@RM-320 dsp setter 0x800ac59c: 0/24 words equal  first diffs [[0,"0xe3510009","0xeb00038a"],[1,"0xe92d4010","0xe8bd401c"],[2,"0xa000007","0xe3a01008"]]
@RM-320 mult reader 0x800b1544: 0/24 words equal  first diffs [[0,"0xe92d4010","0xe28d805c"],[1,"0xe59f0198","0xe5900008"],[2,"0xeb0001c3","0xe89801e0"]]
@RM-320 pool 0x800b16ec: 0/1 words equal  first diffs [[0,"0xc6008540","0x24c2004"]]
@RM-320 dvfs body 0x80148410: 0/6 words equal  first diffs [[0,"0xe92d4010","0xeb000403"],[1,"0xe3a00001","0xe5840004"],[2,"0xeb00014c","0xe5951004"]]
@RM-320 usbc DoControl 0x8010bd04: 0/16 words equal  first diffs [[0,"0xe92d4ff0","0xe5c4000a"],[1,"0xe24dd044","0xe1a00007"],[2,"0xe1a04000","0xeb000656"]]
body@0x8013e7b0 vs RM-320 body: 6/6 exact; words 0xe92d4010 0xe3a00001 0xeb00014c 0xe59f11d0 0xe8bd4010 0xea000135

==== rm160-10.2.006.rom  size 0x15a000
reset vector 0xea00007e
iRomBase 0x80000000 iRomSize 0x200000 rootList 0x80007d80 files 76
dvfs files in ROM dir: NONE
ascii "dvfs_interface": none
power: \Sys\Bin\power.dll hdr 0x80047e80 code 0x80047ef8 codeSize 0xb320 text 0xb320 entry 0x80047ef8 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\power.dll code 0x800a7c98 codeSize 0xb600 -> same address: false, same size: false
dvfs: not in ROM dir
usbc: \Sys\Bin\eusbc.ldd hdr 0x80088590 code 0x80088608 codeSize 0x4204 text 0x4204 entry 0x80088608 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\eusbc.ldd code 0x8010a098 codeSize 0x4244 -> same address: false, same size: false
ekern: \Sys\Bin\ekern.exe hdr 0x80008f88 code 0x80009000 codeSize 0x31c64 text 0x31c64 entry 0x80009110 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\ekern.exe code 0x8006a000 codeSize 0x378cc -> same address: false, same size: false
dvfs DoControl(1) body signature hits (0): 
Usbc DoControl prologue signature hits (1): 0x8008a270
relock routine signature hits (1): 0x800481c4
@RM-320 relock 0x800a7f6c: 0/80 words equal  first diffs [[0,"0xe92d40fe","0xe584049c"],[1,"0xe59f1128","0xe3a05102"],[2,"0xe59f5128","0xe58db03c"]]
@RM-320 dsp setter 0x800ac59c: 0/24 words equal  first diffs [[0,"0xe3510009","0xe28b1004"],[1,"0xe92d4010","0xebffe0d3"],[2,"0xa000007","0xe3500000"]]
@RM-320 mult reader 0x800b1544: 0/24 words equal  first diffs [[0,"0xe92d4010","0x70736944"],[1,"0xe59f0198","0x4479616c"],[2,"0xeb0001c3","0x63697665"]]
@RM-320 pool 0x800b16ec: 0/1 words equal  first diffs [[0,"0xc6008540","0x800b1430"]]
@RM-320 dvfs body 0x80148410: 0/6 words equal  first diffs [[0,"0xe92d4010","0xc00018"],[1,"0xe3a00001","0x7ffffb69"],[2,"0xeb00014c","0x0"]]
@RM-320 usbc DoControl 0x8010bd04: 0/16 words equal  first diffs [[0,"0xe92d4ff0","0x80018400"],[1,"0xe24dd044","0x7ffdb69e"],[2,"0xe1a04000","0x1"]]

==== rm160-11.2.009.rom  size 0x15a000
reset vector 0xea00007e
iRomBase 0x80000000 iRomSize 0x200000 rootList 0x80007d80 files 76
dvfs files in ROM dir: NONE
ascii "dvfs_interface": none
power: \Sys\Bin\power.dll hdr 0x80047e80 code 0x80047ef8 codeSize 0xb320 text 0xb320 entry 0x80047ef8 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\power.dll code 0x800a7c98 codeSize 0xb600 -> same address: false, same size: false
dvfs: not in ROM dir
usbc: \Sys\Bin\eusbc.ldd hdr 0x80088590 code 0x80088608 codeSize 0x4204 text 0x4204 entry 0x80088608 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\eusbc.ldd code 0x8010a098 codeSize 0x4244 -> same address: false, same size: false
ekern: \Sys\Bin\ekern.exe hdr 0x80008f88 code 0x80009000 codeSize 0x31c64 text 0x31c64 entry 0x80009110 [code = hdr+120: linear]
   vs RM-320 \Sys\Bin\ekern.exe code 0x8006a000 codeSize 0x378cc -> same address: false, same size: false
dvfs DoControl(1) body signature hits (0): 
Usbc DoControl prologue signature hits (1): 0x8008a270
relock routine signature hits (1): 0x800481c4
@RM-320 relock 0x800a7f6c: 0/80 words equal  first diffs [[0,"0xe92d40fe","0xe584049c"],[1,"0xe59f1128","0xe3a05102"],[2,"0xe59f5128","0xe58db03c"]]
@RM-320 dsp setter 0x800ac59c: 0/24 words equal  first diffs [[0,"0xe3510009","0xe28b1004"],[1,"0xe92d4010","0xebffe0d3"],[2,"0xa000007","0xe3500000"]]
@RM-320 mult reader 0x800b1544: 0/24 words equal  first diffs [[0,"0xe92d4010","0x70736944"],[1,"0xe59f0198","0x4479616c"],[2,"0xeb0001c3","0x63697665"]]
@RM-320 pool 0x800b16ec: 0/1 words equal  first diffs [[0,"0xc6008540","0x800b1430"]]
@RM-320 dvfs body 0x80148410: 0/6 words equal  first diffs [[0,"0xe92d4010","0xc00018"],[1,"0xe3a00001","0x7ffffb69"],[2,"0xeb00014c","0x0"]]
@RM-320 usbc DoControl 0x8010bd04: 0/16 words equal  first diffs [[0,"0xe92d4ff0","0x80018400"],[1,"0xe24dd044","0x7ffdb69e"],[2,"0xe1a04000","0x1"]]
```

## 3. Per-build routines, module offsets, word comparison, power.dll data

```
RM-320 power.dll code 0x800a7c98 data 0x800b3298 dataBssLin 0xc80030c0 dataSize 0x34 bss 0x174 total 0x1a8
RM-320 offsets: relock-code 0x2d4 dsp-code 0x4904 reader-code 0x98ac body-dvfscode 0x2e8 usbc-eusbccode 0x1c6c global-dataBssLin 0x8 hint-dataBssLin 0x2c
RM-320 power.dll literals into own data: 0xc80030c0x1 0xc80030c8x1 0xc80030d0x1 0xc80030d4x1 0xc80030d8x1 0xc80030dcx1 0xc80030e4x1 0xc80030ecx1 0xc80030f4x1 0xc8003170x1 0xc80031ecx1

==== rm320.rom
power.dll code 0x800a7c98..0x800b3298 dataBssLin 0xc80030c0 dataSize 0x34 bss 0x174 total 0x1a8
relock hits 0x800a7f6c in power.dll: true offset 0x2d4 (RM-320 0x2d4)
DSP setter (11 exact words) hits 0x800ac59c,0x800ac614 in power.dll: true,true offset 0x4904,0x497c (RM-320 0x4904)
mult reader hits 0x800b1544 offset 0x98ac (RM-320 0x98ac)
dvfs body hits 0x80148410 in dvfs ldd: true offset 0x2e8 (RM-320 0x2e8)
  relock 0x50 words vs RM-320: 80 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 80
  relock first 8 words: 0xe92d40fe 0xe59f1128 0xe59f5128 0xe3a03000 0xee073f9a 0xe5914070 0xe3844048 0xe5814070
  dsp setter 24 words vs RM-320: 24 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 24
  dsp setter 24 words vs RM-320: 15 exact + 2 PC-relative (same opcode, other offset) + 7 real differences of 24 [13:0xe1a02001/0xe1a02401 15:0xe3a0101f/0xe3a01c1f 19:0xe3510001/0xe3510000 20:0x13510002/0xe59f029c 21:0x13e00005/0x3a02000 22:0xe92d4010/0x3a01080]
  reader 40 words vs RM-320: 40 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 40
  reader ldr @+0x1c -> pool 0x800b16ec = 0xc6008540
  body + dispatcher 0x40 words vs RM-320: 64 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 64
power.dll literals into own data: 0xc80030c0x1(+0x0) 0xc80030c8x1(+0x8) 0xc80030d0x1(+0x10) 0xc80030d4x1(+0x14) 0xc80030d8x1(+0x18) 0xc80030dcx1(+0x1c) 0xc80030e4x1(+0x24) 0xc80030ecx1(+0x2c) 0xc80030f4x1(+0x34) 0xc8003170x1(+0xb0) 0xc80031ecx1(+0x12c)
RM-320 global offset +0x8 -> here 0xc80030c8; hint offset +0x2c -> here 0xc80030ec

==== n159.rom
power.dll code 0x800a7c98..0x800b3298 dataBssLin 0xc80030c0 dataSize 0x34 bss 0x174 total 0x1a8
relock hits 0x800a7f6c in power.dll: true offset 0x2d4 (RM-320 0x2d4)
DSP setter (11 exact words) hits 0x800ac59c,0x800ac614 in power.dll: true,true offset 0x4904,0x497c (RM-320 0x4904)
mult reader hits 0x800b1544 offset 0x98ac (RM-320 0x98ac)
dvfs body hits 0x8014b5f0 in dvfs ldd: true offset 0x2e8 (RM-320 0x2e8)
  relock 0x50 words vs RM-320: 80 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 80
  relock first 8 words: 0xe92d40fe 0xe59f1128 0xe59f5128 0xe3a03000 0xee073f9a 0xe5914070 0xe3844048 0xe5814070
  dsp setter 24 words vs RM-320: 24 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 24
  dsp setter 24 words vs RM-320: 15 exact + 2 PC-relative (same opcode, other offset) + 7 real differences of 24 [13:0xe1a02001/0xe1a02401 15:0xe3a0101f/0xe3a01c1f 19:0xe3510001/0xe3510000 20:0x13510002/0xe59f029c 21:0x13e00005/0x3a02000 22:0xe92d4010/0x3a01080]
  reader 40 words vs RM-320: 40 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 40
  reader ldr @+0x1c -> pool 0x800b16ec = 0xc6008540
  body + dispatcher 0x40 words vs RM-320: 64 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 64
power.dll literals into own data: 0xc80030c0x1(+0x0) 0xc80030c8x1(+0x8) 0xc80030d0x1(+0x10) 0xc80030d4x1(+0x14) 0xc80030d8x1(+0x18) 0xc80030dcx1(+0x1c) 0xc80030e4x1(+0x24) 0xc80030ecx1(+0x2c) 0xc80030f4x1(+0x34) 0xc8003170x1(+0xb0) 0xc80031ecx1(+0x12c)
RM-320 global offset +0x8 -> here 0xc80030c8; hint offset +0x2c -> here 0xc80030ec

==== rm421-1.2.011.rom
power.dll code 0x8009fb88..0x800ab188 dataBssLin 0xc80030c0 dataSize 0x34 bss 0x174 total 0x1a8
relock hits 0x8009fe5c in power.dll: true offset 0x2d4 (RM-320 0x2d4)
DSP setter (11 exact words) hits 0x800a448c,0x800a4504 in power.dll: true,true offset 0x4904,0x497c (RM-320 0x4904)
mult reader hits 0x800a9434 offset 0x98ac (RM-320 0x98ac)
dvfs body hits 0x80140610 in dvfs ldd: true offset 0x2e8 (RM-320 0x2e8)
  relock 0x50 words vs RM-320: 80 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 80
  relock first 8 words: 0xe92d40fe 0xe59f1128 0xe59f5128 0xe3a03000 0xee073f9a 0xe5914070 0xe3844048 0xe5814070
  dsp setter 24 words vs RM-320: 24 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 24
  dsp setter 24 words vs RM-320: 15 exact + 2 PC-relative (same opcode, other offset) + 7 real differences of 24 [13:0xe1a02001/0xe1a02401 15:0xe3a0101f/0xe3a01c1f 19:0xe3510001/0xe3510000 20:0x13510002/0xe59f029c 21:0x13e00005/0x3a02000 22:0xe92d4010/0x3a01080]
  reader 40 words vs RM-320: 40 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 40
  reader ldr @+0x1c -> pool 0x800a95dc = 0xc6008540
  body + dispatcher 0x40 words vs RM-320: 64 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 64
power.dll literals into own data: 0xc80030c0x1(+0x0) 0xc80030c8x1(+0x8) 0xc80030d0x1(+0x10) 0xc80030d4x1(+0x14) 0xc80030d8x1(+0x18) 0xc80030dcx1(+0x1c) 0xc80030e4x1(+0x24) 0xc80030ecx1(+0x2c) 0xc80030f4x1(+0x34) 0xc8003170x1(+0xb0) 0xc80031ecx1(+0x12c)
RM-320 global offset +0x8 -> here 0xc80030c8; hint offset +0x2c -> here 0xc80030ec

==== rm421-20.2.005.rom
power.dll code 0x8009fc38..0x800ab238 dataBssLin 0xc80030c0 dataSize 0x34 bss 0x174 total 0x1a8
relock hits 0x8009ff0c in power.dll: true offset 0x2d4 (RM-320 0x2d4)
DSP setter (11 exact words) hits 0x800a453c,0x800a45b4 in power.dll: true,true offset 0x4904,0x497c (RM-320 0x4904)
mult reader hits 0x800a94e4 offset 0x98ac (RM-320 0x98ac)
dvfs body hits 0x80140740 in dvfs ldd: true offset 0x2e8 (RM-320 0x2e8)
  relock 0x50 words vs RM-320: 80 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 80
  relock first 8 words: 0xe92d40fe 0xe59f1128 0xe59f5128 0xe3a03000 0xee073f9a 0xe5914070 0xe3844048 0xe5814070
  dsp setter 24 words vs RM-320: 24 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 24
  dsp setter 24 words vs RM-320: 15 exact + 2 PC-relative (same opcode, other offset) + 7 real differences of 24 [13:0xe1a02001/0xe1a02401 15:0xe3a0101f/0xe3a01c1f 19:0xe3510001/0xe3510000 20:0x13510002/0xe59f029c 21:0x13e00005/0x3a02000 22:0xe92d4010/0x3a01080]
  reader 40 words vs RM-320: 40 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 40
  reader ldr @+0x1c -> pool 0x800a968c = 0xc6008540
  body + dispatcher 0x40 words vs RM-320: 64 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 64
power.dll literals into own data: 0xc80030c0x1(+0x0) 0xc80030c8x1(+0x8) 0xc80030d0x1(+0x10) 0xc80030d4x1(+0x14) 0xc80030d8x1(+0x18) 0xc80030dcx1(+0x1c) 0xc80030e4x1(+0x24) 0xc80030ecx1(+0x2c) 0xc80030f4x1(+0x34) 0xc8003170x1(+0xb0) 0xc80031ecx1(+0x12c)
RM-320 global offset +0x8 -> here 0xc80030c8; hint offset +0x2c -> here 0xc80030ec

==== rm421-35.2.002.rom
power.dll code 0x800a7c98..0x800b3298 dataBssLin 0xc80030c0 dataSize 0x34 bss 0x174 total 0x1a8
relock hits 0x800a7f6c in power.dll: true offset 0x2d4 (RM-320 0x2d4)
DSP setter (11 exact words) hits 0x800ac59c,0x800ac614 in power.dll: true,true offset 0x4904,0x497c (RM-320 0x4904)
mult reader hits 0x800b1544 offset 0x98ac (RM-320 0x98ac)
dvfs body hits 0x80148410 in dvfs ldd: true offset 0x2e8 (RM-320 0x2e8)
  relock 0x50 words vs RM-320: 80 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 80
  relock first 8 words: 0xe92d40fe 0xe59f1128 0xe59f5128 0xe3a03000 0xee073f9a 0xe5914070 0xe3844048 0xe5814070
  dsp setter 24 words vs RM-320: 24 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 24
  dsp setter 24 words vs RM-320: 15 exact + 2 PC-relative (same opcode, other offset) + 7 real differences of 24 [13:0xe1a02001/0xe1a02401 15:0xe3a0101f/0xe3a01c1f 19:0xe3510001/0xe3510000 20:0x13510002/0xe59f029c 21:0x13e00005/0x3a02000 22:0xe92d4010/0x3a01080]
  reader 40 words vs RM-320: 40 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 40
  reader ldr @+0x1c -> pool 0x800b16ec = 0xc6008540
  body + dispatcher 0x40 words vs RM-320: 64 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 64
power.dll literals into own data: 0xc80030c0x1(+0x0) 0xc80030c8x1(+0x8) 0xc80030d0x1(+0x10) 0xc80030d4x1(+0x14) 0xc80030d8x1(+0x18) 0xc80030dcx1(+0x1c) 0xc80030e4x1(+0x24) 0xc80030ecx1(+0x2c) 0xc80030f4x1(+0x34) 0xc8003170x1(+0xb0) 0xc80031ecx1(+0x12c)
RM-320 global offset +0x8 -> here 0xc80030c8; hint offset +0x2c -> here 0xc80030ec

==== rm160-20.2.011.rom
power.dll code 0x8009ab88..0x800a6188 dataBssLin 0xc80030c0 dataSize 0x34 bss 0x174 total 0x1a8
relock hits 0x8009ae5c in power.dll: true offset 0x2d4 (RM-320 0x2d4)
DSP setter (11 exact words) hits 0x8009f48c,0x8009f504 in power.dll: true,true offset 0x4904,0x497c (RM-320 0x4904)
mult reader hits 0x800a4434 offset 0x98ac (RM-320 0x98ac)
dvfs body hits 0x8013e7b0 in dvfs ldd: true offset 0x2e8 (RM-320 0x2e8)
  relock 0x50 words vs RM-320: 80 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 80
  relock first 8 words: 0xe92d40fe 0xe59f1128 0xe59f5128 0xe3a03000 0xee073f9a 0xe5914070 0xe3844048 0xe5814070
  dsp setter 24 words vs RM-320: 24 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 24
  dsp setter 24 words vs RM-320: 15 exact + 2 PC-relative (same opcode, other offset) + 7 real differences of 24 [13:0xe1a02001/0xe1a02401 15:0xe3a0101f/0xe3a01c1f 19:0xe3510001/0xe3510000 20:0x13510002/0xe59f029c 21:0x13e00005/0x3a02000 22:0xe92d4010/0x3a01080]
  reader 40 words vs RM-320: 40 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 40
  reader ldr @+0x1c -> pool 0x800a45dc = 0xc6008540
  body + dispatcher 0x40 words vs RM-320: 64 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 64
power.dll literals into own data: 0xc80030c0x1(+0x0) 0xc80030c8x1(+0x8) 0xc80030d0x1(+0x10) 0xc80030d4x1(+0x14) 0xc80030d8x1(+0x18) 0xc80030dcx1(+0x1c) 0xc80030e4x1(+0x24) 0xc80030ecx1(+0x2c) 0xc80030f4x1(+0x34) 0xc8003170x1(+0xb0) 0xc80031ecx1(+0x12c)
RM-320 global offset +0x8 -> here 0xc80030c8; hint offset +0x2c -> here 0xc80030ec

==== rm160-35.2.001.rom
power.dll code 0x8009ab88..0x800a6188 dataBssLin 0xc80030c0 dataSize 0x34 bss 0x174 total 0x1a8
relock hits 0x8009ae5c in power.dll: true offset 0x2d4 (RM-320 0x2d4)
DSP setter (11 exact words) hits 0x8009f48c,0x8009f504 in power.dll: true,true offset 0x4904,0x497c (RM-320 0x4904)
mult reader hits 0x800a4434 offset 0x98ac (RM-320 0x98ac)
dvfs body hits 0x8013e7b0 in dvfs ldd: true offset 0x2e8 (RM-320 0x2e8)
  relock 0x50 words vs RM-320: 80 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 80
  relock first 8 words: 0xe92d40fe 0xe59f1128 0xe59f5128 0xe3a03000 0xee073f9a 0xe5914070 0xe3844048 0xe5814070
  dsp setter 24 words vs RM-320: 24 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 24
  dsp setter 24 words vs RM-320: 15 exact + 2 PC-relative (same opcode, other offset) + 7 real differences of 24 [13:0xe1a02001/0xe1a02401 15:0xe3a0101f/0xe3a01c1f 19:0xe3510001/0xe3510000 20:0x13510002/0xe59f029c 21:0x13e00005/0x3a02000 22:0xe92d4010/0x3a01080]
  reader 40 words vs RM-320: 40 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 40
  reader ldr @+0x1c -> pool 0x800a45dc = 0xc6008540
  body + dispatcher 0x40 words vs RM-320: 64 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 64
power.dll literals into own data: 0xc80030c0x1(+0x0) 0xc80030c8x1(+0x8) 0xc80030d0x1(+0x10) 0xc80030d4x1(+0x14) 0xc80030d8x1(+0x18) 0xc80030dcx1(+0x1c) 0xc80030e4x1(+0x24) 0xc80030ecx1(+0x2c) 0xc80030f4x1(+0x34) 0xc8003170x1(+0xb0) 0xc80031ecx1(+0x12c)
RM-320 global offset +0x8 -> here 0xc80030c8; hint offset +0x2c -> here 0xc80030ec

==== rm160-10.2.006.rom
power.dll code 0x80047ef8..0x80053218 dataBssLin 0xc8004150 dataSize 0x34 bss 0x114 total 0x148
relock hits 0x800481c4 in power.dll: true offset 0x2cc (RM-320 0x2d4)
DSP setter (11 exact words) hits 0x8004c75c,0x8004c7d4 in power.dll: true,true offset 0x4864,0x48dc (RM-320 0x4904)
mult reader hits 0x800516c0 offset 0x97c8 (RM-320 0x98ac)
  relock 0x50 words vs RM-320: 80 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 80
  relock first 8 words: 0xe92d40fe 0xe59f1128 0xe59f5128 0xe3a03000 0xee073f9a 0xe5914070 0xe3844048 0xe5814070
  dsp setter 24 words vs RM-320: 23 exact + 1 PC-relative (same opcode, other offset) + 0 real differences of 24
  dsp setter 24 words vs RM-320: 15 exact + 2 PC-relative (same opcode, other offset) + 7 real differences of 24 [13:0xe1a02001/0xe1a02401 15:0xe3a0101f/0xe3a01c1f 19:0xe3510001/0xe3510000 20:0x13510002/0xe59f029c 21:0x13e00005/0x3a02000 22:0xe92d4010/0x3a01080]
  reader 40 words vs RM-320: 40 exact + 0 PC-relative (same opcode, other offset) + 0 real differences of 40
  reader ldr @+0x1c -> pool 0x80051868 = 0xc6008540
power.dll literals into own data: 0xc8004150x1(+0x0) 0xc8004158x1(+0x8) 0xc8004160x1(+0x10) 0xc8004164x1(+0x14) 0xc8004168x1(+0x18) 0xc800416cx1(+0x1c) 0xc8004174x1(+0x24) 0xc800417cx1(+0x2c) 0xc8004184x1(+0x34) 0xc80041e0x1(+0x90) 0xc800423cx1(+0xec)
RM-320 global offset +0x8 -> here 0xc8004158; hint offset +0x2c -> here 0xc800417c
```

## 4. Whole-module comparison and Usbc DoControl

```
== rm320 vs rm421-35.2.002 [power\.dll$]
A code 0x800a7c98 size 0xb600  B code 0x800a7c98 size 0xb600  delta 0x0
words 11648: exact 11643, absolute-address literals shifted by delta 0, other 5
  +0xaa60 (A 0x800b26f8)  0x766f4e3d -> 0x7270413d
  +0xaa64 (A 0x800b26fc)  0x20362020 -> 0x20333120
== rm320 vs rm421-1.2.011 [power\.dll$]
A code 0x800a7c98 size 0xb600  B code 0x8009fb88 size 0xb600  delta -0x8110
words 11648: exact 11051, absolute-address literals shifted by delta 458, other 139
  +0x1ac0 (A 0x800a9758)  0x64c9 -> 0x64ed
  +0x9d0c (A 0x800b19a4)  0x8008de14 -> 0x80085d68
== rm320 vs rm160-20.2.011 [power\.dll$]
A code 0x800a7c98 size 0xb600  B code 0x8009ab88 size 0xb600  delta -0xd110
words 11648: exact 11052, absolute-address literals shifted by delta 458, other 138
  +0x1ac0 (A 0x800a9758)  0x64c9 -> 0x64ed
  +0x9d0c (A 0x800b19a4)  0x8008de14 -> 0x80080d68
== rm160-20.2.011 vs rm160-35.2.001 [power\.dll$]
A code 0x8009ab88 size 0xb600  B code 0x8009ab88 size 0xb600  delta 0x0
words 11648: exact 11648, absolute-address literals shifted by delta 0, other 0
== rm160-20.2.011 vs rm160-35.2.001 [dvfs_interface\.ldd$]
A code 0x8013e4c8 size 0xa24  B code 0x8013e4c8 size 0xa24  delta 0x0
words 649: exact 649, absolute-address literals shifted by delta 0, other 0
== rm160-20.2.011 vs rm160-35.2.001 [eusbc\.ldd$]
A code 0x80100018 size 0x4230  B code 0x80100018 size 0x4230  delta 0x0
words 4236: exact 4236, absolute-address literals shifted by delta 0, other 0
== usbc DoControl, 128 words from the prologue vs RM-320 0x8010bd04
rm421-1.2.011.rom @0x80103aa4: 128 words from DoControl prologue: 128 exact, 0 PC-relative, 0 other 
rm421-20.2.005.rom @0x80103bc4: 128 words from DoControl prologue: 128 exact, 0 PC-relative, 0 other 
rm421-35.2.002.rom @0x8010bd04: 128 words from DoControl prologue: 128 exact, 0 PC-relative, 0 other 
rm160-20.2.011.rom @0x80101c84: 128 words from DoControl prologue: 128 exact, 0 PC-relative, 0 other 
rm160-10.2.006.rom @0x8008a270: 128 words from DoControl prologue: 90 exact, 38 PC-relative, 0 other 
```

## Script: namscan.js

```js
// namscan.js - offline comparison of NAM XIP images against the RM-320 reference.
// Usage: node namscan.js <ref.rom> <img.rom> [...]
// Everything is read from the images; nothing is assumed about linear mapping:
// it is checked per module by TRomEntry.iAddressLin vs TRomImageHeader.iCodeAddress.
const fs = require('fs');
const BASE = 0x80000000;

function u32(b, o) { return (o >= 0 && o + 4 <= b.length) ? b.readUInt32LE(o) : null; }
function hex(x) { return x === null || x === undefined ? 'null' : '0x' + (x >>> 0).toString(16); }

// Walk the ROM file system from TRomHeader.iRomRootDirectoryList.
function walkRom(b) {
  const files = [];
  const romBase = u32(b, 0x8c), romSize = u32(b, 0x90), rootList = u32(b, 0x94);
  const seen = new Set();
  function dir(addr, path, depth) {
    if (depth > 8 || seen.has(addr)) return;
    seen.add(addr);
    const o = addr - BASE;
    const size = u32(b, o);
    if (size === null || size <= 0 || size > 0x100000) return;
    let p = o + 4;
    const end = o + 4 + size;
    while (p < end && p + 10 <= b.length) {
      const esz = u32(b, p), eaddr = u32(b, p + 4), att = b[p + 8], nlen = b[p + 9];
      let name = '';
      for (let i = 0; i < nlen; i++) name += String.fromCharCode(b.readUInt16LE(p + 10 + 2 * i));
      const full = path + '\\' + name;
      if (att & 0x10) dir(eaddr, full, depth + 1);
      else files.push({ name: full, size: esz, addr: eaddr });
      p += 10 + 2 * nlen;
      p = (p + 3) & ~3;
    }
  }
  const nroots = u32(b, rootList - BASE);
  for (let i = 0; i < nroots && i < 8; i++) {
    const hw = u32(b, rootList - BASE + 4 + 8 * i), a = u32(b, rootList - BASE + 8 + 8 * i);
    dir(a, '', 0);
  }
  return { romBase, romSize, rootList, files };
}

function modInfo(b, f) {
  const o = f.addr - BASE;
  return {
    name: f.name, hdr: f.addr,
    uid3: u32(b, o + 8), entry: u32(b, o + 16), code: u32(b, o + 20),
    data: u32(b, o + 24), codeSize: u32(b, o + 28), textSize: u32(b, o + 32),
  };
}

function findMod(rom, b, re) {
  return rom.files.filter(f => re.test(f.name)).map(f => modInfo(b, f));
}

// Signature search: pat is a list of [value, mask].
function search(b, pat, lo, hi) {
  const hits = [];
  lo = Math.max(0, lo || 0); hi = Math.min(b.length - 4 * pat.length, hi || b.length);
  for (let o = lo & ~3; o < hi; o += 4) {
    let ok = true;
    for (let i = 0; i < pat.length; i++) {
      if (((b.readUInt32LE(o + 4 * i) & pat[i][1]) >>> 0) !== pat[i][0]) { ok = false; break; }
    }
    if (ok) hits.push(BASE + o);
  }
  return hits;
}
const X = v => [v >>> 0, 0xffffffff];
const DVFS_BODY = [X(0xe92d4010), X(0xe3a00001), [0xeb000000, 0xff000000], [0xe59f1000, 0xfffff000], X(0xe8bd4010), [0xea000000, 0xff000000]];
const USBC_PRO = [X(0xe92d4ff0), X(0xe24dd044), X(0xe1a04000), X(0xe1a06002)];
const RELOCK = [X(0xe92d40fe), [0xe59f1000, 0xfffff000], [0xe59f5000, 0xfffff000], X(0xe3a03000), X(0xee073f9a), X(0xe5914070), X(0xe3844048), X(0xe5814070)];

// Compare n words at ra in ref vs ia in img. Returns {eq, diff, list}.
function cmpWin(rb, ra, ib, ia, n) {
  let eq = 0; const diff = [];
  for (let i = 0; i < n; i++) {
    const r = u32(rb, ra - BASE + 4 * i), m = u32(ib, ia - BASE + 4 * i);
    if (r === m && r !== null) eq++; else diff.push([i, hex(r), hex(m)]);
  }
  return { eq, n, diff };
}

function cmpBytes(rb, ra, ib, ia, n) {
  const r = rb.slice(ra - BASE, ra - BASE + n), m = ib.slice(ia - BASE, ia - BASE + n);
  if (r.length !== n || m.length !== n) return 'out of image';
  let d = 0, first = -1;
  for (let i = 0; i < n; i++) if (r[i] !== m[i]) { d++; if (first < 0) first = i; }
  return d === 0 ? 'IDENTICAL (' + n + ' bytes)' : d + ' bytes differ of ' + n + ', first at +' + hex(first);
}

const ref = fs.readFileSync(process.argv[2]);
const refRom = walkRom(ref);
const refMods = {};
for (const [k, re] of [['power', /\\power\.dll$/i], ['dvfs', /dvfs_interface\.ldd$/i], ['usbc', /usbc\.ldd$/i], ['ekern', /\\ekern\.exe$/i]])
  refMods[k] = findMod(refRom, ref, re)[0];

// Reference addresses (RM-320 profile).
const R = { body: 0x80148410, relock: 0x800a7f6c, dsp: 0x800ac59c, reader: 0x800b1544, pool: 0x800b16ec, usbc: 0x8010bd04 };

for (const path of process.argv.slice(3)) {
  const b = fs.readFileSync(path);
  console.log('\n==== ' + path + '  size ' + hex(b.length));
  console.log('reset vector ' + hex(u32(b, 0)));
  const rom = walkRom(b);
  console.log('iRomBase ' + hex(rom.romBase) + ' iRomSize ' + hex(rom.romSize) + ' rootList ' + hex(rom.rootList) + ' files ' + rom.files.length);
  const dvfsNames = rom.files.filter(f => /dvfs/i.test(f.name)).map(f => f.name);
  console.log('dvfs files in ROM dir: ' + (dvfsNames.join(', ') || 'NONE'));
  const strHits = [];
  for (let p = b.indexOf('dvfs_interface'); p >= 0 && strHits.length < 6; p = b.indexOf('dvfs_interface', p + 1)) strHits.push(hex(BASE + p));
  console.log('ascii "dvfs_interface": ' + (strHits.join(' ') || 'none'));

  for (const [k, re] of [['power', /\\power\.dll$/i], ['dvfs', /dvfs_interface\.ldd$/i], ['usbc', /usbc\.ldd$/i], ['ekern', /\\ekern\.exe$/i]]) {
    const ms = findMod(rom, b, re);
    if (!ms.length) { console.log(k + ': not in ROM dir'); continue; }
    for (const m of ms) {
      const lin = (m.code === m.hdr + 120);
      console.log(k + ': ' + m.name + ' hdr ' + hex(m.hdr) + ' code ' + hex(m.code) + ' codeSize ' + hex(m.codeSize) + ' text ' + hex(m.textSize) +
        ' entry ' + hex(m.entry) + (lin ? ' [code = hdr+120: linear]' : ' [NOT hdr+120]'));
      const rm = refMods[k];
      if (rm) {
        console.log('   vs RM-320 ' + rm.name + ' code ' + hex(rm.code) + ' codeSize ' + hex(rm.codeSize) + ' -> same address: ' + (rm.code === m.code) +
          ', same size: ' + (rm.codeSize === m.codeSize));
        if (rm.codeSize === m.codeSize) console.log('   whole code section: ' + cmpBytes(ref, rm.code, b, m.code, m.codeSize));
      }
    }
  }

  const sig = (nm, pat) => {
    const h = search(b, pat);
    console.log(nm + ' signature hits (' + h.length + '): ' + h.slice(0, 8).map(hex).join(' '));
    return h;
  };
  const bh = sig('dvfs DoControl(1) body', DVFS_BODY);
  const uh = sig('Usbc DoControl prologue', USBC_PRO);
  const rh = sig('relock routine', RELOCK);

  // Fixed reference addresses: compare windows.
  for (const [nm, a, n] of [['relock', R.relock, 80], ['dsp setter', R.dsp, 24], ['mult reader', R.reader, 24], ['pool', R.pool, 1], ['dvfs body', R.body, 6], ['usbc DoControl', R.usbc, 16]]) {
    const c = cmpWin(ref, a, b, a, n);
    console.log('@RM-320 ' + nm + ' ' + hex(a) + ': ' + c.eq + '/' + c.n + ' words equal' + (c.diff.length ? '  first diffs ' + JSON.stringify(c.diff.slice(0, 3)) : ''));
  }
  // dvfs body found by signature: compare 6 words and dispatcher continuation against RM-320.
  for (const a of bh) {
    const c = cmpWin(ref, R.body, b, a, 6);
    console.log('body@' + hex(a) + ' vs RM-320 body: ' + c.eq + '/6 exact; words ' + [0,1,2,3,4,5].map(i => hex(u32(b, a - BASE + 4 * i))).join(' '));
  }
}
```

## Script: namdetail.js

```js
// namdetail.js - per-build derivation details. Usage: node namdetail.js <ref.rom> <img.rom>...
const fs = require('fs');
const BASE = 0x80000000;
const u32 = (b, o) => (o >= 0 && o + 4 <= b.length) ? b.readUInt32LE(o) : null;
const hex = x => x === null || x === undefined ? 'null' : '0x' + (x >>> 0).toString(16);

function walk(b) {
  const files = [];
  const seen = new Set();
  function dir(addr, path, d) {
    if (d > 8 || seen.has(addr)) return; seen.add(addr);
    const o = addr - BASE, size = u32(b, o);
    if (!size || size > 0x100000) return;
    let p = o + 4; const end = o + 4 + size;
    while (p < end) {
      const esz = u32(b, p), ea = u32(b, p + 4), att = b[p + 8], nl = b[p + 9];
      let n = ''; for (let i = 0; i < nl; i++) n += String.fromCharCode(b.readUInt16LE(p + 10 + 2 * i));
      if (att & 0x10) dir(ea, path + '\\' + n, d + 1); else files.push({ name: path + '\\' + n, addr: ea });
      p = (p + 10 + 2 * nl + 3) & ~3;
    }
  }
  const rl = u32(b, 0x94) - BASE, nr = u32(b, rl);
  for (let i = 0; i < nr && i < 8; i++) dir(u32(b, rl + 8 + 8 * i), '', 0);
  return files;
}
function mod(b, files, re) {
  const f = files.find(f => re.test(f.name)); if (!f) return null;
  const o = f.addr - BASE;
  return { hdr: f.addr, code: u32(b, o + 20), data: u32(b, o + 24), codeSize: u32(b, o + 28), dataSize: u32(b, o + 36),
           bssSize: u32(b, o + 40), dataBssLin: u32(b, o + 96), totalData: u32(b, o + 108) };
}
const X = v => [v >>> 0, 0xffffffff];
function search(b, pat, lo, hi) {
  const h = []; lo = lo - BASE; hi = Math.min(hi - BASE, b.length - 4 * pat.length);
  for (let o = lo; o < hi; o += 4) {
    let ok = true;
    for (let i = 0; i < pat.length; i++) if (((u32(b, o + 4 * i) & pat[i][1]) >>> 0) !== pat[i][0]) { ok = false; break; }
    if (ok) h.push(BASE + o);
  }
  return h;
}
// Is this ARM word PC-relative (ldr/str [pc,#], b/bl)? Such words legitimately differ when code moves.
function pcRel(w) {
  if (((w >>> 25) & 7) === 5) return 'branch';
  if ((w & 0x0c0f0000) === 0x040f0000) return 'ldr-pc';
  return null;
}
// Compare n words; report exact, differing-but-PC-relative-same-opcode, and real differences.
function cmp(rb, ra, ib, ia, n) {
  let exact = 0, rel = 0; const real = [];
  for (let i = 0; i < n; i++) {
    const r = u32(rb, ra - BASE + 4 * i), m = u32(ib, ia - BASE + 4 * i);
    if (r === m) exact++;
    else if (pcRel(r) && pcRel(m) && pcRel(r) === pcRel(m) &&
             (pcRel(r) === 'branch' ? (r >>> 24) === (m >>> 24) : (r & 0xfffff000) === (m & 0xfffff000))) rel++;
    else real.push(i + ':' + hex(r) + '/' + hex(m));
  }
  return exact + ' exact + ' + rel + ' PC-relative (same opcode, other offset) + ' + real.length + ' real differences of ' + n + (real.length ? ' [' + real.slice(0, 6).join(' ') + ']' : '');
}

const ref = fs.readFileSync(process.argv[2]); const rf = walk(ref);
const RP = mod(ref, rf, /\\power\.dll$/i), RD = mod(ref, rf, /dvfs_interface\.ldd$/i), RU = mod(ref, rf, /eusbc\.ldd$/i);
const R = { body: 0x80148410, relock: 0x800a7f6c, dsp: 0x800ac59c, reader: 0x800b1544, pool: 0x800b16ec, usbc: 0x8010bd04, global: 0xc80030c8, hint: 0xc80030ec };
console.log('RM-320 power.dll code ' + hex(RP.code) + ' data ' + hex(RP.data) + ' dataBssLin ' + hex(RP.dataBssLin) + ' dataSize ' + hex(RP.dataSize) + ' bss ' + hex(RP.bssSize) + ' total ' + hex(RP.totalData));
console.log('RM-320 offsets: relock-code ' + hex(R.relock - RP.code) + ' dsp-code ' + hex(R.dsp - RP.code) + ' reader-code ' + hex(R.reader - RP.code) +
            ' body-dvfscode ' + hex(R.body - RD.code) + ' usbc-eusbccode ' + hex(R.usbc - RU.code) + ' global-dataBssLin ' + hex(R.global - RP.dataBssLin) + ' hint-dataBssLin ' + hex(R.hint - RP.dataBssLin));
// Literals in RM-320 power.dll code that point into its own data/bss.
function dataLits(b, P) {
  const s = new Map();
  for (let a = P.code; a < P.code + P.codeSize; a += 4) {
    const w = u32(b, a - BASE);
    if (w >= P.dataBssLin && w < P.dataBssLin + P.totalData) s.set(w, (s.get(w) || 0) + 1);
  }
  return [...s.entries()].sort((x, y) => x[0] - y[0]);
}
console.log('RM-320 power.dll literals into own data: ' + dataLits(ref, RP).map(([w, c]) => hex(w) + 'x' + c).join(' '));

const DSP_SIG = []; for (let i = 0; i < 11; i++) DSP_SIG.push(X(u32(ref, R.dsp - BASE + 4 * i)));
const READER_SIG = [X(0xe92d4010), [0xe59f0000, 0xfffff000], [0xeb000000, 0xff000000], X(0xe3a01003), X(0xe1d10000), X(0x13a00000), X(0x18bd8010), [0xe59f0000, 0xfffff000], [0xeb000000, 0xff000000], X(0xe1a01000), X(0xe1a00300), X(0xe1b00ea0)];
const RELOCK = [X(0xe92d40fe), [0xe59f1000, 0xfffff000], [0xe59f5000, 0xfffff000], X(0xe3a03000), X(0xee073f9a), X(0xe5914070), X(0xe3844048), X(0xe5814070)];
const BODY = [X(0xe92d4010), X(0xe3a00001), [0xeb000000, 0xff000000], [0xe59f1000, 0xfffff000], X(0xe8bd4010), [0xea000000, 0xff000000]];

const imgs = {};
for (const path of process.argv.slice(3)) {
  const b = fs.readFileSync(path); imgs[path] = b;
  const f = walk(b);
  const P = mod(b, f, /\\power\.dll$/i), D = mod(b, f, /dvfs_interface\.ldd$/i), U = mod(b, f, /eusbc\.ldd$/i);
  console.log('\n==== ' + path);
  console.log('power.dll code ' + hex(P.code) + '..' + hex(P.code + P.codeSize) + ' dataBssLin ' + hex(P.dataBssLin) + ' dataSize ' + hex(P.dataSize) + ' bss ' + hex(P.bssSize) + ' total ' + hex(P.totalData));
  const relock = search(b, RELOCK, BASE, BASE + b.length);
  const dsp = search(b, DSP_SIG, BASE, BASE + b.length);
  const reader = search(b, READER_SIG, BASE, BASE + b.length);
  const body = D ? search(b, BODY, BASE, BASE + b.length) : [];
  const inP = a => a >= P.code && a < P.code + P.codeSize;
  console.log('relock hits ' + relock.map(hex) + ' in power.dll: ' + relock.map(inP) + ' offset ' + relock.map(a => hex(a - P.code)) + ' (RM-320 ' + hex(R.relock - RP.code) + ')');
  console.log('DSP setter (11 exact words) hits ' + dsp.map(hex) + ' in power.dll: ' + dsp.map(inP) + ' offset ' + dsp.map(a => hex(a - P.code)) + ' (RM-320 ' + hex(R.dsp - RP.code) + ')');
  console.log('mult reader hits ' + reader.map(hex) + ' offset ' + reader.map(a => hex(a - P.code)) + ' (RM-320 ' + hex(R.reader - RP.code) + ')');
  if (D) console.log('dvfs body hits ' + body.map(hex) + ' in dvfs ldd: ' + body.map(a => a >= D.code && a < D.code + D.codeSize) + ' offset ' + body.map(a => hex(a - D.code)) + ' (RM-320 ' + hex(R.body - RD.code) + ')');
  for (const a of relock) console.log('  relock 0x50 words vs RM-320: ' + cmp(ref, R.relock, b, a, 0x50));
  for (const a of relock) console.log('  relock first 8 words: ' + [0,1,2,3,4,5,6,7].map(i => hex(u32(b, a - BASE + 4 * i))).join(' '));
  for (const a of dsp) console.log('  dsp setter 24 words vs RM-320: ' + cmp(ref, R.dsp, b, a, 24));
  for (const a of reader) {
    console.log('  reader 40 words vs RM-320: ' + cmp(ref, R.reader, b, a, 40));
    // pool literal: ldr r0,[pc,#imm] at reader+0x1c
    const w = u32(b, a - BASE + 0x1c), lit = a + 0x1c + 8 + (w & 0xfff);
    console.log('  reader ldr @+0x1c -> pool ' + hex(lit) + ' = ' + hex(u32(b, lit - BASE)));
  }
  for (const a of body) console.log('  body + dispatcher 0x40 words vs RM-320: ' + cmp(ref, R.body, b, a, 0x40));
  const lits = dataLits(b, P);
  console.log('power.dll literals into own data: ' + lits.map(([w, c]) => hex(w) + 'x' + c + '(+' + hex(w - P.dataBssLin) + ')').join(' '));
  console.log('RM-320 global offset +' + hex(R.global - RP.dataBssLin) + ' -> here ' + hex(P.dataBssLin + (R.global - RP.dataBssLin)) +
              '; hint offset +' + hex(R.hint - RP.dataBssLin) + ' -> here ' + hex(P.dataBssLin + (R.hint - RP.dataBssLin)));
}
```

## Script: namdiff.js

```js
// namdiff.js - whole-module comparison with PC-relative tolerance.
// Usage: node namdiff.js <a.rom> <b.rom> <moduleRegex>
const fs = require('fs');
const BASE = 0x80000000;
const u32 = (b, o) => (o >= 0 && o + 4 <= b.length) ? b.readUInt32LE(o) : null;
const hex = x => '0x' + (x >>> 0).toString(16);
function walk(b) {
  const files = [], seen = new Set();
  function dir(addr, path, d) {
    if (d > 8 || seen.has(addr)) return; seen.add(addr);
    const o = addr - BASE, size = u32(b, o); if (!size || size > 0x100000) return;
    let p = o + 4; const end = o + 4 + size;
    while (p < end) {
      const ea = u32(b, p + 4), att = b[p + 8], nl = b[p + 9];
      let n = ''; for (let i = 0; i < nl; i++) n += String.fromCharCode(b.readUInt16LE(p + 10 + 2 * i));
      if (att & 0x10) dir(ea, path + '\\' + n, d + 1); else files.push({ name: path + '\\' + n, addr: ea });
      p = (p + 10 + 2 * nl + 3) & ~3;
    }
  }
  const rl = u32(b, 0x94) - BASE, nr = u32(b, rl);
  for (let i = 0; i < nr && i < 8; i++) dir(u32(b, rl + 8 + 8 * i), '', 0);
  return files;
}
function mod(b, re) {
  const f = walk(b).find(f => re.test(f.name)); const o = f.addr - BASE;
  return { code: u32(b, o + 20), codeSize: u32(b, o + 28), text: u32(b, o + 32) };
}
const [a, b] = [fs.readFileSync(process.argv[2]), fs.readFileSync(process.argv[3])];
const re = new RegExp(process.argv[4], 'i');
const A = mod(a, re), B = mod(b, re);
const delta = B.code - A.code;
console.log('A code ' + hex(A.code) + ' size ' + hex(A.codeSize) + '  B code ' + hex(B.code) + ' size ' + hex(B.codeSize) + '  delta ' + (delta < 0 ? '-' : '') + hex(Math.abs(delta)));
const n = Math.min(A.codeSize, B.codeSize) / 4;
let exact = 0; const diffs = [];
for (let i = 0; i < n; i++) {
  const x = u32(a, A.code - BASE + 4 * i), y = u32(b, B.code - BASE + 4 * i);
  if (x === y) { exact++; continue; }
  // A literal that is an address inside the image shifted by exactly delta is a relocation, not a code change.
  const isReloc = (y - x) >>> 0 === delta >>> 0 && x >= BASE && x < BASE + 0x10000000;
  diffs.push({ off: 4 * i, x, y, reloc: isReloc });
}
const reloc = diffs.filter(d => d.reloc).length;
console.log('words ' + n + ': exact ' + exact + ', absolute-address literals shifted by delta ' + reloc + ', other ' + (diffs.length - reloc));
for (const d of diffs.filter(d => !d.reloc).slice(0, 400)) console.log('  +' + hex(d.off) + ' (A ' + hex(A.code + d.off) + ')  ' + hex(d.x) + ' -> ' + hex(d.y));
```

## Script: usbcwin.js

```js
const fs=require('fs');const B=0x80000000;const u=(b,o)=>b.readUInt32LE(o);const h=x=>'0x'+(x>>>0).toString(16);
const ref=fs.readFileSync(process.argv[2]);const ra=parseInt(process.argv[3],16);
for(let k=4;k<process.argv.length;k+=2){const b=fs.readFileSync(process.argv[k]);const a=parseInt(process.argv[k+1],16);
let ex=0,rel=0,real=[];for(let i=0;i<128;i++){const x=u(ref,ra-B+4*i),y=u(b,a-B+4*i);if(x===y)ex++;else if((((x>>>25)&7)===5&&(x>>>24)===(y>>>24))||((x&0x0c0f0000)===0x040f0000&&(x&0xfffff000)===(y&0xfffff000)))rel++;else real.push(i+':'+h(x)+'/'+h(y));}
console.log(process.argv[k]+' @'+h(a)+': 128 words from DoControl prologue: '+ex+' exact, '+rel+' PC-relative, '+real.length+' other '+real.slice(0,8).join(' '));}
```
