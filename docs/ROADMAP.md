# Pre-Release Roadmap

Order of work agreed upon on 2026-08-08. Goal — open-source the code on GitHub and
publish a signed `.sis`. Update along with `state.md`.

The device is unavailable until ~2026-08-15, so everything that does not require
hardware is done first. Items 1-6 — without the device, item 7 — at home.

---

## 0. Auto-applying safe settings with a cancellation window

Done **first**, before the rest of the preparation.

**What:** after launching the application — a 60 s countdown, after which the default
configuration is applied automatically. During the countdown, application
can be cancelled.

**Why:** to have a working overclock without navigating menus manually, and at the same time
a guaranteed window to abort it, "just in case".

### Default values

`mult 111` for this device: +29.1%, confirmed by long-term daily
use (findings §22, clarification 2026-08-08). `107` remains as an option
with a margin.

In the **public** build, `107` should be the default: on someone else's device
there is neither long-term verification nor guarantee of the same cooling conditions, while the margin
over 111 is less than one multiplier step (112 reboots within minutes).
In practice: save the value as a setting, initialize `107`,
the user sets their own value once — and it persists.

### Cancellation: two independent paths

**1. Any key while the application is on screen.** The countdown is visible from the first
second: how much time remains and what exactly will be applied. Not a silent timer.
Cancellation is the default behavior; confirmation is required only to
apply ahead of time.

**2. Holding a hardware combination for ~5 s** — for the case when the UI is not
visible (will be relevant if auto-apply is ever hooked up to system
boot).

The combination must be checked **throughout the entire countdown window**, not just at
the moment of startup: otherwise one has to anticipate it in advance.

Choosing the combination is a separate decision, and `4`+`6` is not suitable for it for a purely
mechanical reason: **on the N95 8GB, the numeric keypad is under the slider.** If
the device is in a pocket with the slider closed — precisely when the escape grip is
needed — `4` and `6` cannot physically be reached. On the E90, the layout is different again.

Candidates available when closed on both devices:

- **volume + / volume −** simultaneously (side keys);
- **navigation left + right** (d-pad key on the front).

Both need to be verified on the device: the system intercepts some keys at
the window server level, and whether events will reach the application is a matter for experimentation.
Do not touch the power key: it opens the system menu.

The technical prerequisite for both paths is identical: to read keys, the application
needs focus. Therefore, during the countdown it must **bring itself to the foreground**
— this simultaneously makes the countdown visible and makes the keys readable.

### Remaining requirements

- The gate from item 1 acts **before** the countdown: if the firmware does not match — the timer is
  not armed at all, only a message.
- Cancellation must leave the system in exactly the same state as before launch: no patch
  applied, no channel opened.

### Failsafe against a reboot loop

The main risk appears not right now, but if auto-apply is ever hooked up
to autostart on boot: failed application → reboot →
autostart again → failure again. There is no way out of such a loop on Symbian other than
uninstalling the application from a PC or reflashing.

A hardware key combination is **not a solution** here: if the previous attempt killed
the device before the application received focus, there is nothing to read the keys. The combination
saves from "the system booted, but I want to stop it this specific time" — that is a different
scenario.

The primary failsafe is an **attempt marker**. Protocol:

1. the user selects settings in the application and enables auto-apply;
2. before relock, a marker file is created;
3. if after **3 minutes** the application is still running — the overclock is considered stable,
   the marker is deleted;
4. on device boot, the application starts after one minute and **first and foremost
   checks the marker**:
   - no marker → create, apply, delete after 3 min;
   - marker present → **do not apply**, consider configuration unstable,
     warn user upon entering the application.

Do **not reset** settings in doing so: the user may want to tweak them
partially, and the reboot could simply have occurred at an inopportune moment.

Place the marker on `C:`, in the application's private directory. Placing on `E:` is not allowed: this
drive is unmounted in Mass Storage mode, which would make the marker unreadable
precisely when it is needed most. The marker deliberately survives a reboot — that is
the point; the "do not write to flash" rule applies to ROM and is not violated.

### Four details without which the protocol does not work

**1. The marker must reach disk BEFORE relock.** Symbian buffers writes. If
a file is created and one immediately proceeds to relock, a hardware reboot may occur
earlier than data hits flash — and the marker will not be there after the reboot. That is, the scheme
will silently fail in the single case for which it exists. Mandatory:
`RFile::Flush()` (and closing the file) before applying.

**2. The main loophole: failures slower than 3 minutes.** The protocol catches only fast
failures. But a failure on the verge is precisely slow: according to observations, `mult 112` "works
and crashes within minutes", while thermal instability generally emerges under prolonged
load — 3D after 20 minutes in a warm pocket.

Scenario: the configuration survives 10 minutes. At minute 3, the marker is deleted as
"stable". At minute 10, the device reboots. On startup, there is no marker →
the overclock is applied again → another 10 minutes → reboot. **A loop that the watchdog
never sees.**

The solution is not to extend the 3 minutes (that merely shifts the threshold), but to count. Keep
in the marker a **counter of applications after which the user has never
opened the application manually**. Real use resets the counter to zero; a reboot
loop increments it quickly. On the third attempt — auto-apply turns itself off
and a warning is shown.

**3. A clean exit from the application must also delete the marker.** Otherwise, a typical
scenario yields a false "unstable": the user applied the overclock, closed
the application earlier than 3 minutes, and the phone rebooted an hour later for
its own reasons. A clean exit is proof that the system was alive and delivered
the exit command, which is evidence of the same class as "process survived 3 min".

**4. The marker is better off not empty.** It is virtually free to put inside: the values
that were applied (mult and dividers), timestamp, and `User::NTickCount()` at the moment
of creation. Provides two benefits:
- the warning becomes concrete: "last attempt: mult 112, 2026-08-15 14:32";
- the nanokernel tick counter is reset only by a full reboot, so comparing
  the stored value with the current one **directly distinguishes** "the device rebooted"
  from "the application died, but the system is alive". The project already uses this
  instrument — see `findings.md` §19.

False positives (the user rebooted the device at an inopportune time) are always produced by the protocol,
and this is the correct direction: an error leads to "do not apply", not to
"apply blindly".

### Autostart on device boot

**Disabled by default in the public build.** The end user enables
it manually in the application and configures frequencies for auto-apply themselves — after
a warning with recovery paths (below).

**Implementation.** The standard S60 3rd mechanism is a resource in the startup list
(`\private\101f875a\import\<UID>.rsc`, Start-up List Management). Writing a file there
from the application **is not possible**: this is another process's private directory, requiring
`AllFiles`, which a self-signed certificate does not grant. Therefore, enabling/disabling
autostart cannot be done by creating and deleting this file.

Workable arrangement instead:

- The SIS **always** places an entry in the startup list, meaning the application starts on
  boot;
- on startup, it reads **its own** setting (its private directory, permissions
  are present) and, if auto-apply is disabled — silently exits, doing
  nothing. This is the default state in the public build;
- if enabled — brings itself to the foreground and arms the countdown with
  frequencies configured by the user.

### Installation to memory card — as additional insurance

Install the application to `E:` (use `!:` instead of a fixed
letter in `.pkg` so that the installer prompts for the drive).

**Why:** in USB Mass Storage mode, drive `E:` is unmounted from Symbian and
handed over to the PC. An application located on it **cannot run** in this
state — that is, the cable becomes a hardware kill-switch for autostart, independent of
the state of the system itself.

Two clarifying details important for documentation:

- On the **N95 8GB**, there is no removable card: `E:` is the built-in 8 GB mass storage.
  It cannot be physically removed, so only the path via Mass Storage works, not
  "pull out the card". The **E90** has a microSD slot, and both work there.
- Keep application settings on `C:` (its private directory). Then
  unmounting `E:` halts **launch**, but does not lose configuration.

**Requires verification on the device:** does the system manage to unmount `E:` before
the startup list executes when the cable is plugged in prior to powering on
the device and the default USB mode is set to Mass Storage. If it does manage — this is
the most reliable recovery path and should be made the first item in the instructions.
If not — the path remains viable, but already within the 60-second countdown window.

### Warning when enabling autostart

Display before allowing enablement, and repeat in the README. Recovery
paths, in descending order of reliability:

1. **USB Mass Storage** — `E:` is unmounted, the application does not start at all.
2. **Removable card** (where available, e.g. E90) — remove before powering on the device.
3. **Attempt marker** — after a failed attempt, auto-apply disables itself,
   without user intervention.
4. **Countdown window** — 60 s, any key or hardware combination.
5. **Application uninstallation** via PC — requires the device to be responsive.

And on a separate line, what alleviates most fear: **frequency does not persist**.
Nothing is written to flash; a reboot always restores the device to stock
330 MHz. There is exactly one dangerous scenario — a reboot loop, and items 1–3 are targeted
precisely against it.

Current status is documented in `state.md`: "No autostart: nothing is applied after
reboot" — update there as well upon implementation.

---

## 1. Device and firmware gate, addresses into a table

Currently there is no verification at all: `ocengine.cpp` keeps `KDvfsBody = 0x80148410`
and `KDvfsOriginal[6]` as hardcoded constants. On a different firmware, the exact same patch
will land on the **wrong code**, and that is no longer "a reboot restores things as they were", but execution
of arbitrary bytes in kernel mode. This is the most critical change prior to publication.

- `HAL::EMachineUid` + SW version via `SysUtil::GetSWVersion` against an allowlist.
  Mismatch — the application refuses to patch and directly states which
  configurations are verified. Tests remain accessible.
- Move addresses out of constants into a table of "device + firmware → address set"
  (`KDvfsBody`, original body words, `power.dll` addresses, SRAM object
  global).
- Cost-free channel fingerprint, without a single write: on stock firmware,
  `DoControl(1, 0)` returns a percentage (50 or 100), remaining opcodes return 0. If matched —
  module belongs to the same family.

### Principle that must remain in the architecture

> For each new firmware there is **exactly one unconfirmed write** — the READ patch.
> Everything following it is verified by **reading** before anything is written.

After the READ patch, verify: expected bytes at `power.dll` addresses, global
`0xC80030CC`, SRAM linear address, decoding of `CM_CLKSEL1_PLL` into sensible
`mult/div`, and dispatcher bytes immediately following the patched body (`+0x18`) — the latter
is direct proof that the patch landed in that module. Mismatch — abort and unshadow.

---

## 2. Signature search for addresses in `romtool.ps1`

Automatically deducing addresses **on the device** is impossible: scanning the ROM
requires a read primitive, but to obtain one — one already needs to know the address for
the READ patch. The circle cannot be broken on the device.

Therefore, the search is performed **offline, against a ROM dump**, once per firmware:

- input: unpacked dump of the new firmware;
- output: a row in the address table for item 1;
- method: byte patterns (like the search patterns in RomPatcher+ itself) rather than
  fixed offsets. Search for: the body of `DoControl(1)` in `dvfs_interface.ldd`,
  the `power.dll` idle routine, the global of the power-down object.

Verify immediately against the E90 firmware dump — the device itself is not needed for this.

---

## 2a. List of OMAP 2420 devices and which firmwares to download

Compiled 2026-08-30. Devices with S60 3rd Edition, that is, those where our code could
potentially run at all:

| Device | Product codes | Platform |
|---|---|---|
| Nokia N93 | RM-55 (global), RM-153 (China, no 3G/WiFi) | S60 3rd FP0, Symbian 9.1 |
| Nokia N93i | RM-156, RM-157 (China) | S60 3rd FP0, Symbian 9.1 |
| Nokia N95 | RM-159 (global), RM-160 (NAM), RM-245 (China) | S60 3rd FP1, Symbian 9.2 |
| Nokia N95 8GB | RM-320 (global), RM-421 (NAM), RM-321 (China) | S60 3rd FP1, Symbian 9.2 |
| Nokia N82 | RM-313, RM-314 (China) | S60 3rd FP1, Symbian 9.2 |
| Nokia E90 Communicator | RA-6, RA-7 | S60 3rd FP1, Symbian 9.2 |

Outside Symbian, the same SoC is found in Nokia N800 (RX-34) and N810
(RX-44) tablets running Maemo. We will not be overclocking them, but one fact from there is important:
the N810 runs at 400 MHz stock, and the N800 after updating to OS2008 also increased from
330 to 400. That is, our 426 MHz lies not much higher than the frequency that Nokia itself
considered stock for this very same silicon in another product.

Do not confuse with OMAP 2420: N73, N80, E61i — these are OMAP 1710 (ARM926 at 220 MHz);
N76, N81, N81 8GB, E51, E66, E71, 6120 classic — Freescale MXC300-30; N96 —
STMicro Nomadik; N86 and N97 — ARM11 434 MHz with Broadcom GPU. None of them are
suitable. Likewise the Nokia C5-00.2, which we have in hand: ARM11 600 MHz, different
SoC, and within this project it remains a device for the daemon, not a target.

### Exactly how many firmwares are needed

Significantly fewer than it seems. The product code (seven digits like `0550798`)
specifies the region, language pack, and carrier customization, but does not alter
the core ROM. This is not a guess: in our `firmware/n95-8gb/` there are three different
product codes — `0550798`, `0550818`, `0550819` — and all three `.vpl` files
reference the exact same file `RM-320_31.0.015_PR.C0R` with the single
checksum `e7ffb3c7`. Only PPM (languages) and CNT (C: drive contents) differ.

Therefore, one needs to download different firmware versions (`SwVersion`, e.g. `31.0.015`
versus earlier `20.x` or `11.x`), one product code per version, rather than
a dozen regional variants of the same version. Regional variants will be needed
only if signature searching ever reveals a divergence within a single version —
in which case that will be a separate discovery worthy of entry in `findings.md`.

Of the files in the package, exactly one is needed: MCU/CORE (`*.C0R` or `*_core.fpsx`) —
that is where `power.dll`, `dvfs_interface.ldd`, and `ehwaomap.pdd` reside. PPM (`*.vNN`) and
CNT/UDA do not need to be downloaded at all.

### Order

1. E90 (RA-6) — the same FP1/9.2, different firmware. The primary generalization test,
   and the device is in hand.
2. A second firmware version for RM-320 — verifies whether signatures are caught between
   versions on a single model. This is the most common case in a release.
3. N82 (RM-313) and N95 (RM-159) — other models of the same generation; no devices available,
   so verification is purely offline.
4. N93/N93i — separately and later. This is FP0/Symbian 9.1, meaning a different kernel build;
   there is no basis to expect the same patterns, and failure here will say nothing
   about the rest.

---

## 3. English-language materials

- README in English — **write from scratch**, do not translate. Foundation —
  `WALKTHROUGH.md`.
- Translating code comments into English. This is not just about publication:
  Cyrillic in `.cpp` on this toolchain is also a build risk.
- Do **not mechanically translate** `findings.md` and `state.md`. This is a research
  record, and its value lies precisely in the nuances of corrections; machine translation will ruin it.
  Ukrainian originals remain as is.

The first thing that must be visible in the README: everything is runtime, a reboot restores the device to its
original state, nothing is written to flash. This is the strongest argument and it does not belong at the
bottom of the page.

Separately in the README: the project is about **OMAP2420**, not about Symbian in general. C5-00 and 701
are different SoCs, and 701 (Belle) is also a different kernel and a different patch model.

---

## 4. Application icon

Currently there is not even scaffolding: `ocgui_reg.rss` does not have `localisable_resource_file`,
`bld.inf` does not have `PRJ_EXTENSIONS`.

1. `gfx/ocgui_icon.svg` — SVG-Tiny, not full SVG;
2. `bld.inf`: `PRJ_EXTENSIONS` with `START EXTENSION s60/mifconv` → `ocgui.mif`
   and `ocgui.mbg`;
3. `ocgui_loc.rss` with `number_of_icons = 1;`
   `icon_file = "\\resource\\apps\\ocgui.mif";`
4. reference to it from `ocgui_reg.rss` via `localisable_resource_file`;
5. add `.mif` to `.pkg` — otherwise the menu will show an empty square.

---

## 5. GUI refinement

Open item from `state.md`: the application itself has not yet been verified on the device. The engine
underneath it is proven (`patcher oc 91`, `hold 107`), the interface — not.

---

## 6. Repository hygiene

- The repository is not yet initialized as git.
- Do not commit: `out/`, ROM dump, firmwares, third-party binaries.
- **Do not put `patcher.ldd` into the repository** — it belongs to someone else's RomPatcher+. Link
  to the original and a "prerequisites" section in the README; otherwise the very first issue will be
  "doesn't work".

---

## 7. At home, with devices

Order is mandatory: **do not debug two unknowns simultaneously.**

1. **GUI on N95 8GB (RM-320)** — known-working configuration. First
   ensure that the interface and auto-apply work there.
2. **E90 as a generalization test.** The same OMAP2420, the same S60 3rd FP1,
   different firmware. Verifies not addresses, but **whether the structure generalizes**: whether
   signature search finds the same patterns. If it finds them — the method can be released to the
   public; if not — you will learn about this prior to release, rather than from issues.

Caveat regarding E90: divider compensation is tuned for the N95 panel, whereas it
has a different display (two panels, different resolution). Its operating point will almost certainly be
its own. This is not a blocker, but one should not expect a match with `mult 107`.

---

## Publication: sequential, not synchronous

The overclock and the PS1 emulator have **different blockers** (here — GUI testing, there — screenshots,
video, and polishing). Tying releases together surrenders pace to the slower one.

Order: first overclock (it is closer to readiness), with a mention in the README:
"tested on my PS1 emulator, +14% in 3D" (SPmark, 2026-10-12); then the emulator with a link
back. Cross-linking provides the same synergy without a shared blocker, and the overclock simultaneously
gathers the audience to which the emulator is addressed.
