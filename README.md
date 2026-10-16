<p align="center">
  <img src="docs/icon.png" width="128" height="128" alt="SymbianOverclock icon">
</p>

# SymbianOverclock

<!-- Video demonstration: to be added here. -->

SymbianOverclock raises the processor and memory bus frequency of Nokia phones
built on the TI OMAP 2420 chip: the Nokia N95 8GB and the Nokia E90. It works
while the phone is running and never modifies the firmware: a new frequency
exists only in the processor's clock registers. Restarting the phone therefore
returns it to its factory frequency, unless you have deliberately switched on
autostart.

The first part of this document is written for people who want to install the
application and use it. The second part, starting at
[How it works](#part-2-how-it-works), is written for people who want to know
what happens inside, check the measurements, or build the application
themselves.

# Part 1. Using the application

## What it gives you

On both supported phones the processor runs at 330 MHz from the factory. With
this application it can run at up to about 500 MHz, and the memory bus can be
raised as well. The practical effect is measured, not estimated: on the E90 the
SPmark 3D test on the large inner screen ran at 16.4 frames per second at
factory settings and at 28.0 frames per second with the author's everyday
setting, which raises both the bus and the processor: 71% faster. On the N95
8GB the same test went from 66.2 to 75.2 frames per second, 14% faster. The
interface, games and emulators become noticeably quicker.

The application offers the following:

- **Processor overclocking** in steps of 3.84 MHz, from the factory 330 MHz
  up to the limit of the particular phone.
- **Memory bus overclocking**, which on this chip gives the largest gain in
  games and graphics. The graphics core is clocked from the same bus, so a
  faster bus speeds up both the memory and the GPU, and 3D follows the bus
  more than the processor.
- **Built-in measurements.** The application measures the real processor
  frequency, the memory speed and the cache speed by timing actual work. The
  frequency the phone reports about itself is wrong on this hardware, so the
  application never relies on it.
- **Built-in stability tests** for the processor, the memory and the memory
  bus. They detect calculation errors and corrupted memory, and they name the
  part of the chip that failed first.
- **Optional autostart.** A setting that has been tested can be applied
  automatically after every restart. It is switched off after installation and
  protects itself against a setting that turns out to be unstable.
- **Safety by design.** The firmware is never modified; the application stores
  only its own small settings files. Without autostart, a restart always brings
  back the factory frequency, and the one operation known to crash the phone is
  blocked.
- **Underclocking** is possible too, down to 165 MHz.
- **33 interface languages**, chosen automatically from the phone's language
  setting or by hand in the `LANGUAGE` row, and a full built-in help text in
  every one of them.
- **Protection against wrong phones.** The application changes nothing on a
  phone or firmware version it does not recognise. On such a phone it can still
  be used to measure speed.

## Supported phones

| Phone | Firmware version | Factory clock | Highest measured clock |
|---|---|---|---|
| Nokia N95 8GB (RM-320) | 35.0.001 or 31.0.015 | 330 MHz | 491 MHz |
| Nokia E90 (RA-6) | 400.34.93 | 330 MHz | 503 MHz |

The firmware version can be checked by typing `*#0000#` on the phone. The
highest clock differs slightly from phone to phone, so the figures above show
what two particular phones reached, not a guarantee.

Four more phones use the same chip: the Nokia N82 (RM-313), the original
Nokia N95 (RM-159), the Nokia N93 (RM-55) and the Nokia N93i (RM-156). The
overclock has not been tested on any of them, but in theory it should work,
and the application includes a test profile for each. A test profile is never
selected automatically; it can only be chosen by hand in the `TEST PROFILE`
row, after two warnings, on a freshly restarted phone. Before writing anything,
the application checks that the firmware code it is about to modify is the
code it expects, and stops if it is not.

The chances differ between these phones. For the N82 and the original N95 the
addresses were derived from firmware images and match the N95 8GB almost
exactly. For the N93 and N93i they could not be derived in advance, so the
application searches for them on the phone itself; whether it finds them is
not known yet. Owners of any of these phones who try it are asked to report
the result (see [`docs/PORTING.md`](docs/PORTING.md)).

The North American versions, the Nokia N95-3 (RM-160) and the Nokia N95 8GB
NAM (RM-421), are not supported yet: the application has no profile for them
and will report an unknown device. A test profile can still be tried by hand.
The check of the firmware code described above then decides whether anything is
written; if the code differs from what the profile expects, the application
stops without touching the processor.

## Before you install

The application needs direct access to the processor's clock registers, which
an ordinary Symbian application does not have. Two things are therefore
required:

1. **The phone must accept the installation.** The package is self-signed but
   asks for system capabilities (`PowerMgmt`, `WriteDeviceData`,
   `ReadDeviceData`, `ProtServ`, `CommDD`) that a self-signed package does not
   normally receive. The phone has to be prepared for homebrew software
   ("hacked") in the way usual for Symbian 9, so that the installer accepts
   such packages; on most phones this is the **Install Server** patch of
   RomPatcher+.
2. **RomPatcher+ must be installed.** Its driver `patcher.ldd` is what the
   application uses to reach the clock registers. The driver belongs to
   RomPatcher+ and is not included in this package.

As far as the author can tell, the patches do not have to stay active for the
overclock itself once the application is installed, but this has not been
checked separately. If the application reports `NOT READY`, activate the
patches in RomPatcher+ and start it again.

If `patcher.ldd` is missing, the status line shows `NOT READY`, the log at the
bottom of the screen says `patcher.ldd missing or driver refused`, and only the
measurements are available.

## Installation

1. Download `SymbianOverclock_sha1.sis` from the
   [Releases](../../releases) page.
2. Copy it to the phone in any convenient way: USB cable, Bluetooth or memory
   card.
3. Open the file on the phone and confirm the installation. The application is
   always installed to the phone memory (`C:`).
4. The application appears in the menu as **Overclock** in the grid view, or
   as **Symbian Overclock** in the list view and the application manager.

To remove it, use the phone's application manager, as with any other
application. Removal also removes the autostart component.

## Your first overclock

On the first launch, and again after each update, the application opens its
built-in help. The up and down keys scroll it, and any other key closes it. The
help can be opened again at any time from the `HELP / how to use` row.

The main screen is a list of rows. The up and down keys move between rows, the
left and right keys change a value, and the OK key performs the action of a
row. The header at the top always shows what frequencies the current selection
will produce before anything is applied. The right soft key closes the
application.

1. Open the application and close the help.
2. Go to the `MULT` row (the multiplier) and press right a few times. The
   factory value is 86; each step adds 3.84 MHz. The digit keys 1 to 9 jump
   straight to 5%, 10% and so on up to 45% above the factory value.
3. Select `APPLY SETTING`. This only remembers the choice; the hardware is not
   changed yet.
4. Select the next row, which now reads `CONFIRM OVERCLOCK` (before step 3 it
   reads `CONFIRM (use APPLY SETTING first)`). Now the frequency is changed,
   one step at a time.
5. Run `TEST combined (all)`. If it reports that all subsystems are clean, the
   phone is stable at this frequency.

The two separate steps, apply and confirm, exist so that an accidental key
press cannot change the frequency.

`RESTORE STOCK` returns the multiplier and the dividers to their factory
values, with the single exception described in
[Questions and problems](#questions-and-problems). `KEEP ON EXIT` decides
whether the frequency stays after the application is closed; it is on by
default. In either case, a restart returns the factory frequency unless
autostart is on.

## Finding the limit of your phone

Every phone has its own limit, even among phones of the same model, so a
stable setting has to be found on each phone by its owner. The procedure is
simple. Raise the multiplier by a few steps, apply, confirm and run the tests.
If the tests are clean, raise it further and repeat. When the phone restarts by
itself or a test reports errors, the previous value is the highest usable one.
A restart does no damage: the phone simply comes back at the factory frequency.

The built-in tests are not enough on their own. They run for a short time and
check the processor and memory, while a real instability may appear only after
several minutes of a game, a video or a phone call. Once the tests are clean,
use the phone for a while in its usual tasks and in the games you play. For
everyday use, choose a value one or two steps below the highest one that
survived all of this.

### Examples from the author's phones

These settings are stable on the author's phones. They are a reasonable
starting point, not a promise that another phone will take them.

On the N95 8GB the author uses multiplier 111, with all dividers left in
`AUTO`. The memory bus divider then stays at its factory value, so the whole
system speeds up evenly: the processor goes from 330 to 426 MHz and the memory
bus from 165 to 213 MHz, both by 29%. Only the display divider is adjusted,
automatically, so that the screen keeps its normal frequency. The phone has run
in this state every day since August 2026, receives calls, keeps WiFi connected
and shows no memory errors. In SPmark 3D it gives 75.2 frames per second
instead of 66.2.

On the E90 the corresponding everyday setting is multiplier 104 with the bus
divider lowered from 6 to 4: the processor runs at 399 MHz and the memory bus
at 199.7 MHz instead of 110 MHz. In SPmark 3D on the large inner screen it
gives 28.0 frames per second instead of 16.4. The E90 gains more from the bus
than the N95, because its factory bus is slower (see
[Measured results](#measured-results)).

### Near 500 MHz

Both tested phones stopped near 500 MHz. That is the limit of the processor at
its factory voltage, and the application does not change the voltage.

At such a clock the bus divider cannot stay where it was for the everyday
settings, because the bus would rise together with the processor beyond what
the memory takes. The author's N95 8GB reached 491 MHz only with the bus
divider raised from 4 to 8, which runs the bus at about 124 MHz, below its
factory 165 MHz. The E90 reached 503 MHz with its factory bus divider of 6,
which gives about 168 MHz: faster than its factory 110 MHz, but slower than the
201.6 MHz of its everyday setting. A setting near 500 MHz therefore helps tasks
that compute a lot and read little from memory; games and 3D usually do better
with a moderate multiplier and a faster bus.

### Settings for experienced users

`BUSDIV` sets the memory bus divider: a smaller divider gives a faster bus.
This is where games gain the most, and also where instability is most likely,
so always run `TEST sdram integrity` after changing it. The new divider is
applied together with the multiplier through `APPLY SETTING` and `CONFIRM`.
Pressing OK on the `BUSDIV` row returns it to automatic selection.

`DIVIDERS` switches between `AUTO` and `MANUAL`. In `AUTO` the application
picks the remaining dividers itself whenever the multiplier changes; in
`MANUAL` each of them gets a row of its own. Beginners should leave it in
`AUTO`.

`DSPDIV` sets the frequency of the signal processor used for audio and video.
Unlike the other settings, it is applied at once by pressing OK on its row, and
it returns to the factory value by itself after 120 seconds. Raising the DSP
frequency this way crashes the phone; the multiplier already raises it safely
together with everything else. On the E90 the row is shown, but applying it
is refused with the message `DSP divider not available on this phone`, because
the necessary firmware address has not been measured there.

## Autostart

Without autostart, the overclock lasts until the next restart. With autostart,
a tested setting is applied automatically about 45 seconds after the phone
starts.

### Switching autostart on

1. Set the frequency in the usual way, apply and confirm it, and use the phone
   for a while to make sure it works.
2. Select `SAVE FOR AUTOSTART`. The application refuses to save a setting that
   is not actually running in the processor at that moment. It then runs three
   rounds of all tests; a single error and nothing is saved. If `KEEP ON EXIT`
   was off, it is switched on, so that the check below tests the saved
   setting and not the factory one.
3. The `AUTOSTART` row now shows `CHECKING` with a countdown of about three
   minutes. Keep using the phone normally and do not restart it. The application
   may be closed; the check continues by itself.
4. At the end, one more round of tests is run. If it is clean, the row shows
   `AUTOSTART: ON` together with the saved multiplier and bus dividers, for
   example `AUTOSTART: ON  (m111 at boot)  l3/4 l4/2`, and from now on the
   setting is applied after every restart.

### Switching autostart off

There are two ordinary ways, and either is enough:

- In the application, select the `AUTOSTART` row and press OK. It changes to
  `AUTOSTART: OFF`.
- With any file manager, delete the file `C:\Data\Overclock\boot.dat`. The
  application itself stays installed.

### If a saved setting turns out to be unstable

The phone protects itself. Before changing the frequency at startup, the
autostart component writes a note that it is about to do so. If the phone
freezes or restarts during the next few minutes, the following startup finds
that note, refuses the setting, switches autostart off and starts at the
factory frequency. At most one restart is affected. The next time the
application is opened, it explains what happened.

There is one failure the phone cannot detect by itself: the phone keeps working
but its screen stays dark. If that happens after a restart, or during the
three-minute check after saving, restart the phone by hand within the next few
minutes. A setting interrupted in this way is never applied again.

If the phone still needs to be rescued, the 45 seconds between startup and the
change of frequency are enough to open the application and switch autostart
off, or to remove the application through the application manager.

The log of every automatic start is kept in `C:\Data\Overclock\boot.log`.

## Questions and problems

**The phone restarted while I was raising the frequency.** This is the expected
way of finding the limit. Nothing is damaged. Use a lower multiplier.

**The application says UNKNOWN DEVICE or "no profile for THIS build".** Either
the phone model or its firmware version is not one of those listed above.
Nothing is changed in this case; the measurements still work. Adding a phone is
described in [`docs/PORTING.md`](docs/PORTING.md).

**The application says "patcher.ldd missing or driver refused".** RomPatcher+ is
not installed, or its patches are not active at the moment. Activate them and
start the application again.

**After RESTORE STOCK the application says "BUS LEFT LOWERED - reboot for
stock".** This is deliberate. Returning a lowered memory bus to its factory
speed crashed both test phones every time it was tried, so the application
refuses to do it. Restart the phone to return to factory settings.

**Does overclocking drain the battery faster or heat the phone?** Probably
somewhat, but this has not been measured yet.

# Part 2. How it works

## Clock structure

The OMAP 2420 produces all of its internal frequencies from a 19.2 MHz crystal.
A phase-locked loop (the DPLL) multiplies this frequency, and each part of the
chip then receives the result through its own divider:

```
DPLL = 19.2 MHz x MULT / 5     factory MULT = 86    ->  330.24 MHz
CORE = DPLL x 2                                     ->  660.48 MHz
CPU  = CORE / MPUDIV           factory MPUDIV = 2   ->  330.24 MHz
bus  = CORE / BUSDIV           factory BUSDIV = 4 on the N95 8GB (165.12 MHz)
                                              = 6 on the E90     (110.08 MHz)
```

The application changes the DPLL multiplier in `CM_CLKSEL1_PLL` and the
dividers in `CM_CLKSEL1_CORE` (memory bus, display, peripherals),
`CM_CLKSEL_MPU` and `CM_CLKSEL_DSP`. Because every divider takes its input from
the same DPLL, raising the multiplier raises every derived frequency. The
display divider is compensated automatically, so that the display keeps
approximately its factory pixel clock; a display driven far above its rated
frequency shows a distorted or blank picture.

The registers are written by a short relock routine of the application's own,
which runs from on-chip SRAM, so that the DPLL can relock while the external
memory is in self-refresh and not being accessed. The firmware has a routine
of the same kind: its power driver copies it into SRAM at boot and uses it for
deep idle. That copy cannot be changed at runtime, so the application writes
its own routine, built along the same lines, into a free part of the same SRAM
area and runs it there. Both steps go through the `patcher.ldd` kernel driver
from RomPatcher+ (`docs/WALKTHROUGH.md` §3.13–3.14). The addresses involved belong
to one firmware build, which is why the application checks both the machine
identifier and the firmware version before writing anything, and why it
verifies the code at the target address before patching it.

## Measured results

E90, SPmark 3D on the large inner screen:

```
                        bus        core       SPmark 3D
factory               110 MHz     330 MHz     16.33 fps
bus divider 6 -> 4    165 MHz     330 MHz     24.55 fps
plus multiplier 105   201 MHz     403 MHz     28.20 fps
```

The most informative row is the second one. Raising the bus alone, with the
core unchanged, increased 3D performance by 50%. On this chip 3D rendering
follows the bus clock, not the processor. The graphics core (PowerVR MBX) is
clocked from the same L3 bus as the memory, so this test cannot tell memory
bandwidth and GPU clock apart (docs/findings.md §53).

The test was repeated on 2026-10-12 for the video, with the everyday setting
the author now uses:

```
                        bus        core       SPmark 3D
factory               110 MHz     330 MHz     16.37 fps
multiplier 129        165 MHz     495 MHz     24.47 fps
multiplier 104        200 MHz     399 MHz     28.00 fps
```

The factory figure agrees with the first table. The second row runs the bus at
the same 165 MHz as the bus-only row above, with the core raised by a further
50%, and comes out the same (24.47 against 24.55 fps): the extra 165 MHz on the
core buys almost nothing. The everyday setting has a slower core than that row
but a faster bus, and it is the fastest of the three.

This also explains why the E90 gains more than the N95 8GB. On the small outer
screen, at equal clocks, the two phones run SPmark at the same speed. The
difference lies in the factory bus: 110 MHz on the E90 against 165 MHz on the
N95 8GB. The E90 therefore starts further behind and has more to gain from
raising the bus.

The memory bandwidth figures published in earlier versions of this README are
not reproduced here. They came from an earlier version of the built-in memory
benchmark, which counted twice as many bytes as it actually read
(`docs/findings.md`, section 46.1), and they are not valid.

On the N95 8GB the factory bus is already 165 MHz, so the multiplier alone
raises the whole chip evenly. With the bus divider left at its factory value of
4, every clock derived from the DPLL rises in the same proportion:

```
                        bus          core          SPmark 3D
factory, mult 86      165.12 MHz   330.24 MHz      66.17 fps
mult 111, BUSDIV 4    213.12 MHz   426.24 MHz      75.24 fps  (+14%)
```

The only divider the application changes here is the display divider DSS1
(6 to 8), which keeps the pixel clock near its factory value; the resulting
`CM_CLKSEL1_CORE` is `04360844` instead of the factory `04360644`. Both SPmark
figures were recorded on 2026-10-12 with this compensation in place. An earlier
pair, taken before the compensation worked, gave 62 and 75 fps; at that time
the register stayed at its factory value, so the display clock was also 29%
higher. The phone ran that way for about a week with zero SDRAM integrity
errors in every log (`docs/findings.md`, section 26.3). The two factory
figures, 62 and 66.17 fps, differ by about 7%, which is the usual spread
between repeated runs (see [How the measurements are made](#how-the-measurements-are-made)).

### Near 500 MHz

The core reaches about 500 MHz on both phones without a change in voltage.
These settings are not the best ones for games, because the bus cannot be
raised as far as at a moderate multiplier, but they are sometimes useful for
work that is limited by the processor rather than by memory.

```
                     mult   core (measured)   bus divider   bus          result
N95 8GB (RM-320)     129    491 MHz           8             123.84 MHz   core +50.1%, 0 bad words in 1 MB of SDRAM
E90 (RA-6)           131    503 MHz           6 (factory)   ~167.7 MHz   ALU test 37 Mitr/s
```

On the N95 8GB the core clock was measured by the instruction-timing method
described below, and the phone held the load with no SDRAM errors
(`docs/findings.md`, section 38.4). Before the bus was slowed down, the same
phone could not get past multiplier 111, so on the N95 8GB the bus, not the
core, was the first limit. On the E90 the bus divider of that run was not
recorded; the factory divider is the value consistent with the measurement
(section 40.1).

The E90 figure invites a comparison with the Nokia C5-00.2, a later phone with
an ARM11 core at 600 MHz, which gives 38.2 Mitr/s in the same test. The E90 at
503 MHz is only 3% behind it. Both figures come from the ALU test as it was on
2026-09-11, a serial chain of dependent multiplications, so they compare the
two cores on one kind of work and should not be read as a general ranking.

Above about 500 MHz the multiplier gives no further gain at factory voltage.
The remaining reserve is in the bus.

The configurations the author uses every day, as `ocgui` shows them:

| Phone | Multiplier | Core | Bus | `CM_CLKSEL1_CORE` |
|---|---|---|---|---|
| N95 8GB | 111 | 426.24 MHz | 213.12 MHz | `04360844` |
| E90 | 104 | 399.36 MHz | 199.68 MHz | `04360624` |

## How the measurements are made

The frequency a Symbian phone reports about itself cannot be trusted:
`HAL::ECPUSpeed` on the N95 8GB returns 206438 kHz while the core actually runs
at 330 MHz. All results in this project were therefore obtained by timing work
of known cost. The core clock is measured by executing a chain of instructions
whose cycle count is known exactly; memory bandwidth is measured with a working
set much larger than the cache.

Repeated runs of the same configuration differ by about 10% in memory bandwidth
and about 7% in frame rate. A single run therefore cannot distinguish anything
smaller than that. The second E90 row is the better of two runs of the same
state; the other run gave 23.00 fps.

## The one dangerous operation

Of all the operations the application can perform, exactly one has been
observed to kill a running phone: returning the memory bus from a lowered
frequency (a divider larger than the factory one) back to the factory
frequency. It happened four times out of four, on both
phones (`docs/findings.md`, section 38.2). Raising the bus above the factory
value, by contrast, works: the E90 runs daily at 201.6 MHz instead of 110.08,
and the N95 8GB at 213.12 MHz instead of 165.12. `RESTORE STOCK`
therefore refuses that single return and leaves the bus lowered until the next
restart, which restores factory clocking safely.

## Autostart design

The autostart component `ocboot.exe` is a separate executable with no user
interface, launched by the system's startup list. Its record
`C:\Data\Overclock\boot.dat` passes through four states: `Off`, `Candidate`
(saved, still being checked, never applied at boot), `Armed` (applied at boot)
and `Applied` (applied during this boot, hold period not yet over). Before any
register is touched, the state `Applied` is written and flushed to storage, so
a phone that dies during the hold finds this state on the next boot and refuses
the configuration. A second detector refuses a configuration whose previous
boot session ended less than five minutes after the boot confirmed it. A record
with a wrong size, magic number, version or checksum is renamed to `boot.bad`
and never used; a record written on another phone is ignored. The DSP divider and `RESTORE STOCK` are never used at boot. The
full reasoning is in [`docs/AUTOSTART.md`](docs/AUTOSTART.md).

## Known limitations

- The supply voltage is never changed, so the core limit near 500 MHz is firm.
- The SDRAM refresh interval is read and reported but not compensated. Raising
  the bus shortens the refresh period in real time, which is safe; lowering it
  lengthens the period.
- The cache benchmark performs much more arithmetic per byte than the memory
  benchmark. Its result reflects the arithmetic unit and must not be compared
  with the memory result.
- Battery consumption and temperature at 500 MHz have not been measured.
- Raising the DSP frequency through its own divider crashes the phone on the
  first use of the DSP. The application warns about this and reverts the DSP
  divider automatically after a timeout.

## Translations

The interface and the built-in help are available in 33 languages, the same
set as [PS1Emulator](https://github.com/1234bpv/PS1Emulator): English,
Ukrainian, Russian, Polish, German, French, Spanish, Italian, Portuguese, Czech,
Dutch, Swedish, Turkish, Romanian, Hungarian, Greek, Finnish, Danish,
Norwegian, Slovak, Bulgarian, Croatian, Slovenian, Serbian, Estonian, Latvian,
Lithuanian, Catalan, Chinese, Japanese, Korean, Hindi and Arabic. The phone's
fonts have no Chinese, Japanese or Korean characters on the tested phones, so
those three languages need a phone whose firmware includes them. All translations live in `src/strings.json`; the
command `node tools/gen_strings.js` regenerates the tables in `src/ocstr.cpp`.
A missing string falls back to English.

## Building from source

The build requires the S60 3rd Edition FP1 SDK, the CSL ARM toolchain with
GCCE, and Perl older than 5.22 (the SDK's `E32Plat.pm` uses `defined(%hash)`). `EPOCROOT` must be set without the drive letter.

The build scripts find the SDK, the toolchain and the deploy daemon next to
this repository through `tools/env.cmd`, so the drive letter may change, but the
shared directory layout described in `CLAUDE.md` must be kept. OpenSSL is found
next to Git or on `PATH`; setting `OPENSSL` to the executable overrides the
search. Changes to the environment stay local to each build.

```
tools\build_sis.cmd          build the application, package and sign
                             group\SymbianOverclock_sha1.sis
tools\build.cmd              build the diagnostic probe ocprobe.exe
tools\build.cmd run <ip>     the same, then run it on the phone and fetch its log
node tools/gen_strings.js    regenerate the translation tables
```

Three details are easy to get wrong:

- The package descriptor `group/ocgui.pkg` must stay ASCII with CRLF line
  endings. Otherwise `makesis` fails with the misleading message
  "No vendor names found".
- Signing uses RSA-1024 with SHA-1, because Symbian 9.2 cannot parse SHA-256
  certificates.
- In an SDK copied from another machine, `Epoc32/tools/makdeps.pm` must call the
  preprocessor `arm-none-symbianelf-cpp` through `PATH`, not by an absolute path
  on the old drive.

During development, builds are delivered to the phone over WiFi or Bluetooth by
the neighbouring project `SymbianDeployDaemon`:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ..\SymbianDeployDaemon\tools\deploy_net.ps1 -Ip <phone IP> -Device n95 -Info
```

For Bluetooth, replace `-Ip` with `-Com COMx`, where `COMx` is the outgoing
serial port of the paired phone.

## Further documentation

| File | Contents |
|---|---|
| [`docs/findings.md`](docs/findings.md) | The technical record. Every claim is marked as either confirmed on hardware or derived from ROM code. |
| [`docs/WALKTHROUGH.md`](docs/WALKTHROUGH.md) | A narrative account of how the overclock was found, with a primer on the hardware. |
| [`docs/AUTOSTART.md`](docs/AUTOSTART.md) | Design of the autostart and the reasons behind it. |
| [`docs/PORTING.md`](docs/PORTING.md) | How to add a phone or a firmware version. |
| [`docs/state.md`](docs/state.md) | Current state of the work. |
| [`docs/SCOPE.md`](docs/SCOPE.md) | Scope of the project: homebrew on the author's own devices. |

## Contributing and plans

This is a hobby project, developed in spare time on the author's own phones.
Bug reports, bug fixes and suggestions for improvement are welcome, as issues
or pull requests. Reports from owners of the N82, the original N95, the N93,
the N93i and the North American models are especially valuable: whether a test
profile worked or not, together with the lines the application prints about
the device, is exactly what is needed to make that phone supported
(see [`docs/PORTING.md`](docs/PORTING.md)).

In the future the author would like to extend the list of supported phones.
Because this is a hobby, there is no schedule for it.

## License

MIT, see [`LICENSE`](LICENSE).
