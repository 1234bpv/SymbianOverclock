# Overclock autostart

Status: **implemented in 2.3.0.** Off after installation; switching it on takes
three separate proofs. This file records why the scheme is shaped the way it is.
The code is `src/ocboot.h`, `src/ocbootcfg.cpp`, `src/ocbootmain.cpp` and the
two rows added to `src/ocgui.cpp`.

Earlier revisions of this document proposed a step-down ladder and a hardware
kill switch. Both were dropped, and the reasons are recorded below rather than
deleted, because each was rejected on an argument worth keeping.

## Why this is the most dangerous feature of the project

Everything else in the application has one property: the worst consequence is a
reboot, after which the phone is stock. Autostart destroys that property. A bad
value causes a reboot, the reboot starts the applier, the applier applies the
same value again, and the phone loops with no way in.

So the thing to design is not "how to apply an overclock at startup" but "how to
make the loop impossible". Everything else is secondary.

## The record, and the order in which it is written

The configuration lives in `C:\Data\Overclock\boot.dat`: magic, format
version, the machine UID it was written on, a state, the multiplier, the whole
`CM_CLKSEL1_CORE` and `CM_CLKSEL_MPU` values, the delay and hold periods, a
timestamp, and a CRC32 over all of it. Anything that fails a check - a missing
file, an unreadable disk, a wrong magic, a wrong version, a bad CRC, a machine
UID from another phone - means nothing is applied, and the file is renamed to
`boot.bad` so that it can be examined and can never be used.

It lives in a public directory rather than a private one because the installer
refuses a package in which two executables claim the same secure identifier
(findings 49), so `ocboot.exe` has an identifier of its own and no longer shares
a private directory with the application. Any process can therefore rewrite the
record; what stands between a forged record and a bad boot is the machine check,
the CRC and the two loop detectors below. The visible file also has a genuine
advantage: deleting it with any file manager switches autostart off without
touching the application.

It lives on `C:` rather than `E:`, because `E:` disappears in mass storage mode,
and a record that cannot be read is a record that cannot be switched off.

At boot the order is strict: write the state `EApplied` together with the
current time, `Flush`, `Close`, and only then touch a register. Symbian buffers
writes, and a phone that dies before the data reaches flash would come back to
the old record and try the same value again - which is exactly the loop.

## Four states, and why a configuration has to prove itself twice

`Off` is the state after installation. `Candidate` is what saving produces.
`Armed` is a configuration boot will apply. `Applied` means it was applied this
boot and the hold period has not finished yet.

Saving requires the overclock to be **live** - and live means what the registers
say when read back, not what the screen shows - and then three rounds of the
full test set. A single error and nothing is written. What is written is a
candidate, which boot ignores.

`ocboot soak`, started by the application as a separate process, then waits out
the hold period while the phone is used normally, runs one more round of tests,
and only then promotes the record to `Armed`. A restart during the soak leaves a
candidate, and candidates are never applied.

The first revision of this document put the soak at twenty minutes. Pavlo's
amendment on 2026-09-13 cut it to three, on the grounds that a setting meant for
daily use is better proven by the tests than by waiting, and three minutes is
what the record now defaults to.

## Refusal, not a ladder

A failed configuration is not adjusted. The earlier revision proposed stepping
down one notch at a time - first the bus raise, then the DSP divider, then the
multiplier - so that the phone would converge on something stable by itself.

That is revoked. The user sets a frequency deliberately and expects that
frequency; a machine-chosen approximation of it is no longer the thing they
configured, and presenting it as "your overclock" would be untrue. So a
configuration that fails is refused outright, autostart is switched off, and the
application says so the next time it is opened.

This also makes the scheme easier to reason about. A bad configuration costs
exactly one boot: the phone freezes, the user holds the power button, the next
boot sees `Applied`, refuses it, switches autostart off, and comes up stock.

## Two detectors

The first is the `Applied` state itself, and it catches everything that dies
before the hold finishes.

The second is the timestamp. A configuration that survives the hold and dies a
minute later would otherwise be applied on every boot for ever, and the first
detector would never see it. So if a boot finds an `Armed` record whose
confirmation is less than five minutes old, the previous session ended almost as
soon as it was confirmed, and the configuration is refused on that ground alone.

## What is not applied at boot

`RestoreStock` is never called. Returning an already lowered bus to stock killed
the phone four times out of four across two devices (findings 38.2). On the boot
path it is not needed: after a restart the bus is stock, application only ever
goes upward, and the recovery path is the reboot itself.

The DSP divider is recorded but not applied. A non-stock DSP divider rebooted
the phone twice out of two at idle (findings 24.5), which is why even the
application holds it only under a watchdog. A watchdog that must run for ever is
not a watchdog.

## The window, and the phone that lives without its screen

The frequency is applied about forty-five seconds after boot, not immediately.
On a normal boot nobody notices, because the system is still loading. On a phone
that a configuration brings down, those forty-five seconds are the same window
every time: enough to open the application and switch autostart off, or to
remove the application through the application manager, which needs neither a
memory card nor any patch.

Note what does **not** work as a way out, contrary to the obvious guess.
Removing the memory card does nothing, because the application, the applier and
the record are all on `C:`. Mass storage mode does nothing for the same reason.
And a file manager cannot reach `C:\sys\bin` on a freshly booted phone, because
RomPatcher+ patches are not applied at that point; removal therefore goes
through the application manager.

There is also a failure that no detector here can see. The phone can stay alive
while its screen does not - the software is fine, the hold completes, the
configuration is confirmed, and the user is left with a dark screen. Symbian
cannot tell us that the panel is dead, so the answer is an instruction rather
than a mechanism: if the screen stays dark after a restart, or during the soak
that follows a save, restart the phone by hand within the next few minutes. A
restart during the hold leaves `Applied`, and a restart during the soak leaves
`Candidate`; neither is ever applied again. The application says this when a
configuration is saved, and the built-in help says it as well.

## The hardware kill switch that was rejected

The earlier revision proposed skipping auto-application when the phone was
booted with external power connected: one rule, identical on both devices, no
card and no mode dialog needed. A measurement on 2026-09-11 showed
`EPowerExternal` readable without capabilities and returning 1, while
`EPowerBatteryStatus` and `ECaseState` are not supported on this hardware at
all.

Pavlo rejected it on 2026-09-13, and correctly: people charge phones overnight
and switch them on while plugged in, so the switch would cancel the overclock
constantly and by accident. It is not in the code, and the second data point
that would have confirmed the attribute was never taken.

With refusal instead of a ladder, nothing is lost by dropping it. A bad
configuration already costs at most one boot.

## What is installed, and what is off by default

The package installs `ocboot.exe` into `C:\sys\bin` and a `STARTUP_ITEM_INFO`
resource into `C:\private\101f875a\import\[E0001011].rsc`. The resource cannot
be created by the application - it is another process's private directory and
needs `AllFiles` - so it is always installed, and the switch is the record
instead.

That file name is not a choice. The installer registers the entry only if the
file is named after the UID of the package, and the system launches the entry
only if the executable it names carries that same number as its secure
identifier. This is why `ocboot.exe` has SID `0xE0001011` while `ocgui.exe`,
which cannot share it, moved to `0xE0001014`; the application keeps
`UID3 0xE0001011` and is the same application in the menu. Both rules fail
silently, and finding out cost four reboots - findings 50. With autostart off, `ocboot` reads `boot.dat`, sees `Off` and exits
before opening a single driver. Uninstalling removes both files.

## Honest assessment

The gain is that the phone comes up at the frequency the user chose. The price
is the only scenario in the project that can leave a phone in a state the user
has to act on, inside a known window, rather than simply rebooting out of.

What changed since the first revision is that the recovery no longer depends on
a hardware quirk that is not present on every phone, and that a failure costs
one boot rather than a sequence of them. What has not changed: this must never
be enabled by default.
