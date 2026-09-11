# SymbianOverclock

Runtime CPU and bus overclocking for Symbian 9.2 phones built on the
TI OMAP 2420. Nothing is written to flash: every change lives in PRCM
registers only, and a reboot always returns the phone to stock clocks.

## 1. What it does

The application changes the DPLL multiplier (`CM_CLKSEL1_PLL`) and the clock
dividers for the memory bus, the display and the DSP (`CM_CLKSEL1_CORE`,
`CM_CLKSEL_MPU`, `CM_CLKSEL_DSP`) while the phone is running. It also carries
its own benchmarks, because the clock a Symbian phone reports about itself
cannot be trusted: `HAL::ECPUSpeed` on the N95 8GB returns 206438 kHz while the
core actually runs at 330 MHz. Every result below was measured by doing work
and timing it, never by reading a number the system printed.

## 2. Supported devices

| Phone | Platform | SoC | Stock | Measured ceiling |
|---|---|---|---|---|
| Nokia N95 8GB (RM-320) | S60 3rd FP1, Symbian 9.2 | OMAP 2420 | 330 MHz, bus 165 MHz | 491 MHz core |
| Nokia E90 (RA-6) | S60 3rd FP1, Symbian 9.2 | OMAP 2420 | 330 MHz, bus 110 MHz | 503 MHz core, bus 201 MHz |

Both profiles are proven on the author's own hardware. The application refuses
to touch the kernel on any phone it does not have a profile for, and it also
refuses when the firmware build does not match the one the addresses were
derived from: the same model on a different build would put the patch on the
wrong code. Adding a phone is described in `docs/PORTING.md`.

Two further phones ship as TEST profiles: the Nokia N95 classic (RM-159) and
the N82 (RM-313). Their addresses were derived from ROM images and verified as
far as an image allows, but never on hardware, so they are never selected
automatically - there is a menu row to pick one by hand, after two warnings.
What could not be derived offline is discovered on the phone instead and
checked against the ROM before anything is written. The details, including the
one risk that cannot be removed, are in `docs/findings.md` section 41.

The N93 and N93i are groundwork only: different Symbian version, no DVFS, and
their images are packed the way the E90 image was.

## 3. Results

Measured on the E90 with the built-in benchmarks and a 3D test on the large
screen:

```
                        bus        core       memory     3D
stock                 110 MHz     330 MHz    339 MB/s   16.33 fps
bus divider 6 -> 4    165 MHz     330 MHz    509 MB/s   24.55 fps
plus multiplier 105   201 MHz     403 MHz    569 MB/s   28.20 fps
```

The important part is not the top number but where the limit sits. Raising the
bus alone, with the core untouched, took 3D from 16.33 to 24.55 fps: on this
chip 3D is limited by memory, not by the CPU. The core itself tops out near
500 MHz on both phones without a voltage change, and the multiplier is
effectively exhausted there.

The highest core clock reached on the E90 is a separate measurement:
multiplier 131, that is 503 MHz, at 534 MB/s of memory bandwidth.

Repeat runs of the same configuration differ by about 10% on the memory
benchmark and 7% on frames, so a single run cannot resolve anything smaller
than that. Both numbers in the second row come from repeats of one state.

## 4. Languages

Fifteen: English, Ukrainian, Russian, Polish, German, French, Spanish, Italian,
Portuguese, Czech, Dutch, Swedish, Turkish, Romanian and Hungarian. The
language follows the phone setting. A language may be translated partially and
a missing string falls back to English, which is why the menu and the warnings
are translated widely while the built-in help stays in the two languages it was
written in: it explains what is dangerous, and that text should be right rather
than numerous.

Translations live in `src/strings.json`; `node tools/gen_strings.js` rebuilds
the tables.

## 5. Requirements

The phone must already be open for homebrew. Concretely it needs RomPatcher+
installed with its `patcher.ldd` driver reachable, because that driver is the
write primitive this application uses; on the E90 the `Open4all` patch is also
needed. The application is self-signed and installs only where the Install
Server accepts self-signed packages.

Nothing about this touches another person's device, service or network. It is
a homebrew modification of hardware you own.

## 6. Using it

`MULT` sets the DPLL multiplier and the header shows what the setting would
give before anything is applied. `APPLY SETTING` only arms the change;
`CONFIRM` performs it, stepping one multiplier at a time and stopping at the
first relock the hardware refuses. `RESTORE STOCK` puts the multiplier back.
The full instructions are inside the application under `HELP`, in English and
Ukrainian, so they are available on the phone itself with no network.

`BUSDIV` sets the memory bus divider, where a smaller divider means a faster
bus. `BUS GUARD` is on by default and refuses every request that would raise
the bus, because returning the bus to stock from a lowered state rebooted the
phone four times out of four. That is the only case ever observed to be fatal:
raising the bus above stock, which is where most of the gain is, has been
measured on the E90 and works. The guard cannot tell the two apart, so turning
it off is an explicit choice, and a reboot undoes whatever it leads to.

## 7. Known limitations

The overclock does not survive a reboot, by design. Voltage is never changed,
so the core ceiling near 500 MHz is a hard one. The SDRAM refresh interval is
read and reported but never compensated, so lowering the bus stretches the
refresh window in real time; raising the bus shortens it and is safe in that
respect. The cache benchmark currently does much more arithmetic per byte than
the memory benchmark, so its number reflects the ALU and must not be compared
with the memory result. Battery drain and heat at 500 MHz have not been
measured.

## 8. Building

Requires the S60 3rd Edition FP1 SDK, the CSL ARM toolchain with GCCE, and
Perl 5.20 or older. `EPOCROOT` must be set without the drive letter.

The build scripts and `group/ocgui.pkg` carry absolute paths from the author's
machine and will need editing to build elsewhere; a ready signed package is
attached to the GitHub release for anyone who only wants to install it.

```
tools\build.cmd                 build the probe and the application
tools\build_sis.cmd             build, package and sign SymbianOverclock_sha1.sis
node tools/gen_strings.js    regenerate the translation tables from src/strings.json
```

The package descriptor `group/ocgui.pkg` must stay ASCII with CRLF line
endings; `makesis` fails with a misleading "No vendor names found" otherwise.
Signing uses RSA-1024 with SHA-1, because Symbian 9.2 cannot parse SHA-256
certificates.

## 9. Documentation

`docs/findings.md` is the technical record: every claim in it is either
confirmed on hardware or derived from ROM code, and guesses are marked as
guesses. `docs/state.md` is the current state of the work. `docs/PORTING.md`
explains how to add a phone. `docs/WALKTHROUGH.md` is the narrative account of
how the overclock was found.

## 10. License

MIT, see `LICENSE`.
