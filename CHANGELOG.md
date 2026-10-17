# Changelog

## 2.4.0

- Test profiles for the North American Nokia N95-3 (RM-160) and N95 8GB
  (RM-421), derived offline from all seven collected firmware images and never
  run on a phone. Like the other test profiles they have no machine uid, are
  never selected automatically and can only be chosen in the `TEST PROFILE`
  row on an unrecognised phone.
  - `RM-160 NAM (TEST)`: fixed addresses for 20.2.011 and 35.2.001, whose
    images are identical apart from the ROM header; the routines are the
    RM-320 code moved within the image, matched word for word.
  - `RM-421 NAM (TEST)`: the RM-320 addresses, for 35.2.002, whose kernel,
    dvfs and USB driver code is byte-for-byte RM-320 and whose power.dll
    differs only in a build date string.
  - `RM-160 NAM (TEST, auto)` and `RM-421 NAM (TEST, auto)`: on-phone address
    search with the Usbc host, as on the N93, for every other build. The early
    RM-160 builds 10.2.006 and 11.2.009 show only a resident fragment and no
    dvfs driver in their images, so nothing is known about them in advance.
  Evidence: findings 55, raw output in `out/analysis/nam.md`.

## 2.3.3

- FIXED: when the hardware refused a divider layout, the multiplier stayed
  applied. `CM_CLKSEL1_PLL` and `CM_CLKSEL1_CORE` are written by one relock,
  but the hardware refuses the whole `CM_CLKSEL1_CORE` silently if one field is
  out of its range, and the multiplier went in regardless. On the E90 this
  left m105 on the stock dividers: bus 110 MHz instead of the requested 201,
  which looked like "the bus overclock does not apply", and the display clock
  22% above its own frequency, after which the phone rebooted. The engine now
  reads the register back after the first step; if it differs from the
  request it walks the multiplier back to where it started, keeping the
  dividers the hardware holds, and the log says `dividers refused, back to mN`.
  The bus divider is never moved by the walk back (findings 38.2). Findings 52.

## 2.3.2

- FIXED: the built-in help gave the clock formula as `DPLL = 19.2 MHz x MULT / 4`,
  in all 28 languages. The firmware stores the divider field as 4 and the
  hardware divides by that value plus one, so the formula is `/ 5`: with the
  factory multiplier 86 the old text gave 412.8 MHz instead of the 330.24 MHz
  the application itself shows. The formula and the sentence before it are
  corrected in every language. The version change also makes the help open by
  itself once more, so that phones with 2.3.1 see the corrected text.
- README rewritten: a first part for people who install and use the
  application, a second part with the engineering detail. Memory bandwidth
  figures from before the benchmark fix (findings 46.1) are removed.

## 2.3.1

- FIXED: after an autostart boot the application showed the bus and L4 dividers
  at stock while the hardware held the ones that had been applied. The multiplier
  row was seeded from the live register and the divider rows from the profile, so
  the two described different phones, and the phone the screen described did not
  exist. Every field is now seeded from the registers, and a phone that booted
  non-stock starts in manual mode rather than under automatic compensation, which
  would propose a layout of its own. The clock itself was correct all along -
  findings 51 has the register dump that shows it.
- ocboot reads CM_CLKSEL1_CORE back after applying it and logs what it found, the
  application logs the configuration as it writes it to the record, and the
  autostart row shows the saved l3 and l4 beside the multiplier. Between them,
  the question "was it actually applied" is now answerable on the phone.

## 2.3.0

- The application has a name and an icon. The registration resource carried no
  LOCALISABLE_APP_INFO, so the shell fell back to the name of the executable and
  listed it as "ocgui" with no icon. It is now "Symbian Overclock" in list view
  and "Overclock" under the icon, and the icon is a speedometer drawn at three
  sizes, each with its own amount of detail rather than scaled from the largest.
- The package vendor fields said "SymbianOverclock", which is the name of the
  application rather than of whoever published it. They now say "Pavlo
  Biletskyi". The unique vendor name cannot change once a package has been
  installed anywhere, so this had to be settled before release.
- NEW: the overclock can be applied automatically after a restart. It is off
  after installation and switching it on takes three separate proofs: the
  setting has to be live in the registers, read back rather than assumed; it has
  to pass three rounds of every test at the moment it is saved; and it then has
  to survive a soak period plus one more clean test round before boot will use
  it at all.
  The loop this could create is closed by writing the record before touching the
  clocks. A phone that freezes comes back to a record saying the previous
  attempt never finished, that attempt is refused, autostart is switched off,
  and the phone comes up stock - so at most one boot is affected. The
  application reports it the next time it is opened.
  Autostart never calls RestoreStock: returning an already lowered bus to stock
  killed the phone four times out of four, and after a reboot the bus is stock
  anyway. The DSP divider is recorded but deliberately not applied at boot,
  because a non-stock DSP divider is held only under a watchdog even inside the
  application.
  The work is done by ocboot.exe, a separate executable with no user interface,
  started from the system startup list. Deleting that one file switches
  autostart off without removing the application.
- FIXED: with ocboot.exe in it the package would not install at all, on either
  phone, and the installer said only "unable to install". A secure identifier
  has to be unique across the device, and ocboot.exe had been given the same one
  as ocgui.exe so that the two could share a private directory. It now has its
  own, and the record they share moved to C:\Data\Overclock, which both can
  reach. The full account, including the two explanations that turned out to be
  wrong, is in findings 49.
- FIXED: the startup entry was registered and then never ran, so autostart did
  nothing at all and said nothing either. The package UID, the name of the file
  installed into the startup import directory and the secure identifier of the
  executable being launched all have to be one number; ocboot.exe now carries
  0xE0001011 and ocgui.exe moved to 0xE0001014, keeping UID3 0xE0001011 so the
  application in the menu is unchanged. Confirmed on the N95: the clock is
  applied 45 seconds after boot and the multiplier reads back as asked.
  Findings 50 has the sequence, including three explanations that were wrong.
- FIXED: autostart switched itself off after the very first restart, saying
  nothing, on a phone where the configuration had just been saved and confirmed.
  The loop detector that refuses a configuration whose previous session ended
  right after it was confirmed counted the soak as such a session, and rebooting
  straight after a soak is what anyone would do - on the E90 it happened twice
  out of two. It now counts only confirmations that came from a boot, which is
  the case it was built for.
- The application now says which of the two refusals happened, rather than
  reporting both as a boot the phone did not survive, and it also reports the
  paths that give up before touching a register - engine not ready, apply
  failed, no profile. Those left the record armed and said nothing anywhere,
  which is exactly what a phone whose unlock patches are not applied at boot
  will do every time.
- The soak that follows a save now shows a countdown in the autostart row and
  asks not to restart the phone, and the row updates itself when the soak ends
  instead of waiting for the application to be opened again. Saving also
  switches KEEP ON EXIT on: the soak measures the configuration that is live, so
  closing the window must not put the phone back to stock underneath it.
- NEW: the help opens by itself the first time a build is run, and can be
  skipped with the right soft key. It is keyed on the version rather than on a
  plain flag, because uninstalling does not reliably remove the private
  directory and a flag could survive a reinstall.
- The measurement and stress tests moved from ocgui.cpp into octests.cpp
  unchanged, so that the boot-time applier runs the same tests rather than a
  second copy of them.
## 2.2.1

- FIXED: the DSP watchdog deleted its own timer from inside the timer callback,
  leaving the active object working on freed memory once the callback returned.
  It now cancels the timer and destroys it once, in the destructor.
- FIXED: TEST combined did nothing. The row fell through to the default branch
  of the dispatch switch and the combined run was never reached.
- FIXED: a digit key on the MULT row of an unrecognised phone set the multiplier
  to zero, below the allowed minimum, because the stock multiplier is not known
  until the engine is ready.
- FIXED: entering Prepare a second time - which happens when cycling through test
  profiles on an unrecognised phone - opened the patcher channel again without
  closing the previous handle.
- FIXED: the memory bandwidth figure in ocprobe still used the disproved
  64-byte cache line, so the probe reported roughly twice the truth while the
  application already reported it correctly. Both now read every word.
- FIXED: the view was not on the cleanup stack while AddToStackL could leave.
- FIXED: a ROM size of exactly 0x10000 reported in kilobytes was read as bytes,
  which would have limited the signature scan to the first 64 KB.
- The failed reset-vector check on a Usbc host no longer reports itself as a
  failure to open the driver.
- All comments in the source, and the documentation intended for contributors,
  are now in English.

## 2.2.0

- Bus guard removed. The bus divider now goes to hardware exactly as set by
  the user: overclocking must be tuned on the specific device anyway, and
  an unconstrained multiplier is just as capable of rebooting the phone.
  RESTORE STOCK still does not return an already lowered bus to stock — this is
  not a restriction on choice, but refusal of an operation that crashes the device;
  a reboot restores stock clocking in any case.
- New LANGUAGE row: UI language is selected manually from twenty-eight,
  by the language's native name, and the choice is persisted across launches.
- All 28 languages are now complete: previously, system strings and log messages
  remained English everywhere except Ukrainian.
- DSPDIV row no longer displays confusing "(in hardware /3)": it now shows
  "(now set: /3)" and a unit next to the frequency.
- CONFIRM row refers to the button by its translated name rather than in English.
- Built-in help describes all four dividers, not just the bus and CPU.

## 2.1.0

- FIXED: memory bandwidth benchmark overstated results by exactly 2x. It calculated
  bytes read under the assumption that the cache line was 64 bytes; hardware measurement
  showed 32 (findings 46.1). Now every word is read, and the assumption is eliminated
  from the calculation entirely.
- FIXED: cache benchmark measured arithmetic rather than cache — every word incurred
  multiplication, addition, and verification. It is now the same loop as the memory
  benchmark, except the working set fits into L1 cache, making the two numbers
  comparable.
- CPU benchmark additionally displays real core clock (CLOCK) and cost of a single
  operation in cycles (MUL+ADD). Both numbers are measured by instruction chains
  whose cycle costs are known exactly.
- RM-55 (N93) and RM-156 (N93i) profiles that search for addresses on the device itself
  via ROM signature scanning. Not verified on hardware; they exist so an owner
  of such hardware can try. The search mechanism itself was verified on N95 and E90, where
  correct answers are independently known.
- Built-in help rewritten: formal presentation with terminology definitions, frequency formulas,
  and explanation of each setting and each benchmark.
- Probe tool has new commands: membench, alubench, romfind, and font.

## 2.0.5

- Log pinned to the bottom of the screen, and space for it is allocated by
  number of wrapped LINES rather than number of entries. Because of this,
  benchmark results are no longer pushed offscreen.
- Newest log entry is now at the top of the log. Previously, new lines were added at
  the bottom and only became visible when pushed by an even newer line.

## 2.0.4

- Labels in the options list also wrap to screen width. Along with them,
  visible row recalculation: row height is no longer constant, so the number
  of visible options is calculated by accumulation rather than dividing screen height.

## 2.0.3

- Text wraps to screen width automatically. Manual line breaks removed: help text
  resides in source as solid paragraphs, wrapped by the application, so it is equally
  readable on the 240-pixel N95 display and the wider E90 screen.
- Help scrolling counts wrapped lines rather than source paragraphs.
- Help translated into all 28 build languages.
- List row remains single-line and is truncated: scrolling and cursor position
  rely on its fixed height.

## 2.0.2

- Apostrophe in translations replaced with ASCII: U+02BC has no glyph in any
  device font and rendered as a box (findings 44.3).
- Added font command to probe tool: rasterizes a character and reports whether a glyph
  exists for it in each device typeface.
- E90 verified on hardware with build 2.0.1: capturing kernel body original from
  live ROM works on Usbc carrier, frequency survives process exit.

## 2.0.1

- Overclocking is no longer reset on application exit. New row
  KEEP ON EXIT, enabled by default; reboot always restores stock clocks.
  Kernel patch, unlike frequency, is unconditionally removed on exit.

- Localization expanded to 28 languages: added Greek, Finnish, Danish,
  Norwegian, Slovak, Bulgarian, Croatian, Slovenian, Serbian,
  Estonian, Latvian, Lithuanian, and Catalan.
- Regional device language variants now map to the primary language: Austrian
  and Swiss German to German, Brazilian Portuguese to Portuguese,
  Cypriot Turkish to Turkish, and so on. Previously, such devices
  silently received English even though a translation existed.

- FIXED: patch rollback wrote back a constant from RM-320 in which three of
  the six words are branch offsets of that specific build. On a different build, such
  rollback would leave the driver corrupted until reboot. Stock words are now
  read from the device's live ROM before the first write.
- Bytes at the patch address are verified prior to patching. For a test profile,
  a mismatch results in refusal: nothing is written to the kernel.
- FIXED: TEST PROFILE row did not work at all — the handler sat behind
  the engine readiness check, and on an unknown device the engine is not ready precisely
  because there is no profile yet.
- After successfully attaching a test profile, UNKNOWN DEVICE is cleared from the screen,
  and the erroneous NON-STOCK at start row no longer appears.
- Power-down global search validates that the hint from the profile resides in kernel
  data and is aligned.

## 2.0.0

- Localization into 15 languages: English, Ukrainian, Russian, Polish,
  German, French, Spanish, Italian, Portuguese, Czech,
  Dutch, Swedish, Turkish, Romanian, Hungarian. A language can be
  partially translated — missing strings fall back to English.
- Test profiles for Nokia N82 (RM-313) and N95 classic (RM-159). Never selected
  automatically; manual selection only and after two warnings.
- Power-down global address is now discovered on the device with SRAM copy verification,
  rather than taken from offline conjecture. The mechanism was verified on two devices
  with known answers.

## 1.9.0

- FIXED dangerous code path: on E90 the DSP setter address is unknown and
  equals zero, and ApplyDspDiv did not check it — pressing OK on the
  DSPDIV row, the user would execute a branch to address zero in kernel mode.
  A profile without a DSP setter now gets a refusal before any action.
- Firmware gate bypass closed: ocprobe selftest level 2 applied the patch without
  firmware check, using the reference profile by default.
- Bus guard descriptions brought into agreement with measurement: FATAL is
  RETURNING a lowered bus to stock, whereas raising above stock on E90
  was measured and works. Previously, the application frightened with what had been disproven.

- Localization: English and Ukrainian, language taken from device settings.
  Translation source is `src/strings.json`, tables generated by
  `node tools/gen_strings.js`.
- Built-in help: `HELP` row opens fullscreen help, scrolling
  up/down, exit with any other key. No network needed.
- Firmware gate: application verifies device firmware build against the one addresses
  were derived for, and refuses to patch kernel on mismatch. This gate
  revealed that the N95 8GB runs 35.0.001, while the profile was called 31.0.015 all along.
- Unknown device now prints machine UID and firmware version — exactly what
  starts a new profile (`docs/PORTING.md`).
- Release documentation: README, LICENSE (MIT), this file, AGENTS.md.

## 1.8.0

- `BUS GUARD` became a toggle. Bus raise prohibition remains by
  default, but can be cleared explicitly — via GUI row or `guard=0` switch in
  the probe tool. The basis for prohibition remains returning the bus from a lowered
  state, which produced four reboots out of four.

## 1.7.0

- Multiplier ceiling raised from 129 to 255. 129 was our slider limit, not
  a hardware one: the multiplier field has ten bits.
- Bus clamp is now visible in the interface before applying, not just in the log
  afterwards.

## 1.6.0

- Bus guard moved to the write point, meaning it covers any layout change,
  not just returning to stock.
- Instruments verify the result against what ACTUALLY went to hardware, not what
  was requested.
