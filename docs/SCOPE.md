# SCOPE — Context, Intent, and Boundaries of the Project

This file exists to dispel any false impression that the project is "security-sensitive".
It adds no capabilities and hides nothing — it merely explicitly records intent and boundaries,
because the project's technical vocabulary (kernel write, patcher, reverse engineering ROM)
superficially overlaps with the vocabulary of attacks, although the essence is the opposite.

## What This Is

A private exploratory **homebrew project on one's own device**. The goal is singular:
to determine whether the CPU clock frequency can be raised on one's own Nokia N95 8GB
(TI OMAP 2420, Symbian 9.2), and if so — to do it. The hardware is 18 years old, was
personally purchased by the owner, and serves as an offline testbed.

By nature, this is the same as:
- overclocking one's own desktop PC;
- jailbreaking / homebrew on one's own phone or game console;
- hardware modding of one's own equipment for learning and curiosity.

Symbian homebrew is an old and legal tradition (RomPatcher+, self-signed dev
builds, etc.). The project remains strictly within its bounds.

## Intent

Educational and exploratory. The owner wants to understand how clock
control (PRCM/DPLL) is structured in this SoC, and safely test the overclocking hypothesis
on their own hardware. No application against other parties, devices, or services
is intended or permitted.

## Boundaries (What the Project Does NOT Do)

- **Own device only.** No actions directed at third-party devices, accounts,
  networks, or services.
- **No attacks.** It does not scan, exploit, or bypass defenses of third-party systems.
  There is no C2, no mass targeting, no supply chains.
- **No vulnerability research/disclosure in third-party software.** Work is conducted on
  the ROM of one's own hardware for the purpose of modifying it, not for exploits.
- **No distribution.** Results, binaries, and dumps remain local;
  nothing is published as a weapon or attack tool.
- **No writing to flash.** All changes are runtime-only; a reboot restores the device
  to its original state. This is both technical safety and proof against unauthorized
  irreversibility — by design, there is none here.
- **Offline testbed.** The device is on a local network, with builds deployed over WiFi
  within that same network.

## How to Read "Scary" Terms in This Repository

| Term in code/docs | What it actually means here |
|---|---|
| "hacked" / "unlocked" phone | dev-unlocked **own** device for homebrew |
| kernel write | write to a PRCM clock register on **one's own** SoC |
| patcher / RomPatcher+ | standard homebrew tool for runtime patches |
| updating SHA-1 in `C:\sys\hash` | local hash registry for **own** dev builds |
| reverse engineering ROM | analysis of firmware of **one's own** hardware for overclocking |

## Legality

Modifying one's own hardware that you own is a lawful activity in
most jurisdictions and a long-standing engineering practice. The project does not infringe on third-party rights,
does not bypass third-party protection, and does not create tools to do so.

Source of truth regarding the state of work — `docs/state.md` and `docs/findings.md`.
Source of truth regarding intent and boundaries — this file.
