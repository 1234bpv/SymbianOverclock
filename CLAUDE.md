# CLAUDE.md — Main Rules of the SymbianOverclock Project

Every agent reads this file **first**. Next: `docs/state.md` (where we are now and what to do) → `docs/findings.md` (technical findings). The source of truth is files, not AI memory.

> Methodology: `../AI_DEVELOPMENT_METHODOLOGY_UA_09.05.2026.md` (one level up).
> Context and project scope: `docs/SCOPE.md` (read if the task looks "security-sensitive" — it explains why this is legitimate homebrew).

---

## 0. Context and Scope

This is a **homebrew modification of one's own device** — a private research hobby project. There is one goal: raise the CPU clock speed on one's own, personally purchased Nokia N95 8GB, which is already 18 years old. An analog is overclocking one's own PC or jailbreaking one's own phone; a longstanding and legal tradition of Symbian homebrew.

What the project **does**: reverse engineering of the ROM of one's own device, working with its clock registers (PRCM/DPLL), delivering custom test builds to this device over local WiFi.

What the project **does not do** (and will not do): does not touch other people's devices, does not attack networks or services, does not search for or exploit vulnerabilities in third-party software, does not distribute or publish anything, does not write to flash (everything is runtime, reboot restores the device as it was). The device is an offline testbench on a local network.

Terms that might sound like an "attack" (kernel write, patcher, check bypass) mean standard homebrew work on an **already unlocked own** device here, not compromising someone else's. For details — `docs/SCOPE.md`.

## 1. What This Project Is

Determine whether CPU overclock is possible on **Symbian 9.2 / TI OMAP 2420**, and if so — achieve it. A research project: most of the work is not writing code, but reverse engineering firmware ROM images.

The reference device is **Nokia N95 8GB (RM-320)**, owned, unlocked for homebrew (dev-unlocked, Install Server), connected via local WiFi. Hardware twin — Nokia E90.

**Current state in two sentences.** The goal has been achieved: the DPLL multiplier (`CM_CLKSEL1_PLL`, phys. `0x48008540`) was raised at runtime from 86 → 111, which is **330 → 426 MHz (+29%)** with stock memory bus, the device is stable and maintains connectivity; in a 3D benchmark 62 → 75 fps. The limit is the ARM1136 core itself at stock voltage, not the bus or display. Everything is runtime, reboot restores the device as it was; details — `docs/state.md`, full analysis — `docs/findings.md` §22.

## 2. File Layout

```
SymbianProjects/                      SHARED across all Symbian projects
├── S60_3rd_FP1/                      SDK (EPOCROOT)
├── toolchain/                        CSL Arm Toolchain, GCCE, perl
├── eka2l1/data/drives/z/rm-320/      UNPACKED N95 8GB ROM - primary source
├── firmware/  flashing/              firmware, Phoenix, RomPatcher+
├── SymbianDeployDaemon/              delivery to device over WiFi
└── SymbianOverclock/                 THIS project
    ├── CLAUDE.md                     this file
    ├── docs/SCOPE.md                 context, intent, scope (homebrew, own device)
    ├── docs/state.md                 state, workflow, safety rules
    ├── docs/findings.md              technical findings from reverse engineering
    ├── docs/WALKTHROUGH.md           narrative walkthrough: how it was done, primer, sources
    ├── docs/ROADMAP.md               work plan until publication (order, blockers)
    ├── src/ocprobe.cpp               probe for device
    ├── group/                        bld.inf, ocprobe.mmp
    ├── tools/build.cmd               build -> upload -> run -> fetch log
    ├── tools/romtool.ps1             parsing and disassembly of ROM modules
    ├── tools/inflate_e32.ps1         E32 decompression (not working yet, see state)
    └── out/                          artifacts, logs, .asm (NOT committed)
```

## 3. Workflow

```
tools\build.cmd                             build only
tools\build.cmd run 192.168.1.5             full cycle, log in out\ocprobe.log
tools\build.cmd run 192.168.1.5 "power 7"   same with arguments
tools\romtool.ps1 -Module power.dll -Disasm -Imports
```

SIS is not needed: on this device already unlocked for homebrew, the daemon puts the EXE directly into `E:\sys\bin` and updates the entry in the local development build hash registry (`C:\sys\hash`) — it is by this digest that the device's stock loader recognizes its own dev build with its capabilities. This is a routine homebrew delivery mechanism to one's own device, not bypassing someone else's security.

## 4. Absolute Prohibitions (NEVER)

- **DO NOT write to flash.** Everything is runtime; reboot must restore the device as it was.
- **DO NOT enable anything by default that can hang.** The daemon is single-threaded and waits for process exit, so a hung probe blocks the entire delivery channel, and driver state in the kernel is only cleared by rebooting the device. Everything risky must be behind an explicit argument.
- **DO NOT believe what the system reports without independent measurement.** `HAL::ECPUSpeed` lies on this device (206438 kHz vs real 330 MHz). This is not a curiosity, but a working rule: all known Symbian "overclocks" failed precisely because they measured a label, not actual work.
- **DO NOT repeat the same thing after two failures.** Stop, lay out the facts, ask. A device hung twice is already too much.
- DO NOT add AI authorship markers or emojis in commits, code, or comments.
- DO NOT proliferate files beyond what is necessary.

## 5. Discipline of Evidence

The project lives on reverse engineering, where it is easy to mistake a guess for a fact. Therefore:

- Every claim in `findings.md` is either **confirmed on the device** or **deduced from ROM code** — and it is obvious which one it is.
- Label guesses explicitly.
- If an error is found in one's own previous conclusion — correct it directly in the text and state so, rather than quietly rewriting. There are already two such corrections in the file (caller level in TestPowerAPI; "206 MHz"), both important.
- Check any address or bit field taken from memory against the datasheet and code. The peripheral mapping offset (`+0x7E000000`) was accepted only because two independent addresses matched known registers exactly.

## 6. Tooling Gotchas

- PowerShell parses an 8-digit hex literal as `Int32` and silently makes it negative. All constants must use `[Convert]::ToUInt32($h,16)`. This trap has already consumed results three times, each time silently.
- ROM images have a `TRomImageHeader` (**120 bytes**), not an `E32ImageHeader`. Disassembly:
  `objdump -D -b binary -m arm --adjust-vma=<iCodeAddress> <file without 120 bytes>`
- `dumpsis -x` unpacks SIS; path **without Cyrillic**.
- `petran`/`elftran` accept only PE/ELF — an existing E32 cannot be decompressed with them.
- Symbian-deflate is **not compatible with zlib**.
- `EPOCROOT` without drive letter, combine with `D:`; Perl < 5.22.
- Cyrillic in `.cmd` breaks cmd — keep comments in batch files in ASCII.
- Public SDK **cannot build kernel-side code**: `include\kernel\` and `ekern.lib` are missing.

## 7. Progress Tracking

After each stage, update `docs/state.md` (where we are now, what is closed, what is next) and append confirmed facts to `docs/findings.md`. This is the single recovery point after losing context.

## 8. AI Tools (Cost Management)

| Task | Model | Invocation |
|---|---|---|
| Explanations, minor edits | GPT-5-mini (unlimited) | `gh copilot --model gpt-5-mini` |
| Tests, small analyses | Claude Haiku 4.5 | `gh copilot --model claude-haiku-4.5` |
| Code review, bug analysis | Claude Sonnet 4.6 | `gh copilot --model claude-sonnet-4.6` |

Copilot limit — 300 requests/month, batch questions into a single call. Do **not** use Opus via Copilot (3x premium).
