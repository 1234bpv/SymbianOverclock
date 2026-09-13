# Overclock autostart: scheme proposal

Status: **proposal, not implemented.** The application does not apply anything by itself and is not in the startup list.

This revision incorporates four amendments from 2026-09-11, and each of them revokes a decision of the previous revision. **All** user settings are applied, including the bus. There is no countdown with key cancellation. Mass Storage is no longer the primary recovery path. And overclocking now survives exiting the application, and this is already done (the `KEEP ON EXIT` line).

## Why this is the most dangerous feature of the project

Everything else in the application has one property: the worst consequence is a reboot, after which the device is stock. Autostart destroys this property. A bad value causes a reboot, the reboot starts the application, the application applies the same value again — and the device loops in a cycle with no user entry point.

That is, one must design not "how to apply overclock at startup", but "how to reliably exit the loop". Everything else is secondary.

## 1. Everything configured by the user is applied

The previous revision excluded the bus from auto-application as too dangerous. This has been revoked, and not just because the user decided so — upon closer inspection, the reasoning was simply flawed.

The fatal operation is **returning an already reduced bus back to stock**: it was specifically this that killed the device four times out of four (§38.2). Raising the bus from stock upward has been measured, and it works. And on the autostart path, returning does not happen at all: after a reboot the bus is already stock, application goes only upward, and if the next boot deems the configuration failed, it simply **does not apply** it — there is nothing to roll back, because the reboot has already rolled everything back.

Hence autostart does not merely "have the right" to touch the bus — it touches it in the only direction proven safe, and has no access to the direction that kills. The only condition that must be strictly maintained: **autostart never calls `RestoreStock`.** The recovery path here is only one — a reboot.

## 2. Instead of a countdown — automatic confirmation

The previous revision brought the application to the foreground and showed a 60-second countdown with cancellation by any key. User amendment: this is not a solution, but merely extending the phone's startup by a minute.

The objection is valid, and the reason goes deeper than inconvenience. The countdown protects against something that does not happen here: the user does not "change their mind" — they configured the settings deliberately the previous time. The dangerous case is different: the setting turned out to be fatal, and the person is not looking at anything at that moment. The countdown in this case saves no one, while taking a toll on every normal boot.

Therefore, confirmation must be automatic and rely on fact, not on attention.

The application writes a marker, applies the settings, and **remains in memory**. If the device survived a holding period after application (I suggest three minutes), the application rewrites the marker: this configuration held out. The next boot sees one of two things. The marker says "held out" — we apply without any questions and without a delay noticeable to a human. The marker says "applied, but not confirmed" — the previous session did not survive the holding period, and the configuration is deemed failed.

The holding period costs nothing: the application remains a background process anyway, and from the user's perspective nothing happens.

## 3. Step down instead of refusal

A failed configuration does not disable auto-application, but steps down by one step, in descending order of risk: first the bus raise is removed, then the non-stock DSP divider, and only then the multiplier goes down in steps of one.

The cycle becomes self-correcting, and corrects from the end where the win is cheapest. Stopping condition — when the stock state is reached or five steps have been spent; then auto-application is disabled, and the application shows where it stopped.

## 4. Two independent loop detectors

The application counter lives in the marker. If the marker did not manage to be written to flash, this detector is gone — precisely in the case for which everything was built.

Therefore, the second detector takes data from another source: the **wall-clock time** at the moment of application is put into the marker, and the clock survives a reboot. If at startup it turns out that less than five minutes have elapsed since the last application, the previous session was short, and this is a conclusion independent of both the counter and the completeness of the marker.

## 5. Write order without which everything is in vain

The marker must land in flash **before** application: Symbian buffers writes, and a hardware reboot may occur before the data reaches the disk. Thus `RFile::Write`, `RFile::Flush`, `RFile::Close` — and only then application.

The marker resides on `C:`, in the application's private directory. Not on `E:`: this drive is unmounted in Mass Storage mode, and the marker would become unreadable when it is needed most.

## 6. Manual path: window before application, not a countdown

The application applies settings not immediately after boot, but after approximately forty seconds. This is **not** the countdown from item 2 of the previous revision: the screen is not occupied by anything, the phone is fully operational, no one is pressing anything.

The difference is in who pays. The countdown took a minute from every normal boot. The delay takes nothing: in the normal case, the user does not notice it at all, because during these forty seconds the phone is loading the rest of the system anyway.

On the other hand, in the corrupted case, these forty seconds are a working window. If the configuration crashes the device, it crashes it every time at approximately the same second, and each time forty seconds remain to open the application and disable auto-application or delete the application altogether. This is enough, and it requires neither a card nor Mass Storage mode.

## 7. Hardware kill switch: why Mass Storage is not suitable

The previous revision called USB Mass Storage mode the primary recovery path: the application resides on `E:`, the drive is handed over to the PC, the application physically cannot start. The user amendment removes this from the role of primary path, and in essence rightfully so. The phone asks the user for the connection mode, by default it is often not Mass Storage, and on the N95 8GB there is no card that could simply be removed. On the E90 there is a card, and removing it indeed works there — but a scheme that works on only one of the two devices cannot be the primary one.

Therefore, Mass Storage remains as a nice bonus where it works, while we need to look for a switch that depends neither on the mode selection dialog nor on the presence of a card.

### Candidate: external power at boot time

Idea: if the device was booted with external power connected, auto-application is skipped. The rule is easy to remember, it is identical on both devices, requires neither a card nor connection mode selection, and can be done from a powered-off phone.

**Measured 2026-09-11 on N95 8GB** (`ocprobe`, HAL dump):

```
EPowerExternal = 1
EPowerBatteryStatus -> err -5
ECaseState -> err -5
```

That is, cover state and battery status are not supported by this device at all, and as a switch they are ruled out. Meanwhile `EPowerExternal` is read without any capabilities and returned `1`.

**What this measurement does NOT prove.** A one by itself does not mean that the attribute tracks the cable: it could be a constant. A second point is needed — the same line with power disconnected. Until it is available, the switch remains a candidate rather than a solution, and taking it into code is premature.

## What remains by default

Autostart is disabled. SIS always puts an entry into the startup list (it cannot be created from the application — it is a private directory of another process, requiring `AllFiles`), but at startup the application reads its own setting and, if auto-application is disabled, quietly terminates doing nothing.

## Honest assessment

The risk-to-benefit ratio here is worse than in the rest of the application, and this revision does not change that. The gain is "no need to open the application after a reboot". The price is the only scenario in the entire project that can leave the device in a state from which the user cannot recover on their own.

What changed: the recovery path now does not rely on a hardware quirk that is not present on every phone, and the loop itself is cured automatically and without human intervention. What did not change: enabling this by default must never be done.
