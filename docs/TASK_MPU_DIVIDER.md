# Task: MPU divider and full clock layout without UI

> **STATUS 2026-08-15: §5 closed with three runs, §6 fixed, the main
> question remains for the client.**
>
> The layout **holds** — both `CM_CLKSEL_MPU` and `CM_CLKSEL1_CORE`, without
> a resident patch, under load, across OPP transitions. Nothing needs to be
> held.
>
> §2 describes the mechanism incorrectly twice. The patch is a transport, not a latch, so
> its residency has nothing to do with the contents of `CM_CLKSEL1_CORE`. And divider
> compensation did not "live for seconds" — it was **not written at all**: the L3 validator
> in `power.dll` @`0x800AC424` rejects 0, 3, 5, 9, 18, while `CompensatedCore()`
> selected specifically `l3div=5`, and the device ignores such a write completely.
>
> The command `patcher clocks mult= mpu= l3= dss= sec= [child=ps1] [args:...]` exists and
> is verified on configuration D. All that remains is to run `ps1stage1.exe` in its window.
>
> Full analysis — `findings.md` §26, response to the client —
> `../PS1Emulator/plans/reports/answer-mpu-divider-2026-08-15.md`.

Client — neighboring project `../PS1Emulator` (port of PCSX-ReARMed for S60v3). It
needs to separate the frame budget into CPU and memory parts, and doing this
is only possible from here: kernel access to PRCM is required, which exists only in this
project.

Device is the same — Nokia N95 8GB (RM-320), Symbian 9.2, OMAP 2420.

Rationale from the client's side — `../PS1Emulator/plans/reports/pipeline-soc-2026-08-15.md`,
sections 8.1 and 12.2. The logs I reference below reside in
`../PS1Emulator/plans/runs/oc*.log`.

---

## 1. The question to be answered

The emulator spends 27.2 ms per frame at stock 330 MHz. It is unknown what fraction
of this time is ARM1136 execution and what is waiting on memory. Where to invest
the next month directly depends on the answer: into dynarec codegen and rasterizer, or
into data formats, alignment, and traffic volume to SDRAM.

Measurements on 2026-08-08 and 2026-08-15 did not answer this question because in both cases
the core frequency and bus frequency moved together. When both domains change by
the exact same +24.4%, the hypotheses "everything is bottlenecked by CPU" and "everything is bottlenecked by
memory" predict an identical result, so a measurement does not distinguish between them. This is not
a defect of the instrument, this is a property of the experiment.

Distinguishing between them is possible only with a configuration in which **one domain changed while the other
did not**. The MPU divider provides such a configuration, and this part of PRCM in the project has already
been figured out — `findings.md` §6 and §21, `state.md` regarding the `MPUDIV` line in v1.2.

## 2. Why the existing `patcher hold` does not provide this

The idea was that `hold` already writes compensated dividers, so using it
would be sufficient. It indeed writes them, but **they do not hold**.

The run `patcher hold 107` (`oc107_apply.log:15-20`) read the stock state
correctly and wrote the compensation:

```
engine ready: SRAM @cb202000, stock mult 86
  stock: m=86 DPLL=330240 CORE=660480 MPU=330240 BUS=165120 DSS1=110080 (l3div=4 dssdiv=6)
  CLKSEL1_CORE 04360644 -> 04360745
  compensated: m=107 CORE=821760 BUS=164352 DSS1=117394 (l3div=5 dssdiv=7)
```

The next read of the same register, roughly ten seconds later
(`oc_engine_after_apply.log:22`), yields `04360644` — the initial stock word.
Compensation is removed.

The cause is most likely in the exit mechanism itself. `HoldOverclock` calls
`KeepOnExit(ETrue)` and writes "Kernel patch removed; frequency stays" in the log
(`ocprobe.cpp:801-806`). The DPLL multiplier indeed stays — the emulator run of
133 seconds ran at 411 MHz from start to finish. But the dividers in
`CM_CLKSEL1_CORE` are held specifically by the relock blob, and as soon as the patch is removed, the stock setter
rewrites the register from its tables at the very first DVFS transition. That is, the multiplier
survives patch removal, while the dividers do not.

Practical consequence for any client: **the divider configuration lives exactly
as long as the resident patch**. Hence the measured process must run
inside the window while `ocprobe` holds the patch, rather than after its exit.

## 3. What specifically is requested

A headless command that sets the **full clock layout**, holds the patch, launches
a child process, and restores stock state on exit. The mechanism for this in the project
already exists: `ProbeDspSet` in mode `ESoakRun` does exactly this for the DSP divider
(`ocprobe.cpp:668-681`), including a time ceiling for the child so that a hung child
does not hold the non-stock state (`RunChild`, `ocprobe.cpp:552-555`). We ask to
generalize this mode from a single DSP divider to four fields: DPLL multiplier, MPU
divider, L3 divider, and DSS1 divider.

Approximately like this, form at your discretion:

```
ocprobe.exe patcher clocks mult=86 mpu=4 l3=4 dss=6 sec=200 child
```

The four configurations needed by the client are given in §4. All of them are
**reductions** in frequency relative to stock, so the stability question does not arise
here at all: none of them pushes the device outside the regime in which it operates daily.

Three implementation details visible from the client's side.

The child process is currently hardcoded: `E:\sys\bin\dsptest.exe` with arguments
`baseonlydcdopnobench` (`ocprobe.cpp:671-672`). Another is needed — our
`E:\sys\bin\ps1stage1.exe`. A second hardcoded option is sufficient, selection
can be made via a keyword; full parameterization is not required.

The child's arguments are long. The client's run looks like this:

```
gui play nosnd frames6100 dsa light blend imggt.pbp
```

This is 50 characters, and together with the command's own flags it does not fit into
`TBuf<64> cmd`, into which `ocprobe` reads the command line (`ocprobe.cpp:1189` and
`:1201`). The buffer must be enlarged, or child arguments must be read from a file on the card.

The child time ceiling must reach up to 200 seconds: the benchmark run of the reference track is 6099
frames, which is from 120 to 170 seconds depending on configuration. The current range of
`soak` (0..900) already allows this, but verify that `RunChild` does not have its own
lower limit.

## 4. Configurations needed

Stock state — multiplier 86, `mpu_div` 2, `l3div` 4, `dssdiv` 6, yielding core
330.24 MHz, bus 165.12, and pixel clock 110.08.

The primary pair is A and D. They have the same bus and 2x different core, so the ratio
of frame time between them directly gives the CPU portion of the budget. If the frame doubles in
duration, the work is entirely CPU-bound; if it increases by 1.5x, one third of the budget is
waiting on memory.

| # | mult | mpu | l3 | dss | MPU, MHz | bus, MHz | DSS1, MHz | what it isolates |
|---|---|---|---|---|---|---|---|---|
| A | 86 | 2 | 4 | 6 | 330.24 | 165.12 | 110.08 | baseline, stock |
| D | 86 | **4** | 4 | 6 | **165.12** | 165.12 | 110.08 | 2x slower core with unchanged bus |
| C | **43** | **1** | 4 | **3** | 330.24 | **82.56** | 110.08 | same core with 2x slower bus |
| B | **43** | **1** | **2** | **3** | 330.24 | 165.12 | 110.08 | same core and bus, 2x lower CORE |

Configurations C and B deserve special attention because they rely on an observation
already in `state.md`: with `mpu_div=1` the same core frequency is achieved with 2x
lower CORE. Multiplier 43 yields DPLL 165.12 and CORE 330.24, from which divider 1 restores
the core to exactly stock 330.24 MHz. Next, the DSS1 divider, reduced from 6 to 3, restores
the display to exactly stock 110.08 MHz — compensation here is exact, without remainder, unlike
the case with multiplier 107. And the L3 divider remains a free parameter: 4 yields
bus 82.56 (this is C), 2 yields stock 165.12 (this is B).

The pair C versus A measures the same thing as D versus A, but from the opposite side, so it
serves as a cross-check. The pair B versus A answers this project's own question,
already formulated in `state.md`: whether the limit is ARM1136 itself, or CORE as
the highest internal frequency.

If not doing everything, the minimum is A and D.

## 5. Zero-cost step worth doing first

Before writing any code, it is worth checking whether the governor rewrites
`CM_CLKSEL_MPU` just as it rewrites `CM_CLKSEL1_CORE`. The existing logs do not
say: in them `CM_CLKSEL_MPU = 00000002` both before and after
(`oc_engine_after_apply.log:19`, `oc_engine_final.log:19`), but we have never
written to this register, so the match proves nothing.

The check costs one run: write divider 4, wait thirty seconds under
load, read back. If the value holds without a resident patch,
the entire construct with a child process is unnecessary and the task reduces to a single write
command. If it does not hold — `child` is needed, as described above.

## 6. Side finding: `CompensatedCore()` ratchets display divider

This is no longer a task, but a bug, and it reproduces in the logs completely.

`COcEngine::Prepare()` captures the baseline state from live registers
(`ocengine.cpp:816-825`), meaning `iStockCore` and `iStockClocks` are whatever was in
the device at the moment of process launch, rather than what is stock for this SoC. As long as
the device is stock, there is no difference. As soon as the device is already overclocked — the baseline point
drifts.

Then `CompensatedCore()` selects the DSS1 divider so as to hit
`iStockClocks.Dss1Khz()` (`ocengine.cpp:907`), that is, the frequency that was just
read as "stock". Here is the full cycle from the logs.

First `hold 107` set multiplier 107, and the system removed the divider compensation,
as described in §2. The device was left with `dssdiv=6` at CORE 821.76, that is with a pixel
clock of 136.96 MHz instead of 110.08.

Then `hold 86` (`oc86_restore.log:15-20`) read this state and took it as
the baseline:

```
engine ready: stock mult 107
  stock: m=107 CORE=821760 BUS=205440 DSS1=136960 (l3div=4 dssdiv=6)
  CLKSEL1_CORE 04360644 -> 04360544
  compensated: m=86 CORE=660480 BUS=165120 DSS1=132096 (l3div=4 dssdiv=5)
  vs stock: DSS1 -3.5%  BUS -19.6%  (0 = display untouched)
```

That is, it dutifully preserved for the display the frequency it saw — 136.96 MHz — and for
this it reduced the divider from 6 to 5. The line "vs stock: DSS1 -3.5%" at the same time
reports that the display is almost untouched, whereas in reality it was just pinned
at 132.1 MHz instead of stock 110.08, which is 20% higher.

The final state of the device confirms this (`oc_engine_final.log:15-16,22`): multiplier
is 86 again, stock, while `CLKSEL1_CORE = 04360544`, meaning `dssdiv=5`. And the next
launch will take this frequency as the baseline — hence the "ratchet": each
"raise — lower" cycle shifts the divider by another step and never returns it.

The intermediate run `oc107_apply2.log:16` shows this exact shift halfway:
the baseline read there was `DSS1=132096 (dssdiv=5)`.

How harmful this is is a separate question. The device with `dssdiv=5` works normally and
does not break rendering, so for a single session this is rather cosmetic. But it quietly
spoils the very thing for which compensation exists, and does so in a direction that is
impossible to notice from the logs, because the logs report "display untouched".

The smallest fix that addresses this: keep the baseline point **constant**, rather than reading it
each time from live registers. Since the project is single-device, it is sufficient to put the stock
word and stock multiplier into constants next to other register constants in
`ocengine.h`, with a comment that these are default values from ROM:

```
CM_CLKSEL1_CORE = 0x04360644   l3div=4 dssdiv=6  ->  BUS 165.12, DSS1 110.08
CM_CLKSEL_MPU   = 0x00000002   mpu_div=2         ->  MPU 330.24
CM_CLKSEL1_PLL  mult=86
```

Then `CompensatedCore()` will target 110.08 MHz always, regardless of the state in
which it found the device. An alternative is remembering the first observed state in
a file on the card, but for a single device this is more complicated without benefit.

Separately, it is worth loudly logging the case where "live multiplier does not equal stock at
start": currently it silently becomes the new normal.

## 7. What does not need to be done

No UI is needed — the `MPUDIV` line in the GUI already exists (v1.2), and for a human this is
enough. We specifically ask for a headless route because measurement is performed over WiFi via the deployment
daemon, without touching the device.

No frequency increase is needed. All four configurations are reductions, and that is why
they are safe. An MPU divider of 1 at stock multiplier would give 660 MHz, which is twice as high
as the established limit of 426 MHz; no such configuration is in §4 and it is not needed.

No need to touch SDRC timings. In configurations C and D the bus is either unchanged or
lower than stock, so the timings remain with headroom.

## 8. Acceptance criterion

The client considers the task complete when a single command from the PC side launches an emulator
run in the specified clock layout and restores the device to stock state, and the log
contains reads of all four registers **after** the child finishes — to show
that the layout held the entire time, rather than just the first few seconds.

Minimum useful result — two configurations, A and D. The rest — if desired.

## 9. Option requiring no code at all

If modifying code is undesirable, the same measurement can be done manually. The
`MPUDIV` line in the GUI sets the divider, and the patch remains resident while the application
is running. Thus it is sufficient to set the required divider on the device, leave the application
in the background, and launch the emulator over WiFi with the usual deploy command. This yields
configuration D in a single session near the device.

Two caveats. One needs to verify that the GUI in the background actually holds the patch rather than removing
it upon losing focus. And this route yields only D — configurations B and C require
simultaneously changing the multiplier and three dividers, which from the screen takes long and is easy
to get wrong.
