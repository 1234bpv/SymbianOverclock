# Work Status and Recovery Point

Read alongside `findings.md` — technical conclusions are there, operational status is here.
Update after every stage: this is the single recovery point after losing context.

`WALKTHROUGH.md` (2026-08-08) — narrative breakdown of the entire journey for an outside reader:
a primer on hardware and Symbian, chronology with dead ends, methodological lessons, sources
of OMAP2420 documentation. The foundation for the future README upon publication.

`ROADMAP.md` (2026-08-08) — work plan prior to publication on GitHub and order of its
execution. Current first item — auto-apply of safe settings with a
60-second cancellation window, followed by a firmware gate and signature-based address search.

## Objective

Determine whether CPU overclocking is possible on Symbian 9.2 / OMAP 2420, and if so —
implement it. Reference device: Nokia N95 8GB (RM-320) — owned, unlocked for
homebrew (dev-unlocked), connected over local WiFi. Context and scope —
`docs/SCOPE.md`.

## Where We Are Now

### Version 2.3.0: autostart works on both phones (2026-09-15)

The overclock can now be applied after a restart without touching the phone, and
it is confirmed end to end on the N95 and on the E90. It is off after
installation and switching it on takes three proofs - the configuration has to
be live in the registers, pass three rounds of tests when it is saved, and then
survive a soak plus one more clean round before boot will use it. The applier is
a separate executable, `ocboot.exe`, with no user interface; deleting that one
file switches autostart off without removing the application. The design, and
the loop it has to make impossible, are in `docs/AUTOSTART.md`.

Two defects stood between the feature and a working phone, and both were silent.
The package would not install at all while `ocboot.exe` shared a secure
identifier with `ocgui.exe`, because an identifier has to be unique across the
device (findings 49). Then the startup entry registered and never ran, because
the package UID, the name of the file installed into the startup import
directory and the SID of the launched executable all have to be one number
(findings 50). `ocboot.exe` therefore carries `0xE0001011` and `ocgui.exe` moved
to `0xE0001014`, keeping `UID3 0xE0001011` so the menu entry is unchanged.

A third defect showed up only on the E90: our own second loop detector read the
soak as a boot session that had died and refused the configuration twice out of
two. It now counts only confirmations that came from a boot. Along with it the
application learned to report the paths that used to give up in silence - engine
not ready, apply failed, no profile - which is exactly what a phone whose unlock
patches are not applied at boot does every time.

### Version 2.0.1: rollback with live words, working test profile (2026-09-11)

The final pre-release review found five discrepancies, and all five
were confirmed upon reading the code. The main one: the TEST PROFILE string did not work at all
because its handler was placed behind the engine readiness check, and on an unknown device
the engine is not ready precisely because the profile does not exist yet. The rest — residual text
UNKNOWN DEVICE after a successful connection, false NON-STOCK at start on test
profiles (comparison was made against profile fields, which are zero there), lack of boundaries in
the global search, and an outdated paragraph in PORTING.md.

Our own check found a worse defect that was not in the review. The patch rollback
wrote back a constant from RM-320, in which three words out of six are branch
offsets of a specific build. On a different build, such a rollback would leave the driver
corrupted until reboot, silently at that. Now stock words are captured from
live ROM before the first write, and that same operation also provided device identification:
bytes at the patch address are verified with a mask prior to any write to the kernel, and for
a test profile, a mismatch means rejection. Verified on N95 (findings §42).

**Localization expanded to 28 languages**, and what was missing was added: mapping
device language regional variants to the base language. Austrian German, Belgian
French, Brazilian Portuguese, and similar previously silently gave English,
even though a translation existed. The idea was borrowed from PS1 emulator i18n.

### Version 2.0.0: test profiles, 15 languages, autostart scheme (2026-09-11)

**Test profiles.** Nokia N82 (RM-313) and N95 classic (RM-159) added as
TEST profiles: never picked automatically, selectable only manually and after two
warnings. What could be proven from images was proven firsthand
byte by byte (findings §41), and what could not be proven — the power-down global address —
the engine searches for on the device with SRAM copy verification. The search mechanism was verified on
both working devices against known answers: `c80030c8` on N95 and
`c8003068` on E90.

**Localization into 15 languages** with an English fallback for every
individual string. This was what the emulator approach lacked, where each table
must be complete. 21 strings that the user reads as
coherent text are widely translated; service formats with register names and help remain
in English and Ukrainian deliberately.

**Autostart** is described in `docs/AUTOSTART.md` as a proposal and is not
implemented. Main differences from ROADMAP §0: a step down instead of failure
after three attempts, and a second loop detector based on the wall clock, independent
of whether the marker managed to land in flash. Two rules are designated as hard: never
touch the bus automatically, and verify the Mass Storage recovery path
on the device BEFORE release.


### Preparation for release: localization, help, firmware gate (2026-09-11)

Application 1.9.0. Three changes, of which the main one is the last.

**Localization.** English and Ukrainian, language is taken from device settings
once at startup. Translation source is `src/strings.json`, tables are generated by
`node tools/gen_strings.js` into `src/ocstr.cpp` as escapes of the form backslash-x-XXXX.
Done this way deliberately: there is no reason to rely on old GCCE correctly converting
UTF-8 inside L"..." to UTF-16, and an error would be silent and
surface only on the device screen. Verified otherwise than by build: UTF-16
of Ukrainian strings was found in the finished binary.

**Built-in manual.** The `HELP` item opens full-screen help,
scrolling up/down, exit by any other key. The text resides in the same
translation source, so the manual is on the device in the native language and without a network.

**Firmware gate — and what it found right away.** Profile addresses are derived from
a SPECIFIC build, not from the model, so a match of the machine uid guarantees nothing: on
another build the exact same patch would land on the wrong code. Now the engine reads the device
firmware version and does not touch the kernel without a match; an unreadable version is also
a rejection. The very first run showed that N95 8GB runs on **35.0.001**, whereas the profile
was named 31.0.015 all along. Now the profile contains both builds with different grounds:
35.0.001 is proven on device, 31.0.015 is the image from which addresses were derived, and their
match in itself is proof that code did not shift between these builds.

**Porting instead of made-up profiles.** An unknown device now prints
the machine uid and firmware version — exactly what a profile begins with; the rest is in
`docs/PORTING.md`. Profiles for N82, RM-159, N93, and N93i DO NOT exist and will not until
hardware confirms them. Offline analyses in `out/analysis/` are real, but
subagents sent for machine uids and stock layouts returned numbers without
a trace of evidence (for example `0x20002D85` against RM-320 `0x20002D84` — that is, adjacent
by template), and the second honestly admitted lacking the means to run objdump and
extrapolated by offset. Such data does not go into a profile.

Release documents: `README.md`, `LICENSE` (MIT — default choice, worth
confirming), `CHANGELOG.md`, `AGENTS.md`. The `release/v1/` directory is a deliberate
snapshot of the proven version 1.0, not a forgotten duplicate; it has its own README.

**Localization boundary is deliberate.** Strings facing the user go through
the table: menu row captions, warnings, test verdicts, hints, and all
help. Technical log lines with register names and hex
values remain English: translating a format full of CLKSEL fields
would detract from clarity rather than add to it.

**Smoke test.** 1.9.0 uploaded to N95 and launched remotely — daemon returned
`OK proc 1`, meaning the process is created and the binary on the device is working.

**Autostart never existed and still does not**, and for release this is correct: the application
does not apply anything itself, and `.pkg` does not put an entry into the startup list. The attempt
marker protocol from ROADMAP §0 remains unimplemented and unneeded until
this is changed.

**Subagent review found a real critical defect, and this is the main gain of the day.**
On E90 `iDspSetter` equals zero — the address for this device was not measured — while
`ApplyDspDiv` did not check it. By pressing OK on the `DSPDIV` item, the user
would execute a branch to a null address in kernel mode. Meanwhile, the menu item was
always accessible, which is precisely what is forbidden by the rule "nothing that can
hang by default". Now a profile without a setter receives a rejection prior to
any action, and the application explains the reason in words.

A gate bypass was found in the same place: `SelfTestLevel` at level 2 applied a patch without
a firmware check, and the default profile is the reference one, so `ocprobe
selftest 2` on someone else's device would write the RM-320 address into someone else's kernel. Closed by the
same gate.

And third, worth remembering separately: the application still told the user that
raising the bus killed the device four times out of four. This was disproven by our own measurement
(§39), and the wording was fixed in findings, but the fix did not reach the texts of the application itself.
Now it has reached them — into help, warnings, and README.

**Second review: five out of six items closed, the sixth seen through to completion.**
The subagent verified each fix against the code and confirmed that rejection due to
null `iDspSetter` stands before any action and there is no way to bypass it (the only path to
`WriteDspBlob` is via `ApplyDspDiv`), that the gate in `SelfTestLevel` is in place, that
README is aligned with findings, and that `out/analysis/` is now in git. As the sixth item it
named strings that were still bypassing localization; all of them were completed. Only
the technical layer remained in English — register names, hex dumps, and measurement
units — and this is deliberate.

**Blocker closed: gate verified on E90.** Pavlo brought up the device, build was uploaded,
`patcher engine` completed to the end: in log `profile: RA-6 400.34.93` and
`engine ready: SRAM @cb102000, stock mult 86`, then all registers read and
`restore check ... -> 1`, meaning kernel code was restored to its place. Thus the E90 version string
indeed contains "400.34.93", and the profile passes the gate. The application on E90
also starts (`OK proc 1`). The device at that sits on stock
`CLKSEL1_CORE=04360626`, meaning after reboot the bus returned by itself.

No open blockers before release. Things remaining block nothing:
MIT license is chosen by default and is worth confirming; build scripts and
`.pkg` carry absolute paths of the author's machine, and this is stated directly in README;
profiles for the remaining OMAP 2420 devices await live hardware (`docs/PORTING.md`).


### E90 at 503 MHz; ~500 MHz ceiling confirmed on second device (2026-09-11)

Multiplier 131 (two steps above the old slider ceiling of 129), ALU 37 Mitr/s,
memory 534 MB/s. The limit raised to 255 was needed in practice immediately. Together with
491 MHz on N95, this gives the same core limit on both devices — around 500 MHz without
voltage increase.

Separately worthy of attention is the comparison with C5: 38.2 Mitr/s at 600 MHz versus 37 Mitr/s at
503 MHz, meaning the overclocked E90 virtually caught up with a noticeably newer device
(findings §40.1).


### ocgui uploaded to C5-00.2 as a measurement tool (2026-09-11)

C5 is not OMAP 2420, there is no profile for it, and the application on an unknown device
honestly refuses to touch the kernel — but CPU and memory measurements work even without
the patcher, and it is precisely for them that it is needed there. Three files uploaded
over Bluetooth to COM12: `C:\sys\bin\ocgui.exe` with an entry in `C:\sys\hash`,
`C:\resource\apps\ocgui.rsc`, and
`C:\private\10003a3f\import\apps\ocgui_reg.rsc`. Without the latter two,
the device would not show the application in the menu, because it was never installed there
as a package. Launch by path verified: daemon returned `OK proc 1`.

Common conclusions regarding overclocking are moved a level higher, into
`SymbianProjects/CLAUDE.md` — they are needed by both `PS1Emulator` and
`Hexen2Update`, because the main one among them is not about frequency, but about ARM1136 in 3D
being bottlenecked by memory.


### RAISING THE BUS WORKS, and in 3D it yields more than core overclocking (2026-09-11)

Pavlo disabled `BUS GUARD` and raised the E90 bus by 50% (`l3div` 6 -> 4, that is
110.08 -> 165.12 MHz). The device survived, and the gain is the largest of everything the project
has measured: 16.33 -> 23.00 fps on the large screen with memory at 339 -> 463 MB/s, and up to
24.55 fps / 509 MB/s with further bus overclocking.

This corrects §38.2: raising the bus CAN be done, the conclusion "can lower, cannot raise"
was incorrect. What remains true is that all four deaths occurred on RETURNING
the bus from an already lowered state, and why exactly — is still unknown.

The practical conclusion shifts priorities. ARM1136 in 3D is almost entirely bottlenecked by
memory: the core during these measurements was not moved at all. Multiplier hits silicon limits
around 500 MHz (§38.9), while the bus just gave +41% in 3D. Going forward, it makes sense to measure specifically
the bus.


### Bus safeguard became `BUS GUARD` toggle (2026-09-11)

Showing the clamp was not enough: the prohibition remained absolute, whereas Pavlo needed
the ability to try raising the bus on his own device. Now this is a separate GUI row
`BUS GUARD: ON / OFF` under `BUSDIV` and the `guard=0` flag in the probe; enabled by
default. Disabling removes both the clamp in `ApplyMultAndCore` and the rejection in
`RestoreStock` (findings §38.8).

The truth about the raise itself has not changed: four deaths out of four, on two devices,
by both paths, and not a single successful raise had occurred so far. The cost of failure is
an immediate reboot, because nothing is written to flash. A useful target on N95 at multiplier
129 is `l3div=6` (stock 165 MHz bus), not `l3div=4` (247 MHz, 1.5x stock).

The limit of ARM1136 without voltage increase is around 500 MHz (§38.9), so raising the slider
ceiling to 255 by itself will not add frequency: practical headroom remained in the bus,
not in the multiplier.

GUI 1.8.0 uploaded to both devices.


### Multiplier ceiling raised to 255; bus clamp now visible in UI (2026-09-10)

The slider limit of 129 was ours, not hardware-imposed, and Pavlo asked to raise it to
255. Done; nothing above 129 has been verified, and both the GUI and the probe state
this in a separate line (findings §38.7).

At the same time, the complaint "dividers do not work" has been resolved. The reason is neither hardware nor
a bug: both devices after the experiments of §38.2 remained at `l3div=8` and were not
rebooted, so the safeguard of §38.3 silently clamped every attempt to lower
the divider, that is, raise the bus. Moreover, on RM-320 when transitioning from multiplier 111 to
112, auto-selection itself switches to `l3div=6`, because the bus would otherwise exceed
the ceiling of 213120 kHz. Behavior was not changed — it is backed by four deaths —
but now it is visible: the `BUSDIV` line prints `/4 CLAMPED /8` and the true frequency,
while `armed` separately states `bus stays /8 clamped` (findings §38.5). The clamp
is cleared only by a reboot.

Display divider on E90 resolved the problem with the external screen — confirmed
by Pavlo, findings §38.6.

Next step — experiment with multiplier above 129 on N95 with raised `l3div`,
in steps of one, stopping at the first non-zero relock status.

### CLOSED: bus divider works; returning the bus back is forbidden (2026-09-09)

CORE dividers were not being written due to an application defect rather than hardware: the sole
write in `ApplyRampWithCore` was placed inside the `while (cur != aTarget)` loop, so
changing dividers alone with an unchanged multiplier wrote nothing (findings §38.1).
After the fix, the bus divider was measured with the memory benchmark — the very tool that
Pavlo called the only honest one — on both devices:

```
E90   l3div 6 -> 8:  CPU  0.0%   MEM -21.0%
N95   l3div 4 -> 8:  CPU +0.6%   MEM -44.9%
```

Domains are completely separated: core frequency does not move, memory drops exactly
according to the divider. The layout holds under load, zero corrupted words in
SDRAM.

However, RETURNING the bus back kills the device: four deaths out of four, by both
paths, on two devices. The death of E90 disproved the explanation in §26.15 via
the DVFS governor — on E90 there is no governor at all. Therefore `RestoreStock` now
refuses to do this, leaves the lowered bus (state measured as safe), and
sets `RestoreNeedsReboot()`; the probe and GUI say to reboot, and a reboot
guarantees restoring stock clocking. The safeguard lives in the engine: the probe previously
bypassed it and called `ReassertDividers` directly with RM-320 constants.

Verified on device: N95 completed `patcher clocks l3=8 sec=10` to the end and
stayed alive — for the first time on this operation. Breakdown — `findings.md` §38.2.

**Methodological rule.** Divider operability is verified by MEASUREMENT (memory
benchmark), not by a log line; register reading complements this, not replaces it.

**N95 takes 491 MHz when the bus is lower than stock.** The hypothesis about the bus as
a limiter was confirmed: at `l3div=8` the device completed the ramp 86 → 129 without any
failure, endured 15 s of load, zero corrupted words in SDRAM, `CPU +50.1%`,
`MEM +50.5%`. Until then, the ceiling was 111 (426 MHz). Breakdown and caveat about
the governor — `findings.md` §38.4.

**The safeguard lives at the write point.** The clamping of field `l3` is located in
`ApplyMultAndCore`, through which any layout write passes, not just in
`RestoreStock`: otherwise the same fatal bus raise would be done with a regular
`APPLY` from the GUI. The write is not rejected, but clamped to the live value, so
overclocking after a bus change remains possible. The probe and GUI compare the live register
with `LastCoreWritten()`, not with the requested one, otherwise a deliberate clamp would be read as
hardware failure (§38.3).

**The display divider is also written.** On E90 `dssdiv` 6 → 8 and back are accepted,
readback matches. Blinking of the external screen, therefore, pertains to panel behavior,
not to register writing.

**Next step.** Narrow the N95 threshold: will it take multiplier 129 at `l3div=6`,
that is, at a 165 MHz bus, as in E90. A reboot is needed because raising the bus at runtime
is prohibited by the safeguard.

Both devices after the runs remained with a lowered bus (`l3div=8`) — this is
intentional and safe, a reboot will restore stock.

The GUI overclocking management application is installed and working on both devices; it picks
the profile itself via HAL EMachineUid (findings §37).

Signed package `group/SymbianOverclock_sha1.sis` (version 1.8.0) was built with this
build: `tools/build_sis.cmd`. Method of package verification is not the script report, but
separate control: `signsis -o` shows the publisher, and unpacked `dumpsis -x`
must contain specifically the new `ocgui.exe`, and SHA-1 of the unpacked file0 must match
the freshly built binary (done for 1.6.0, not yet for 1.8.0: binary
was uploaded to devices directly to `C:sys\bin`, bypassing the package). The version is visible on
the device itself: log line
`profile: RA-6 400.34.93  SymbianOverclock 1.8.0`. We do not put `.sis` files
themselves into git, they are under `.gitignore`; the repository holds the package description and script.

Overclocking works on TWO devices: N95 8GB (RM-320) and Nokia E90 (RA-6): E90 +29%
with stock bus, N95 +50% with lowered bus. E90 added by filling the profile, without engine edits.


### E90 OVERCLOCKED: +29%, method is portable (2026-09-09)

The second device was overclocked with the same engine as the reference, and it was added not
by rewriting code, but by filling in a profile. Full breakdown — `findings.md` §36.

| multiplier | MPU | L3 bus | measurement | status |
|---|---|---|---|---|
| 86 stock | 330.24 | 110.08 | 328 MHz, 344 MB/s | — |
| 95 | 364.80 | 121.60 | 362 MHz (+10.3%) | clean |
| 107 | 410.88 | 136.96 | 408 MHz (+24.3%) | clean |
| **111** | **426.24** | **142.08** | **423 MHz (+28.9%)** | **clean** |

Every run — `status=0`, five rounds of endurance without corrupted words,
return to the stock multiplier with a control measurement of exactly 328 MHz. Across all three
runs the device did not reboot (`startTick` unchanged). NOP, ALU, and memory
scale in the same proportion, so this is true frequency, not an indicator artifact.

At the same time, two erroneous strings in reports were corrected: the profile label showed the old
strategy, and `NOT STOCK AT START` compared the E90 state with the RM-320 reference point,
thereby triggering on a completely stock device.

**What's next.** Prolonged daily operation of E90 at 111 is not verified — on RM-320 that was
a week of observations. E90 ceiling was deliberately not sought. An open debt is the DSP
divider setter for E90 (`iDspSetter = 0`): not needed for overclocking, needed only
by the secondary DSP branch.

### Primitive host is profile-based; E90 hits Open4all patches (2026-09-09, night)

The E90 profile was populated with measured addresses, the engine was taught to operate via the
Usbc host, regression on the reference passed — but the overclocking of E90 itself is not yet verified, because
the device became unreachable for uploading. Full breakdown — `findings.md` §35.

What was done. The READ/EXEC primitive host was moved into the profile (`TOcHost`): RM-320
operates via `dvfs_interface` (`DoControl(1)`, address in `r1`), E90 — via
`Usbc` (`DoControl(69)`, address in `r2`). The relock sequence remained one for
both, as required by project rule.

Three E90 addresses were measured on the live device by signature scan and disassembly
of dumped windows:

| field | value | how proven |
|---|---|---|
| `iDvfsBody` | `0x800bf874` | prologue of `DoControl`; dispatcher `0x800c09a4` calls specifically it and passes opcode 69 |
| `iRomSramRoutine` | `0x80058da4` | relock routine (DSB, SDRC, bypass, WFI, DLL `0x770E`); its address is passed by constructor into memmove to SRAM |
| `iPwrDownGlobal` | `0xc8003068` | `str r0,[r5,#4]` @`0x80059448` after constructor call |

DSP divider setter remains zero: it is not needed for overclocking.

**The device rebooted once, and the cause is known.** In `Prepare()` there was
a second, overlooked by me, call applying the READ patch with the old r1 constant; on
the Usbc host `r1` contains opcode 69, so the stub dereferenced address 69 in kernel mode.
Fixed, and at the same time three safeguards were added (§35): rejection on zeroes in
the profile, checking that the patch is alive before trusting it, and checking rollback
by direct reading of the six body words instead of a call into the kernel.

**Regression on RM-320 after rebuild is clean.** `patcher dsp` as before;
`patcher oc 107` yielded 328 → 408 MHz (+24.3%), five rounds of endurance without a single
corrupted word, return to 86 with `status=0`, uptime 722 minutes.

**Blocker.** After reboot on E90 `Open4all` patches were disabled, and writing to
`E:\\sys\\bin` returns `-46`. Manual action on the device is required: launch
`RomPatcher+` and enable patches. Immediately after that — `ocprobe patcher dsp e90`,
which exercises the entire new path without writing a single bit to PRCM; and only if it
is clean — `patcher oc` with a cautious multiplier.

### Read primitive on E90 WORKS; offline E90 addresses disproven (2026-09-09)

The main shift of the stage: **XIP is readable from user mode**, so firmware-dependent
addresses are found by signature scanning directly on the device. The cyclical
dependency that blocked the entire E90 branch ("to scan ROM, a read primitive
is required") has been eliminated. Full breakdown and evidence — `findings.md` §34.

Along the way, what §31 and §33 relied on was disproven: E90 addresses were derived from
`out/roms/e90.rom` under the assumption that a file offset linearly translates into
a virtual address. The device says otherwise — `HAL EMemoryROM` reports 19776 KB against 1.7 MB
in the extracted image, and unrelated code lies at the derived addresses. Because of this, the patch
landed on someone else's page: writing succeeded, there was nothing to execute, the device did not
crash. Three E90 profile addresses in the code were **zeroed out** — an incorrect number is more dangerous than
an absent one.

Measured on live E90 (Bluetooth, COM9; device did not reboot a single
time throughout the entire session, `startTick` unchanged):

| address | what it is |
|---|---|
| `0x8005a610`, `0x800610d8`, `0x800e640c` | `CM_CLKSEL1_PLL` pool |
| `0x8005c338`, `0x800610dc` | `CM_CLKSEL_MPU` pool |
| `0x800bf874` | `DLddUsbcChannel::DoControl` (primitive host) |
| `0x800c09a4` | `HandleMsg` dispatcher of the same driver |

With the measured host, `ocprobe usbcboot e90` gave **3/3** check words; rollback
and `FreeShadowPage` are clean. That is, the READ/EXEC primitive on E90 is proven on device.

New probe modes, all read-only: `romread` (base verification),
`romscan` (continuous scan of live XIP by signatures), `romdump <a> <len>`
(dumping a window to `E:\romdump.bin`).

Stock clocking of E90 by the same primitive is already **measured** (findings §34.1):
`CM_CLKSEL1_PLL = 0x00056400` (mult=86, div=4, sys_clk 19.2 MHz),
`CM_CLKSEL2_PLL = 2`, `CM_CLKSEL_MPU = 2`, `CM_CLKSEL1_CORE = 0x04360626`,
DPLL in lock. That is, E90 runs stock at the exact same **330.24 MHz** as RM-320, and
the guess "86" was confirmed by measurement. These three numbers went into `KProfileE90`.

**Next step.** Using the same scan, measure on the device the three addresses that
remained zero: the SRAM relock routine, the DSP divider setter, and the power-down global.
Only after that — the E90 WRITE path.

### Project in private git; E90 profile in code, `Prepare()` protected (2026-09-09)

A private repository `github.com/1234bpv/SymbianOverclock` (private) was set up.
Deliberately left outside the commit are `sign/` (private signing key) and `out/` (artifacts,
138 MB) — both under `.gitignore`; added `.gitattributes` for line endings.

In the code, the "E90 profile" debt was closed. Added `KProfileE90` with four addresses
derived offline and byte-matched (`iRomSramRoutine=0x80056874`,
`iDspSetter=0x80059aa4`, `iPwrDownGlobal=0xc8004194`, `iDvfsBody=0`), strategy
`EReadMeasureOnly`. Stock CORE/MPU layout of E90 left as `0`: runtime stock
cannot be derived offline. E90 bootstrap was found and verified (`findings §33`) — it
programs `CM_CLKSEL1_CORE=0x08301044` and `mult=97` (266 MHz, not 330/86), but that
is an early phase, not the OS operating stock (the latter is in compressed ROFS). Therefore these numbers
are NOT placed in the profile; `iStockMult=86` remains marked as a guess. At the same time, an erroneous
entry in `_summary.md` was corrected: the E90 multiplier reader is indeed in resident XIP
(`0x8005ea00`, twin of RM-320 `0x800b1544`), not in the paged part.

Most importantly — the footgun of `§27` was eliminated. `Prepare()` now rejects with
`KErrNotSupported`/`EOcStepUnsupported` BEFORE any kernel touch when the profile
is not `EReadDvfs` or `iDvfsBody==0`. `ocprobe` selects `KProfileE90` by the word `e90`
in the patcher command, so `patcher ... e90` on E90 logs a clean rejection instead of
patching RM-320 addresses into someone else's kernel. RM-320 did not change by a single bit; the `gcce urel`
build (ocprobe+ocgui) is clean.

**Confirmed on device (2026-09-09, N95 RM-320 over WiFi `192.168.3.22`).** Two
tests on the live reference. `patcher dsp` — stock path: `engine ready: SRAM
@cb202000, stock mult 86`, DSP domain read, `restore check ... -> 50
(original code back)`; the edit to `Prepare()` did not affect stock RM-320. `patcher dsp
e90` — guard: in log `profile: RA-6 400.34.93 (EReadMeasureOnly, no dvfs)` and
`engine NOT ready: err -5 at step [profile has no dvfs read path (E90)]`, that is,
rejection BEFORE kernel touch. The device did not reboot, remained stock. The footgun
of §27 on real hardware was eliminated.

**Next step** remains unchanged: the overclocking READ/WRITE path of E90 via Usbc bootstrap
(`usbcboot e90`) — requires E90 itself and an established BT link (SPP/COM to E90; N95,
which is currently connected, is not suitable for this step — it is a different device).

### E90 read/call primitive proven on RM-320; host is Usbc (2026-09-03)

Found and verified the carrier of the READ/EXEC primitive for E90 (where there is no dvfs): standard
USB client driver `DLddUsbcChannel` (LDD "Usbc"). The `HandleMsg` dispatcher was found
by signature `ldr [r1,#24]; cmp ,#0x80000000` (E90 `0x80095be8`, RM-320 `0x8010ce3c`);
its DoControl branch gives `bl DoControl` with a1 in **r2** (E90 `0x80094ac4`, RM-320
`0x8010bd04`, prologue byte-for-byte). The READ stub of the r2 variant
(`cmp r2,#0; ldrne r0,[r2]; moveq r0,#0; bx lr`) is placed in the prologue, reading goes
via `DoControl(69,addr)` (the dispatcher passes opcode 69 to DoControl even without
USB readiness). The patch is removed immediately + `FreeShadowPage`.

**Verified on RM-320** (`ocprobe usbcboot`, findings §32): Usbc opened, the stub
read three known ROM words correctly (3/3), rollback is clean, no reboot. The same
code on E90 — command `ocprobe usbcboot e90` (host `0x80094ac4`), ready to run.

**Next step:** run `usbcboot e90` on E90 as soon as the BT link is up
(as of 2026-09-03 not a single COM port on PC — SPP to E90 is not up; last year was COM9).
After a 3/3 match on E90 — populate `KProfileE90` (host Usbc r2, romSram/dspSetter/
pwrDown already exist) and implement the E90 WRITE path (resolving SRAM via this primitive).


### Baseline result: RM-320 overclocked (2026-08-08)

**DONE. Overclocking works, +29% stable (2026-08-08).** DPLL multiplier raised in
runtime from 86 to 111: **330.2 -> 426.2 MHz** on the core. Verified on live
RM-320, device is completely stable, receives calls, and holds WiFi.

| mult | MPU | L3 bus | Status |
|---|---|---|---|
| 86 stock | 330.2 | 165.12 | — |
| **107** | 410.9 (+24.4%) | 205.44 (+24.4%) | first verified point, headroom exists |
| **111** | 426.2 (+29.1%) | 213.12 (+29.1%) | **daily driver** (refined 2026-08-08) |
| 112+ | 430+ | — | autoreboot after minutes |

> **CORRECTED 2026-08-15.** In the "L3 bus" column here stood 164.35 and 170.50,
> that is, the numbers that divider compensation was supposed to yield. Compensation on these
> multipliers **did not work**: it selected a divider that the device does not accept, and
> an invalid value rejects the entire write to `CM_CLKSEL1_CORE` (`findings.md` §26).
> Hence the bus traveled alongside CORE all along, and the device has been holding +29% stably for a week
> not only on the core, but also on the bus and display — this is a stronger result than was
> recorded here.
>
> After the fix, the bus at 111 remains the same (213.12 — selection policy
> now targets specifically it, because it is measured), while the display for the first time is truly
> compensated: 106.56 MHz instead of the unintended 142.08. Both layouts
> verified on device.

Real-world effect, not synthetics: **SPmark 3D 62 -> 75 fps (+21%)**, UI and
emulation noticeably faster.

**The limit is ARM1136 itself at stock voltage, not the bus or display.** Proof: at
mult 129 both the bus and display are set EXACTLY to stock frequencies (`129/86 = 1.5`,
both dividers are multiplied by 1.5), yet the device still reboots
instantly. Full breakdown — `findings.md` §22.

Work cycle:

```
tools\build.cmd run 192.168.1.5 "patcher hold 107"   apply and leave
tools\build.cmd run 192.168.1.5 "patcher hold 86"    restore stock
tools\build.cmd run 192.168.1.5 "patcher oc 107"     overclock + measurements + return
tools\build.cmd run 192.168.1.5 "patcher dsp"        DSP domain registers, read-only
```

Everything is runtime: a reboot restores the device as it was, nothing is written to flash.

Three primitives on which this rests (none requires Kernel DevKit):
1. kernel address read — READ patch of `DoControl(1)` body @`0x80148410`;
2. kernel code execution — same patch, but with address check (§20);
3. staging ground outside SDRAM — SRAM chunk of power.dll, linear `0xCB202000`.

Plus `CM_CLKSEL1_CORE` divider compensation in the same commit (§21): without it,
display and bus traveled alongside CORE.

### Secondary branch: DSP domain (`docs/TASK_DSP_CLOCK.md`)

Task from the neighboring project `../TMS320C55x test`: verify whether C55x
is clocked at half its nominal rate, and if so — fix it. Kernel access to
PRCM exists only here.

**Done, with two results of opposite sign** (`findings.md` §23, §24).

**Initial hypothesis disproven.** On the device `CM_CLKSEL_DSP = 0x23C3`, field
`[3:0] = 3`, meaning C55x is **already at nominal 220.16 MHz**, and across 12 snapshots
the value did not change a single time. The "110 000" seen by the client is not
the DSP divider, but `CORE`: `ord60` computes `CORE / [3:0]`, while `CM_CLKSEL2_PLL[1:0]`
itself shifts between 1 and 2 under the DVFS governor — which this project already knew (§12, §18)
and because of which `Prepare()` takes the maximum of a series rather than an instantaneous reading. There is
nothing to fix there; their own control (throughput between "110" and "220"
runs did not change) pointed to this from the very beginning.

**However, a lever that did not exist appeared.** The DSP divider is now **changed in
runtime**: 3 → 2, that is 220.16 → 330.24 MHz (+50%), confirmed by readback,
neither CORE, nor SDRAM, nor display are affected. The route is calling the STOCK
setter of `power.dll` @`0x800AC59C` via the existing EXEC patch (trampoline in SRAM
sets `r1` and executes `blx` directly into ROM), plus **mandatory latching of
`PRCM_CLKCFG_CTRL`**: without it the setter returns 0, and the register does not change — this
is most likely the reason why both stock setters of this register are never called anywhere in ROM.

```
tools\build.cmd run <IP> "patcher dsp"          read domain
tools\build.cmd run <IP> "patcher dspset 2"     change and restore on exit
tools\build.cmd run <IP> "patcher dsphold 2"    change and leave
```

In the GUI the line `DSPDIV` appeared (left/right — value, OK — apply immediately,
without arm/confirm, because there is no hang window here). `RESTORE STOCK` and application exit
restore the stock divider along with the core frequency.

**The limit was found, and the divider is unsuitable for overclocking (`findings.md` §24.6–§24.7).**
The DSP domain at CORE/2 remains alive while nothing executes on C55x (240 s of deep
idle — clean), and crashes the device as soon as DSP is brought up: death occurred upon
loading the base image, in the window where the divider was held by our live process.
The previous explanation "an issue of process ownership / idle-retention" was thereby
disproven and corrected in the text.

The divider is an integer, so from the stock 3 the only step is immediately 2, that is +50%. No
intermediate values that the silicon could withstand exist.

**However, DSP overclocking has already been achieved — via multiplier.** DSP = CORE/3, so at working
`mult 111` it runs at **284.16 MHz (+29%)**, and the device holds this mode daily.
Measured with the neighboring project's benchmark: **1 136 260 → 882 995 µs, −22%**, that is
exact clock scaling. The limit of C55x at stock voltage is between 284 and 330 MHz.

| C55x frequency | origin | status |
|---|---|---|
| 220.16 | stock | — |
| **284.16** | `mult 111` | **works daily** |
| 330.24 | divider 2 | crashes device during DSP operation |

In the application, the line `DSPDIV` remains: moving **up** (4, 6, 8 → 165/110/82 MHz)
is safe and saves power, while an attempt to go below stock enables the watchdog with
rollback and writes to log that this crashes the device and that the correct lever is `MULT`.

### Secondary branch: MPU divider (`docs/TASK_MPU_DIVIDER.md`)

Task from the neighboring project `../PS1Emulator`, assigned on 2026-08-15. It
requires a configuration in which the core frequency is changed while the bus frequency is not — otherwise
the emulator frame budget cannot be split into processor and memory parts. All
required configurations are **underclocking** (lowering) frequencies, so the stability question does not arise.

**The §5 question was closed with three runs on 2026-08-15.** Full breakdown —
`findings.md` §26, reply to client —
`../PS1Emulator/plans/reports/answer-mpu-divider-2026-08-15.md`.

The layout **holds**, there is no need to actively sustain anything. `CM_CLKSEL_MPU = 4`
held for 40 s under load without a resident patch and across an OPP transition;
non-stock `CM_CLKSEL1_CORE` did likewise, and the word left by the client's run
survived until the next day. The hypothesis "governor overwrites dividers" is disproven.

**The actual reason for what was described as "dividers do not hold" is different:
compensation was not being written at all.** An invalid divider value rejects the entire
write to `CM_CLKSEL1_CORE`, not just its own field. Measured by separation of variables:
`mult=107 l3=6 dss=7` was not written, while `mult=107 l3=6 dss=6` was written.

The lesson is broader than a single value: **the firmware validation table is not enough**.
The DSS1 validator accepts 7, but the hardware does not. Therefore automatic selection now operates on
a deliberately narrow set (1, 2, 4, 6, 8, 12, 16), manual selection remains wider, and
every write is read back.

At the same time, two erroneous conclusions in the text were corrected: the set of allowed L3 values in
§21 (0, 3, 5, 9, 18 are rejected, not just 18 alone) and the direction of SDRAM refresh
action — a slower bus makes it **less frequent**, not more frequent, so lowering the bus
below stock is a risk, not a safeguard.

At the same time, the instrument was calibrated: half core frequency with an unchanged bus costs
`BestMem` 20.8%, while a lowered bus with an unchanged core costs 30.2%. That is, this measurement
responds to both domains, and in old logs where core and bus moved together, it
separated nothing.

What was done in code: constant compensation reference point (`dssdiv` ratchet closed),
line `NOT STOCK AT START`, `patcher clocks` command with full layout and
a child process, readback in `patcher hold`, and the `ReassertDividers` blob —
which, as it turned out, is not needed for this task, so it remains behind the
`hold=1` switch and was not executed on the device.

The new compensation was verified on both working multipliers: `107 -> l3=4 dss=8` and
`111 -> l3=4 dss=8` are written, read back, and hold the window. That is, daily
configuration leaves the bus where it actually was (213.12 MHz), while the display
for the first time is truly compensated — 106.56 instead of the unintended 142.08.

**Two limits found along the way.** A ramp DOWN to multiplier 43 crashes the device — twice
out of two, and the log pinpoints the moment inside the ramp, not at the final layout
(§26.10). Stepping below 86 was never attempted in the project; for now we do not go there.
And a lowered bus crashes the device even without a ramp: `l3div=8` at stock multiplier
was written and held the window, but the device died immediately after it — twice out of two,
and both times specifically upon **returning** the stock state (§26.12, §26.14). The hypothesis
about stretched refresh was disproven by our own measurement: the ARCV firmware does move it
(1242 → 806 on OPP transition), and charge retention is clean — zero corrupted words
per megabyte over three five-second holds. What remains is the asymmetry: 4 → 8 clean, 8 → 4 death.
The guess about the bypass window was tested and also weakened — the lightweight path without bypass
(`ReassertDividers`) died just the same. Thus both return methods crash, and
they share two things: doubling the bus frequency and the lack of SDRAM protection in
the lightweight blob.

**The step-by-step log revealed the exact location (§26.15).** The device perishes **inside**
`ReassertDividers`: the line indicating entry into the call exists, the line with its result does not.
A snapshot immediately before death added a missing detail: the governor at that
moment is at the lower OPP, so the bus runs at 41.28 MHz, and restoring `l3div` to 4
multiplies it fourfold relative to what memory just saw — live, without
self-refresh, which exists only in the large relock blob. Thus of the two candidates,
the first remains. The solution, when time permits: the same pair of "self-refresh
before change, exit after" in `OcDivBlob`. **Three deaths on a single operation — until then
we do not go there.**

**The instrument was decomposed into two contributions** (§26.13): `BestMem` reflects about 0.9 of
bus frequency change and about 0.42 of core frequency change. That is, it is a mixture, not
a bus measurement, and all prior "RESULT MEM" in logs must be read accordingly.

For the client, out of four configurations three are available: A and D are safe, C is available
without multiplier 43 (`mult=86 mpu=2 l3=8 dss=6` gives the same frequencies) but the device cannot
be kept in it, B is unreachable. The remainder of what is open — §26.9; pairs require
`ps1stage1.exe` on the device.

### History prior to this result

We know **what** to write — register `CM_CLKSEL1_PLL` (physical `0x48008540`,
virtual `0xC6008540`), bit fields, and formula (section 4 in `findings.md`).

We lack **the means** to write: an arbitrary kernel address write primitive.

| Branch | Status |
|---|---|
| User DVFS API (`dvfs_interface`) | **closed**: binary latch, level hardcoded, does not provide overclocking |
| Custom kernel-side LDD | **closed**: public SDK lacks `include\kernel\` and `ekern.lib` |
| `TestPowerAPI` (`d_powerapi.ldd`) | **closed**: does not provide arbitrary write, while probing hangs the device |
| RomPatcher+ `patcher.ldd` | **suitable for patching ROM code**, not for direct PRCM: writing to MMIO would be 4×`strb`, but ROM-shadow successfully patched `dvfs_interface.ldd`; PRCM has already been read through it (findings §10, §15) |
## Stage: Generalization to Other Firmwares (from 2026-08-30)

Devices are at home, the daemon is updated and controls all three over two transports: N95 via
WiFi/TCP, E90 and C5-00.2 via Bluetooth (`deploy_net.ps1 -Com COMx`). The goal of the stage is
to make overclocking work not only on our RM-320 `31.0.015` firmware.

Two corrections to previously recorded notes, both from Pavlo:

- **E90 fully cracked.** The limitation recorded in
  `SymbianDeployDaemon/docs/state.md` (`PUT` to `C:\sys\bin` returned `-46`,
  updating only via SIS from the card) is no longer in effect. Iteration on E90 is now
  just as cheap as on N95.
- **C5-00.2 is excluded from this stage.** It is not an OMAP 2420, but an ARM11 600 MHz with FPU,
  meaning a different SoC. First we bring 2420 to completion.

What prevents putting the code on another firmware: four addresses taken from the disassembler
of our specific ROM, and inferring them on the device is impossible (to scan ROM,
a read primitive is needed, and to obtain it — one must already know the first address).

| Constant | Value for RM-320 31.0.015 | What it is |
|---|---|---|
| `KDvfsBody` | `0x80148410` | body of `DoControl(1)` in `dvfs_interface.ldd`, target of the READ patch |
| `KRomSramRoutine` | `0x800A7F6C` | idle routine of `power.dll`, from where code is copied to SRAM |
| `KDspSetter` | `0x800AC59C` | stock DSP divider setter in `power.dll` |
| `KPwrDownGlobal` | `0xC80030C8` | power-down object global |

Addresses of PRCM and SDRC registers are not tied to firmware — this is OMAP 2420 itself.
All that needs to be verified for a new firmware is the peripheral mapping offset
`+0x7E000000`: it is chosen by the variant kernel code, not the hardware, and it is also visible from
the dump.

The list of devices on this SoC, product codes, and verification order are in
`ROADMAP.md` §2a. The main point from there: one needs to download different **versions** of firmware, not
regional variants, because the product code does not alter the ROM kernel (three `.vpl` in
`firmware/n95-8gb/` reference the same MCU file with the same CRC `e7ffb3c7`).
From the package, only MCU/CORE (`*.C0R` / `*_core.fpsx`) is needed.


### Downloaded 2026-08-30

In `../firmware/` lie MCU images (`*.C0R`/`*.c00`/`*.C0`) and language packs for
five devices: E90 RA-6 `400.34.93`, N95 8GB RM-320 `35.0.001` (in addition to
our `31.0.015`), N95 RM-159 `35.0.002`, N82 RM-313 `35.0.002`, N93 RM-55
`20.0.058`. All five have the valid FPSX signature (`b2 00 00 0x`, followed by `BB5`
at offset `0x11`), meaning these are genuine images, not wrappers.

Source — `archive.org/download/nokia-bb5-euro3-firmwares`, where files lie already
unpacked, and Internet Archive serves an individual file from inside the zip via a direct
link. A full catalog of versions exists on `archive.15pmm01.com` (RM-320 alone
has 130 versions there), but each is wrapped in an InstallShield installer. The path to
them has been clarified: `setup.exe /extract_all:<dir>` yields `Disk1` with `data1.hdr`,
`data1.cab`, and `data2.cab`, which is the ISc format that 7-Zip cannot read. An unpacker
in Node (`tools/unshield.js`) has been written, see below.

InstallShield unpacker written: `tools/unshield.js`, pure Node without
dependencies, ISc format version 9, data compressed with standard deflate. Verified
not by a subagent report, but by our own run: `list` on RM-320 `20.0.016` shows
94 files, `extract` extracts `RM-320_20.0.016_PR.C0R` of 70,989,931 bytes in
6.7 s, and the FPSX signature is in place in the unpacked file. One trap: MCU
names differ between models — `.C0R` on N95 and N82, `.C01` on E90, `.C0` on N93,
so the search pattern must be `.(c0[0-9r]?)$`, not a list of extensions.

Mass run completed 2026-08-31: 52 MCU images, one for each
"model + firmware version" pair. E90 is fully covered (six versions), N82 —
six, N95 RM-159 — eleven, RM-245 — seven, RM-160 — four, N95 8GB
RM-320 — seven and RM-421 — three, N93i RM-156 — four, N93 RM-55 — three. Total
4.1 GB. All 52 images have the valid FPSX signature (`b2 00 00 0x` followed by
the string `BB5`), verified by a continuous pass rather than selectively.

Two minor details found by that pass. In early E90 firmwares (`07.24.0.3`,
`07.38.0.2`, `07.40.1.2`), the header fields are in reverse order —
first `XSR 1.5`, then `BB5` — so the fixed-offset check falsely
triggers on them. And some Nokia packages contain no MCU at all: in N93
`10.0.025` these are 45 files of language packs and manifests alone. For such
versions, the core must be taken from another, larger package of the same version — for N93
RM-55 this worked, for the Chinese RM-153 no core exists in any of the three
packages, so this model remained uncovered along with N93i RM-157.

The unpacker has been proven definitively: in the mass run, RM-320 `31.0.015` was extracted anew —
the very version whose image we already had from another
source (archive.org). The MD5 matched byte-for-byte:
`00d98c87a04dfd4e0db71b3b3f832e50`. That is, `unshield.js` reproduces content
accurately, not approximately, and images from installers can be trusted just like
unpacked ones.

Open question: what to use to deploy an MCU image into the `z:` file system. The existing
`eka2l1/data/drives/z/rm-320/` was produced by installing into EKA2L1 via `.vpl`;
whether EKA2L1 will accept our set with a single product code has not yet been verified.



### E90: Application Installed, Launch Deliberately NOT Performed (2026-08-31)

`ocgui.exe` (39752 bytes) resides in `C:\sys\bin` on the E90, registration is in place.
But launching the current build on the E90 must not be done: `Prepare()` immediately writes a READ patch to
`0x80148410` — an address valid only for RM-320 — while on the E90 this is foreign kernel code, and
due to the absence of DVFS, the patch is not removed. The consequence would be a reboot.
Details and requirements for the E90 build — `findings.md` §27 (caution).

Autonomously, the safe part was performed: disassembling E90 clocking code from the image
(`objdump`, window before the PRCM pool). Found the clock write routine in `power.dll`
with multiplier register `0xc6008540` and a read helper alongside — the mechanism on the E90 exists,
only not via DVFS. Next step for the E90: install the image into EKA2L1 for
exact vaddrs, then make an E90 build with parameterized addresses and safe
rollback of the READ patch on the failure branch.


### Lesson: Blindly Opening Kernel Channels Crashes the Device (2026-09-02)

The first version of `e90scan` (`E90ScanReaders`) opened `DoCreate` to EVERY
loaded LDD. On the E90, one of them crashed the kernel — the device rebooted during
the run (PUT+hash succeeded, RUN started and did not return). Some drivers execute
initialization on DoCreate that crashes without valid parameters. This is
a violation of the rule "do not turn on blindly anything that can hang".

Fixed: `e90scan` now only ENUMERATES drivers (`TFindLogicalDevice`, without
any DoCreate). Selecting the READ patch carrier and analyzing its DoControl is done offline from
the resident XIP; the next version will open exactly one selected
driver, protected, logging the name BEFORE the attempt.

At the same time, confirmed: access to `E:\sys\bin` on the E90 works only when hack
patches (Open4all) are active, and they do not always turn on automatically after a reboot — the first
PUT gave `ERR open -46`, and succeeded after manually enabling the patches.

### E90: Reader Reconnaissance Probe Built (2026-09-02)

Three of the four E90 profile fields have been deduced offline (`iRomSramRoutine=0x80056874`,
`iDspSetter=0x80059aa4`, `iPwrDownGlobal=0xc8004194`, `iDvfsBody=0`). What remains is
the READ primitive for resolving SRAM. The choice made (by user) is bootstrapping via write:
using RomPatcher+ write to patch the opcode body of a user-openable E90 driver
with a READ stub (`cmp r1,#0; ldrne r0,[r1]; moveq r0,#0; bx lr`, the same one as on
RM-320) and read through it.

The first step is to find out which drivers on the E90 can be opened at all with our
capabilities. Added mode `e90scan` (`E90ScanReaders`) to `ocprobe.cpp`: enumerates
loaded LDDs and attempts `DoCreate` to each, reporting OPEN / error code. It patches
NOTHING — safe reconnaissance. Probe capabilities have been expanded to "all except
TCB/AllFiles/DRM" so that an open failure indicates channel unavailability rather than lack of
capability. Built: `ocprobe.exe`, `group/SymbianOverclock_sha1.sis`.

Candidate carriers from the resident XIP of E90 (for the next version, once we know what
opens): `RSecEnvLogicalChannel`, `DSdramCrashDataBufferLogicalChannel`,
`DIscChannel`/`NokiaISCDriver`, `DLddUsbcChannel`.

Next step: run `ocprobe.exe e90scan` on the E90 (daemon via Bluetooth),
retrieve `E:\ocprobe.log`, select an openable driver, analyze its
DoControl offline (it is resident), and build a second version that installs the READ patch,
verifies it against a known ROM word, and resolves SRAM
`0xc8004194+4 -> obj -> obj+16`.

### Clocking Addresses Deduced Across the Entire Lineup; E90 WRITE Blocked on SRAM Resolving (2026-09-02)

Three code anchors (multiplier reader, `iRomSramRoutine`, `iDspSetter`) were found by
signature and byte-verified in RM-320, RM-159, N82, N93, N93i; for E90 —
`iRomSramRoutine=0x80056874` and `iDspSetter=0x80059aa4`. Four devices were analyzed
in parallel by `agy` subagents, E90 and the reference — by myself. Summary table — `out/analysis/_summary.md`,
evidence — `out/analysis/<dev>.md`, full breakdown — `findings.md` §31.

E90 offline is blocked on two fields: `iDvfsBody=0` (no CPU dvfs exists, reconfirmed —
"dvfs" in XIP pertains to the display) and resolving the SRAM address, which is done
at runtime via a read primitive, which does not exist on the E90. The WRITE primitive (`patcher.ldd`)
exists. Therefore, WRITE on the E90 requires either a one-time read on the device or bootstrapping read
via write. A safe test step without reading is `EReadMeasureOnly` (measurement by work).

### XIP Is Now Extracted Without EKA2L1 GUI; All Six Devices Analyzed Offline (2026-09-02)

The gate from §29 (manual installation of `.vpl` into EKA2L1) has been lifted. The EKA2L1 firmware
installer crashes on E90 and N82, and a headless installation mode does not exist in the build
at all. Therefore, XIP is now extracted directly from the firmware image — `tools/fpsx_extract.js`,
an exact port of EKA2L1's open-source FPSX parser. Full breakdown and evidence — `findings.md`
§30.

Verified against ground truth: the image extracted from RM-320 in the code region matches byte-for-byte
with the `SYM.ROM` that EKA2L1 once unpacked itself, and the multiplier `0xc6008540` is located
precisely at `vaddr 0x800b16ec`. Ran across all six OMAP2420 devices — each yielded
a valid XIP with the multiplier register inside, images in `out/roms/*.rom`. E90 around
its `0x800b76dc` pool disassembles into coherent ARM PRCM code (`objdump`).

Along the way, it turned out that the OS is split: N95/N82 keep the entire OS uncompressed
(`SOS+CORE`, ~43 MB), while E90/N93/N93i keep only the resident XIP (`SOS+UPDAPP`, 1–1.8 MB),
compressing the rest (`SOS*CORE`, asterisk). The clocking code is in the uncompressed part on all of them,
which is sufficient for overclocking.

**The next step** is now purely offline: disassemble E90 `out/roms/e90.rom`
around `0x800b76dc`, find the four firmware-dependent addresses (equivalents of
`KRomSramRoutine`/`KDspSetter`/`KPwrDownGlobal` and the multiplier write location), populate
`KProfileE90`, and implement the `EReadMeasureOnly` branch. The device is needed only at the end
to confirm what was guessed.

### Offline Adaptation: Method, Safety Fuse, and Device Profile (2026-09-01)

Done without device involvement, except for the final upload last time. Three things:

1. **Adaptation method described** — `findings.md` §29. In brief: there is no single linear
   "file→vaddr" map for an image (FPSX stores ROM in blocks), so real
   addresses are taken from `TRomImageHeader.iCodeAddress` of each module, i.e., from
   the unpacked `z:`. We do not write our own FPSX unpacker — its role is fulfilled by
   installing the image into EKA2L1 (without the device) + `romtool.ps1`. The RM-320 reference
   has been extracted: register read helper `bl 0x800b1c60`, multiplier `0xc6008540` in
   the pool, DPLL formula @`0x800b1544`.

2. **Emergency rollback of the READ patch fixed** (§27 debt). The body of `Prepare` after
   applying the patch was moved into `PrepareProbe()`, and `EndPatchSession()` is now
   called ALWAYS — both on success and on any probe error, and even
   if `PokeWords` fails partially. Now a failed `Prepare` on a device with different
   addresses does not leave a substituted foreign kernel body behind. Built, RM-320 unchanged.

3. **Device profile** (`TOcProfile` in `ocengine.h`). Four firmware-dependent
   addresses (`iDvfsBody`, `iRomSramRoutine`, `iDspSetter`, `iPwrDownGlobal`), the stock
   clocking scheme, and reading strategy (`EReadDvfs`/`EReadPowerHelper`/
   `EReadMeasureOnly`) are assembled into a structure. The engine takes addresses from `iProfile` rather than
   from constants. PRCM registers and field masks remained global — this is OMAP2420 itself.
   Reference `KProfileRM320` is populated with proven numbers; RM-320 works as before
   (built, behavior is identical). Adding a device = adding a second such entry.

What REMAINS for a working E90 build, and why this is precisely the boundary of autonomy:
- **Exact E90 vaddrs** (romSram, dspSetter, pwrDown, plus power.dll read/write
  helpers). Unpacking the E90 image in EKA2L1 is required — which is a GUI action that
  cannot be performed headless. This is the only manual step.
- **Reading strategy branch without dvfs.** The entire `Prepare` probe is currently built for
  `EReadDvfs` (patching the dvfs body). For E90 (`EReadMeasureOnly` — the safest, without
  any read address, measuring work only) this branch needs to be implemented, and it is worth
  writing it against real E90 addresses rather than against guesses.

Next step (requires a single click from Pavlo): install E90 `.vpl` into EKA2L1
(File -> Install -> Device -> Firmware(VPL) -> `firmware/e90-ra-6-400.34.93/
RA6_0564689_400.34.93_001.vpl`). Then I disassemble E90 `power.dll` offline,
populate the E90 profile, and implement the `EReadMeasureOnly` branch.

### "From File" Method Verified and Run Across All Images (2026-08-31)

`tools/romscan.js` searches the raw firmware image for the `dvfs_interface` driver (by
name — reliable), the body of `DoControl(1)` (by signature — exact patch site), and PRCM
register pools (write mechanism). Verified against ground truth: on RM-320 `31.0.015`,
the script outputs exactly `KDvfsBody` = `0x80148410`, which we obtained on the device; on the E90,
the driver is absent both in the image and in live `LS`. Full method breakdown and its limitations —
`findings.md` §28, table across 51 images — `firmware/_romscan.md`.

Key point: the body signature is a positive-only detector (a miss does not prove
absence, because `DoControl(1)` is compiled differently between versions), so
present/absent is judged by driver name. DVFS is present in 25 out of 51 images and
appears starting from a certain version even within a model (RM-159 — from `13.0.003`, Chinese
RM-245 — nowhere, RM-320 — everywhere). PRCM pools are present almost everywhere, meaning the
overclock WRITE mechanism exists even where there is no DVFS reader.

Delegated to AGY subagents (four parallel `ag-programmer`): they ran
romscan across their batches of images and returned tables. The result was checked by our own
run — matched down to the offset, including anomalies, so the subagent reports were accepted,
and the anomalies turned out to be a property of the method, not an error.

The exact numerical address CANNOT be obtained by simply subtracting a base from the file (XIP does
not map with a flat offset — verified). The reliable path to addresses and the complete list
of files is to install the image in EKA2L1 and read `z:` via `romtool.ps1`; the device is not
needed, but the step is manual (installation into the emulator is not possible for a headless subagent).

### E90 Connected, and the Very First Check Changed the Plan (2026-08-31)

Connecting over Bluetooth (`deploy_net.ps1 -Com COM9`) worked on the first
attempt: the daemon is alive, auto-started at boot, access to `C:\sys\bin\` is
full, `patcher.ldd` is present on the device. Firmware was read from the device itself —
`400.34.93` dated 21-04-2009, RA-6 — and its image is already in our possession.

Next, an important fact was discovered, clarifying the next step. E90 firmware lacks
the DVFS subsystem from which we read frequency for verification, and lacks two
backup readers (`d_powerapi.ldd`, `memoryaccess.ldd`). However, the overclocking
mechanism itself is present on the E90: `power.dll`, `ehwaomap.pdd`, and `patcher.ldd` are in place,
and the image contains the same mapped PRCM registers, including the DPLL multiplier
`0xc6008540`, packed into the literal pool of the `power.dll` code. That is, what is missing is not
the mechanism, but the result reader. Full breakdown and boundaries — `findings.md` §27 with
correction.

Therefore, the item "signature search for addresses" (`ROADMAP.md` §2) is inapplicable to the E90
in its current form. First, a different entry point for reading memory must be found,
and only then can offsets be discussed. For other firmwares of the **same**
RM-320 model, the plan remains unchanged — the subsystem there is the same.
## Next Step

Symbian-inflate is implemented and **proven** (`tools/syminflate.js`): decompresses
`file0/1/2` exactly to their `iUncompressedSize` (2688/2700/2780 bytes), images
yield valid ARM. Dispatcher and write path analyzed — `findings.md` section 10:
channel functions 100–105, routing outside `0x85a4`, write via kernel import
**ordinal 725** `(dest,src,len)`; address outside ROM is written directly, without
rejection. Artifacts — `out/rp/inflated/`.

Analysis of ekern (export directory of the real image) and full analysis of the patcher channel
yielded **three results** — details and evidence `findings.md` section 10:
- ordinal 725 = `memmove(dest,src,len)` @`0x8006F800`; 401/402 = lock/unlock; all
  30 patcher stubs resolved against the real export table — the write chain is
  proven from client to `str` in the kernel;
- **but** the patcher channel DOES NOT provide arbitrary address reading (105 takes src only from
  a descriptor, 103 is a fixed getter);
- **and** memmove for len=4 writes 4×`strb` — wrong access width for PRCM registers.

**Conclusion: `patcher.ldd` is unsuitable for programming PRCM.** It does not block
MMIO mapping (an address outside ROM goes via direct str, kaomap proves this); what blocks
is the memmove access width and the lack of reading.

### ROM Driver Inventory — Done, Negative Result

Scanned all 2712 modules for kaomap Read32/Write32/Modify32 imports
(findings §11). 24 import them; 6 LDDs have a user side. In each, the MMIO address is
the hardcoded driver base, **none takes an address from the client.** A raw
MMIO primitive with correct access width does not exist in ROM.

### Reorientation: How Firmware Actually Controls Frequency (findings §12)

Analysis of power.dll: the DPLL multiplier (`CM_CLKSEL1_PLL`) is set only at boot and
is not written at runtime (145 Write32, none to `0xC6008540`). Runtime DVFS
scales only **dividers** downward from the 330 MHz ceiling. Therefore:
- to lower a divider — one must first read the current one (no read primitive);
- to raise DPLL — there is no validated runtime path in firmware, doing it on our own
  is possible only with a correct MMIO primitive (which does not exist).

**Exact formulation of the blocker:** what is missing is not knowledge of "what to write" (known), but
a working channel into the kernel with correct access width **and reading**.

### Chosen Approach: Patch power.dll CODE, Not MMIO (findings §13)

The DPLL boot multiplier is not programmed by Symbian code (it is set by the pre-Symbian
bootloader), so there is no table for a "clean" patch. Instead:

- power.dll code (`0x800A7C98..0x800B3298`) is in a ROM region that patcher patches
  via the shadow path; patching code = writing to normal memory (byte-by-byte memmove is
  correct here, access width issue is eliminated);
- registers are touched by power.dll itself using its correct 32-bit kaomap primitives;
- the trigger is already there: `DoControl(1)` on `dvfs_interface` → `GetClockFrequency`
  `0x800B15DC`.

**READ patch — executed on the device 2026-08-07.** Self-test of writing to user buffer
passed (`DoControl(105) -> 0`, bytes matched), after which the ROM-shadow path
accepted the 16-byte patch `dvfs_interface.ldd @0x80148410`. The patch read:

```
CM_CLKSEL1_PLL = 0x00056400  sys_clk=19.2 MHz, mult=86, div=4
CM_CLKSEL2_PLL = 0x00000002  core_sel=2
CM_CLKSEL_MPU  = 0x00000002  mpu_div=2
CM_CLKEN_PLL   = 0x0000000F  DPLL locked (low bits == 3)
```

Hence `DPLL = 19.2 * 86 / 5 = 330.24 MHz`; with `core_sel=2` and `mpu_div=2`
the MPU also receives 330.24 MHz. Reducing the MPU divider from 2 to 1 would give around
660 MHz — an unacceptable jump, this branch is closed. The READ patch only reads;
a reboot restores the original ROM code.

**Revisited 2026-08-07 (findings §17): the firmware DOES have an SRAM-resident
PRCM/SDRC critical section.** `power.dll` at boot maps 4 KB of physical
`0x4029E000` (internal OMAP2420 SRAM) and copies a 324-byte routine from
`0x800A7F6C` there; on deep idle it executes **from SRAM** and performs
self-refresh SDRAM → bypass DPLL → polling `CM_IDLEST_PLL` → WFI → relock →
polling → relock DLL. That is, the structure from `sram242x.S` is confirmed not by a retelling
of Linux, but by the code of this device. At the same time, the SDRC parameter is settled:
`SDRC_RFR_CTRL = 0x0004DA01` (≈165 MHz), device is at stock OPP 330/660/165.

Two of our assertions proved false: (a) that there is no validated runtime
relock path in the firmware; (b) that power.dll writes MMIO only via kaomap — it
writes via direct `str` by base+offset as well, which is why earlier scans missed everything.

**New blocker (exact):** copying to SRAM is done **once at boot**, so
a runtime ROM-shadow patch of `0x800A7F6C` hits the wrong copy, not the one that executes.

**WRITE phase re-evaluated 2026-08-07: do not run on the device.** Reference
OMAP2420 Linux code performs relock not as three simple writes: it operates from SRAM
and pre-loaded I-cache, transitions to fast-relock bypass, waits for
`CM_IDLEST_PLL`, only then changes `CM_CLKSEL1_PLL` and commits `CLKCFG_CTRL`,
then relock/wait, and in this same critical section updates SDRC refresh and DLL.
Details and proof status — findings §16.

Hence a direct patch of `power.dll` is insufficient and dangerous: its code may be
in ROM, but the stack, data, and SDRAM accesses during a frequency change are not isolated.
Even a small `mult=86 -> 87` (+1.16%) alters CORE/L3/SDRAM, whereas the official
OMAP2420 table has 330 MHz as the fastest point. Before a write touch, all four are required:
1. ~~confirmed for RM-320 SRAM/ICache-safe execution path~~ — **found**
   in firmware (findings §17), but we do not yet know how to patch it: the copy in SRAM
   is made at boot;
2. ~~exact current SDRC refresh~~ — **closed**: `RFR_CTRL = 0x0004DA01`
   (≈165 MHz), DLL value `0x770E`. New values for the new frequency remain;
3. confirmed voltage headroom or controlled Menelaus sequence;
4. independent review of exact ARM bytes, timeout/rollback behavior, and one
   dedicated runtime test. In the absence of any of these items, do not perform WRITE.

### Immediate Step — READ-Only, Without WRITE (Deduced from Code, findings §17)

The chain to the linear address of the SRAM copy was found. `power.dll @0x800A85E4` stores
a pointer to the power-down object in a global:

```
obj      = *(0xC80030CC)        ; global 0xC80030C8, field +4
sram_lin = *(obj + 16)          ; linear address of 4 KB mapping @phys 0x4029E000
chunk    = *(obj + 12)          ; the HW chunk itself
```

These are two arbitrary kernel address reads — exactly what the **verified**
READ patch (§15) can already do, using the same mechanism and with the same risk class. We write
nothing. The goal is to confirm that SRAM is indeed mapped and to obtain the address
where the executing copy of the routine lies.

If the address is confirmed, a path closed earlier opens up: patching
the **SRAM copy** instead of ROM. The limitation of §10 item 2 (memmove writes 4×`strb`) **does not
apply** there — it is normal memory, not MMIO.

Unresolved and mandatory prior to any WRITE: there is no controllable user trigger
for the routine (executes only on deep idle, depth ≥ 2), so the effect
of the patch will be poorly observable. Plus voltage (Menelaus) and new SDRC
values for the new frequency.

**Settled by the READ stage:** client call format, capability set of existing
`ocprobe.mmp`, I-cache/ROM-shadow coherence, and actual PRCM values.
`patcher.ldd` loaded and opened with existing capabilities
`PowerMgmt WriteDeviceData ReadDeviceData ReadUserData WriteUserData ProtServ`;
no expansion needed. Run artifact — `out/ocprobe.log`.

Input files: `out/rp/file0` = `patcher.ldd`, `file1` = `patcherShadow.ldd`,
`file2` = `patcherS3.ldd` (for S60v3, the first two are relevant). Compression —
`KUidCompressionDeflate` (`0x101F7AFC`).

## On-Device Application (SIS)

`ocgui.exe` — overclocking and stress tests without a PC. Build and sign:

```
tools\build_sis.cmd            -> group\SymbianOverclock_sha1.sis
tools\build_sis.cmd push <ip>  same + upload to E:\Downloads
```

**v1.1 (2026-08-08): manual dividers for all subsystems.** The `DIVIDERS` row toggles
`AUTO` / `MANUAL`. In `AUTO`, dividers are selected automatically (L3 as high as possible within
+15%, DSS1 closest to stock). In `MANUAL`, each of the seven fields of
`CM_CLKSEL1_CORE` is adjusted left/right individually: L3, L4, DSS1, DSS2, VLYNQ, SSI,
USB. Alongside the value, the resulting frequency and **deviation from stock** are shown —
this is precisely what is used for tuning. Switching to `MANUAL` takes what the automation
selected as a starting point, so the baseline is immediately working.

Allowed field values are taken from validation in `power.dll` setters (findings §21),
meaning the application will not allow setting a divider invalid for this SoC.

**v1.2: MPU divider.** A separate `MPUDIV` row — this is already a different register,
`CM_CLKSEL_MPU` [4:0]. Valid values are **1, 2, 4, 6, 8** (firmware rejects
0, 3, 5, 7 — divider 3 does not exist on this SoC). Shows resulting CPU and CORE.

Why: with `mpu_div=1`, the same core frequency is achieved with **half the CORE frequency**.
This is a direct way to verify where the limit actually lies — in the ARM itself or in CORE as
the highest internal frequency. For example, CPU ~422 MHz: either mult 111 with CORE 852
(div 2), or mult 55 with CORE 422 (div 1). If the second holds better — CORE is to blame,
not the core.

**v1.4 (2026-08-15): memory bus divider as a separate row.** The `BUSDIV` row is located
directly beneath `MPUDIV` and edits the L3 field in `CM_CLKSEL1_CORE`, i.e., the frequency from which
the SDRAM controller is clocked. It shows not the divider itself, but the bus frequency and deviation
from stock; the `auto` label indicates that the value is still taken from autoselection and will track
along with the multiplier. Left/right sets a custom value, OK returns to autoselection.

Why a separate row when the same field is in the `MANUAL` list: this is the only divider
used to separate "limit in the core" from "limit in the bus". Measured (§26.13) that `BestMem`
responds to bus changes almost entirely, but to core frequency changes by only two-fifths;
to leverage this, the bus needs to be adjusted independently of the multiplier without
entering manual mode and without simultaneously affecting the display divider. Both controls
for the same field maintain the same value, so they cannot diverge.

The set of values here is **narrower** than what the firmware allows: 1, 2, 4, 6, 8, 12, 16.
The reason was measured (§26.0) — the `power.dll` validator lets through values that the device
does not accept, and an invalid value in a single field rejects the **entire** register write.
The wider set remains in `MANUAL`, where verification by reading back exists.

The row itself warns about direction, and the warnings are also measured: a bus **below**
stock holds cleanly, but returning from it killed the device three times out of three
(§26.15), while a bus **above** stock with an unchanged multiplier simply was not tested.

The list scrolls (there are more rows than fit on the screen).
The previous version is saved in `release/v1/` along with sources.

Joystick control: up/down — select row, left/right — change value
of selected row (multiplier, mode, or divider), OK — activate row.
Protection is two-level and intentional: the slider only prepares the value, `APPLY SETTING`
arms it, and **only the separate `CONFIRM OVERCLOCK` button** executes the relock.
There is no autostart: after a reboot, nothing is applied.

Tests (each is both a benchmark and a silent error detector — all self-checking):
ALU (LCG forward and backward via modular inverse multiplier), memory bandwidth,
SDRAM integrity, cache, and a combined run with a "weakest link" verdict.

`patcher.ldd` is **not included** in the package: this driver has kernel-side capabilities
that a self-signed certificate does not grant, and with it the whole package would fail to install.
It must already be on the device (`E:\sys\bin\patcher.ldd` + SHA-1 in
`C:\sys\hash\patcher.ldd`); `build.cmd run ... "patcher ..."` places it there.
If the driver is absent, the application explicitly says so and leaves only the tests available.

Status: SIS built and signed (`group\SymbianOverclock_sha1.sis`, version 1.4.0).
Updating an already installed application does not require using the installer: the daemon
places a new `ocgui.exe` directly on top of `C:\sys\bin\ocgui.exe`, because executables
on C: are not subject to removable media hash checks. This is exactly how build 1.4 was deployed:

```
powershell -File %DAEMON%\tools\deploy_net.ps1 -Ip <ip> ^
  -PutFile %EPOCROOT%Epoc32\release\gcce\urel\ocgui.exe -Remote "C:\sys\bin\ocgui.exe"
```

Verified on the device only to this extent: the replaced application launches (`APP
0xE0001011` -> process in list), executes `Prepare()`, and cleanly terminates
upon `ENDAPP`, meaning it removes the kernel patch behind itself. **The UI itself has not yet been seen
with eyes** — the phone was under auto-lock, and the screenshot showed the screensaver,
not the application.

## "Apply and Hold" Mode

```
tools\build.cmd run 192.168.1.5 "patcher hold 96"    raise and HOLD
tools\build.cmd run 192.168.1.5 "patcher hold 86"    restore stock
```

Circumvents the transition problem (findings §20): relock is performed headlessly when
the display is inactive, and subsequently the device is simply used at the new frequency —
it is specifically the change window that is dangerous, not the stationary mode.

**Frequency remains, kernel patch does NOT.** The patch is dangerous to foreign channel
calls, so it is always removed; after process exit, nothing of ours remains in the kernel.
A reboot resets it (nothing is written to flash), or `hold 86`.

Measurement upon applying mult 96 (2026-08-08, on the device):

```
stock  m86: DPLL=330240 CORE=660480 MPU=330240 BUS=165120 kHz
target m96: DPLL=368640 CORE=737280 MPU=368640 BUS=184320 kHz
NOP 323 -> 351 MHz (+8.6%)   ALU 17.14 -> 18.60 Mitr/s (+8.5%)
MEM 513 -> 572 MB/s (+11.5%)  SDRAM 1 MB: 0 bad words
```

**Open question:** the bus scaled exactly as calculated (+11.5% vs.
theoretical +11.63%), while both processor instruments yielded only +8.5%. The discrepancy
is unresolved; check by reading `CM_CLKSEL_MPU` and `CM_CLKSEL2_PLL` under
load — possibly the governor moves the MPU divider upward at higher CORE.

## Work Cycle

```
tools\build.cmd                             build only
tools\build.cmd run 192.168.1.5             build -> upload -> run -> fetch log
tools\build.cmd run 192.168.1.5 "power 7"   same with arguments
```

Log lands in `out/ocprobe.log`. Device IP is `192.168.1.5` (cached by the daemon in
`../SymbianDeployDaemon/tools/.daemon_ip`; without `-Ip` the client scans the subnet).

Analysis of ROM modules:

```
tools\romtool.ps1 -Module power.dll -Disasm -Imports
```

Places `.asm` in `out/asm/`, resolves import stubs across all ROM modules.

## Safety Rules Developed in Practice

- The daemon is **single-threaded** and waits for the launched process to exit. A probe hang
  blocks the entire delivery channel. The device remains alive in this state — TCP handshake
  succeeds, but there is no response.
- Driver state in the kernel survives daemon restart. It is cleared only by
  **rebooting the device**.
- Uncancellable requests are real: `TestPowerAPI` index 6 (resource 65) never
  completes, and the driver has no `Cancel`.
- Keep `TRequestStatus` for a driver **static**, not on the stack: late
  completion otherwise writes into a foreign frame.
- Timeout on `Req()` is insufficient — `LoadLogicalDevice`
  or `DoCreate` can block as well, and they are synchronous and have no timeout.
- Anything that can hang must be enabled **only by an explicit argument**. A normal run
  must remain one after which the device is guaranteed to respond.
- No writes to flash. Everything is runtime; a reboot restores state to how it was.

## Measurement Instrument

Absolute frequency — a chain of 256 independent `add` instructions (ARM1136 is single-issue,
one cycle per instruction), `ALWAYS_BUILD_AS_ARM` in `.mmp`. Yields 322–325 MHz under
assumed 258 cycles per iteration; true value is 330 MHz (actual cycles
~263). For cross-check there is a second, independent channel: `DoControl(1)` on
`dvfs_interface` returns percentage of nominal directly from PRCM registers, in steps of
3.3 MHz. **After a frequency change, it will serve as the arbiter** — unlike
`HAL::ECPUSpeed`, which on this device simply lies (stable 206438 kHz).

Baseline: ALU 17.05 Mitr/s, memory ~510 MB/s.

## Tooling Gotchas

- PowerShell parses an 8-character hex literal as `Int32` and silently makes it
  negative. All constants — via `[Convert]::ToUInt32($h,16)`.
- ROM images have `TRomImageHeader` (120 bytes), not `E32ImageHeader`.
- `dumpsis -x` unpacks SIS; path **without Cyrillic**, otherwise "cannot open".
- `petran`/`elftran` accept only PE/ELF on input — they cannot decompress an already
  built E32.
- Symbian-deflate is **not compatible with zlib**; `DeflateStream` cannot handle it. Decompress with
  `tools/syminflate.js` (Node) — a verbatim port of Symbian `TInflater`/`Huffman`
  from elf2e32. In compressed E32, the uncompressed portion = `oh->TotalSize()` ==
  `iCodeOffset` (@0x64), the deflate stream starts right there; body length =
  `iUncompressedSize` (@0x7C). The CLI takes both itself: `node tools/syminflate.js
  <in> <out.img>` writes the complete restored image (header + body).
- Implement the port in native Node, not PowerShell: unsigned 32-bit shifts (`reverse`,
  `huff>>17`) in PS would fall into the same Int32 trap. In JS — via `>>>`.
- Trap in porting `HuffmanSubTree`: in C++, `aLevel` (`TUint32**`) is passed **by
  value**, so the level must be a by-value parameter rather than shared state,
  otherwise the first recursive call corrupts the level for the second (symptom: trees
  are constructed, but decode garbage — output cuts short by a few bytes).
- **A kernel patch must have a rollback, and the rollback must be verified.**
  A READ/EXEC patch of the `DoControl(1)` body lives in ROM-shadow and **survives our
  process**. The `dvfs_interface` channel is opened not only by our code (`mmrmserver.dll`,
  `fmradiotunercontrol.dll` — findings §15), so a leftover patch sooner or later
  reboots the device. Three related rules, each bought with a reboot:
  1. original words — in `KDvfsOriginal` (`ocengine.cpp`), always restore;
  2. **overwriting bytes IS NOT ENOUGH** — this is the main gotcha of the entire work
     (findings §19). The patcher does not execute `Cache::IMB`, so the patched function
     remains resident in I-cache and continues executing. The patch must be removed with
     `DoControl(100, &addr, &len)` — this is `FreeShadowPage`: it discards the RAM copy
     and restores mapping to ROM along with cache maintenance.
     `EndPatchSession()` performs both steps in the proper order;
  3. **verify** rollback, **specifically after unshadow**: `DoControl(1, 0)` on stock
     code returns percentage of nominal (50 or 100), while on the patch — exactly 0.
     A non-zero value is the proof. Reading restored bytes directly is
     impossible: the read primitive is those exact same bytes.
  Keep the patch applied only for the duration of a single operation, not for the entire session.
- **Network availability is not proof that the device did not reboot.**
  It has time to come up between checks. Direct proof — `User::NTickCount()`
  (cleared only by a full reboot); the probe prints `UPTIME:` as the first line.
  At the same time, this distinguishes a kernel reboot from a Symbian shell restart.
- `signsis` **without `-s`** returns 0 and produces an UNSIGNED file. The only reliable
  check is `signsis -o`; output must contain issuer.
- Symbian 9.2 cannot parse a SHA-256 certificate and blames the file for it
  ("file is corrupted"). RSA + SHA-1 is mandatory.
- `.pkg` must be **ASCII and CRLF**: `makesis` silently corrupts UTF-8 and LF.
- `EPOCROOT` without drive letter, combine with `D:`. Perl must be < 5.22.
- Cyrillic characters in `.cmd` break cmd — keep comments in batch files ASCII.

## 2026-09-12: E90 Verified on Hardware, Apostrophe Fixed (2.0.2)

E90 connected via Bluetooth COM9, and three things were settled by measurement rather than
reasoning (findings §44).

The Usbc branch in `CaptureOriginal` works: the original body of `DoControl(69)` on the E90
is completely different than on RM-320, so the old restoration constant would have been garbage in the kernel.

The complaint that "overclock resets after exit" on the E90 was explained completely: an 80,936-byte
build was sitting there, older than the `KEEP ON EXIT` row. The frequency itself, as
demonstrated by two separate processes after five minutes of idle, survives process termination,
and the system does not touch it. Build 2.0.2 was uploaded to the device.

New probe command `font` rasterizes a character and reports whether a glyph exists for it.
On the E90, Ukrainian letters U+0456, U+0457, U+0454, U+0491 exist, while the modifier apostrophe
`U+02BC` is absent across all typefaces — it was replaced with ASCII in `src/strings.json`.

Open: the same `ocprobe font` on N95 8GB (daemon was not running on it),
and confirmation from the user that on the fresh build the overclock holds after
exit.

## 2026-09-12 (Later): Automatic Text Wrapping and Help in All Languages (2.0.3)

There are no longer manual line breaks in the source. `src/strings.json` holds help
as paragraphs, and `WrapText` in `src/ocgui.cpp` wraps them on spaces to match the current window
width; the same function with `aGc == NULL` counts lines, and help scrolling
relies on this count. List rows deliberately remained without wrapping:
scrolling and cursor position rely on fixed row height.

Help has been translated into all 28 build languages (four `agy` subagents, six to seven
languages each, verification of structure and characters in a separate pass). The binary grew from
102 to 221 KB — this is almost entirely string tables.

Both devices updated to 2.0.3: N95 via WiFi, E90 via Bluetooth, both in
`C:\sys\bin`.

## 2026-09-12 (Evening): Instruments Fixed, N93/N93i, New Help (2.1.0)

Three remarks from the owner settled, each by measurement rather than reasoning (findings §46).

The memory bandwidth test indeed overestimated, and by exactly two times: it calculated bytes on the
assumption of a 64-byte cache line, whereas a step sweep on hardware showed
a plateau starting at 32. Now every word is read, so the assumption disappeared from
the calculation. True numbers at 165 MHz bus: read 199 MB/s, write 403 MB/s, cache
541 MB/s.

The cache test was measuring arithmetic rather than cache — every word incurred multiplication,
addition, and verification. Now it is the same loop as in the memory test, only the working
set fits into L1, and the two numbers have become comparable.

Operation cost is now measured rather than guessed. `ocprobe alubench` yields
1.04 cycles per dependent addition, 4.94 per multiplication, 5.96 per mul+add pair. Hence
the explanation of the old ALU test figure: 17.34 Mitr/s against a ceiling around 55 — this is
loop overhead and constant reloading, not arithmetic. Two direct indicators were added
to the application: CLOCK and MUL+ADD.

Profiles for N93 (RM-55) and N93i (RM-156) were added in on-device address search mode.
Offline constants for them are impossible in principle: their extracted XIP is a fragment,
and file offset does not equal address (for E90 the difference was measured: 0x2adb0). The live
XIP scanner was verified against N95 and E90, where the correct answers are known independently —
both pairs matched.

Help was rewritten from scratch: formal exposition, explanation of terms (clock
frequency, reference frequency, PLL, multiplier, divider), frequency formula, purpose
of each setting and each test. Everything related to 3D and internal research details
was removed. Translated into all 28 languages.

Open: no one has run N93/N93i on hardware; GUI tests after the fix
have not yet been seen on the device with eyes.
