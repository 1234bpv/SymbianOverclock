#ifndef OCBOOT_H
#define OCBOOT_H

#include <e32std.h>

// Configuration applied at boot by ocboot.exe, written by the application.
//
// The dangerous part of this feature is not applying a clock, it is the loop it
// can create: a fatal setting reboots the phone, the reboot applies it again,
// and there is no way in. The record is built so that the loop cannot close.
//
// A configuration becomes usable only after it has proven itself twice. When
// the user saves it, the overclock must already be live and must pass three
// rounds of stress tests, and the record is written as ECandidate, which boot
// ignores. ocboot then soaks in the background; if the phone is still alive
// after the hold period and one more test round comes back clean, the record
// becomes EArmed.
//
// At boot the order is: write EApplied with the current time, flush it to
// flash, and only then touch the registers. A phone that dies now comes back to
// a record that says EApplied, which means the previous session never reached
// the end of its hold - the configuration is refused and autostart is switched
// off. So one bad boot is the most that can happen. The application reports it
// the next time it is opened.
//
// Autostart never calls RestoreStock. Returning an already lowered bus to stock
// killed the phone four times out of four (findings 38.2); after a reboot the
// bus is stock anyway, so the only direction here is upwards, and the recovery
// path is the reboot itself.

const TUint32 KOcBootMagic   = 0x3142434F;      // 'OCB1'
const TUint32 KOcBootVersion = 1;

enum TOcBootState
    {
    EOcBootOff = 0,         // autostart disabled; nothing is applied
    EOcBootCandidate,       // saved and soaking - boot ignores this
    EOcBootArmed,           // proven; boot applies it
    EOcBootApplied          // applied this boot, hold not yet completed
    };

// Flags kept across boots, for the report shown in the application.
const TUint32 KOcBootFlagFailed  = 0x00000001;  // last boot did not survive
const TUint32 KOcBootFlagSoakBad = 0x00000002;  // soak test found errors

// Wire format. Fixed width, little endian, no 64-bit members, so that the
// layout does not depend on how the compiler pads structures.
class TOcBootRecord
    {
public:
    TUint32 iMagic;
    TUint32 iVersion;
    TUint32 iMachineUid;    // HAL EMachineUid the record was written on
    TUint32 iState;         // TOcBootState
    TUint32 iMult;          // DPLL multiplier
    TUint32 iCore;          // whole CM_CLKSEL1_CORE value
    TUint32 iMpu;           // whole CM_CLKSEL_MPU value
    TUint32 iDspDiv;        // DSP divider, 0 = leave alone
    TUint32 iDelaySec;      // wait after boot before applying
    TUint32 iHoldSec;       // hold before the record is confirmed
    TUint32 iApplyLo;       // TTime of the last apply, low 32 bits
    TUint32 iApplyHi;       // ... and high 32 bits
    TUint32 iHelpShown;     // hash of the version the help was last shown for
    TUint32 iFlags;
    TUint32 iLastResult;    // TOcBootResult of the last run
    TUint32 iReserved;
    TUint32 iCrc;           // CRC32 over every field above
    };

const TInt KOcBootRecordSize = 17 * 4;

// Why the last run of ocboot ended as it did. Without this both a clean
// confirmation and a failure to open the driver leave the record armed, and
// nothing on the phone says which of the two happened.
enum TOcBootResult
    {
    EOcBootResNone = 0,
    EOcBootResConfirmed,      // applied and survived the hold
    EOcBootResNoProfile,      // unknown phone
    EOcBootResNotReady,       // engine could not be prepared; nothing written
    EOcBootResApplyFailed,    // the write itself failed
    EOcBootResRefusedFailed,  // previous boot did not finish
    EOcBootResRefusedRecent,  // previous session ended right after confirming
    EOcBootResSoakFailed,     // soak tests found errors
    EOcBootResSoakOk          // soak passed, record promoted
    };

// Defaults for a phone that has never saved anything.
const TUint32 KOcBootDefaultDelaySec = 45;
const TUint32 KOcBootDefaultHoldSec  = 180;

// Below this many seconds since the recorded apply, the previous session is
// taken as too short to count - the second, independent loop detector, which
// works even when the record never reached flash.
const TUint32 KOcBootMinSessionSec = 300;

void  OcBootInit(TOcBootRecord& aRec);
TUint32 OcBootCrc(const TOcBootRecord& aRec);

// KErrNone, KErrNotFound when there is no file, or KErrCorrupt when the file is
// there but fails any check. A corrupt file is renamed to boot.bad, so that it
// can be looked at later and cannot be applied.
TInt OcBootLoad(TOcBootRecord& aRec);
TInt OcBootSave(const TOcBootRecord& aRec);

// Cheap hash of the version string, to tell whether the built-in help has been
// shown for this build yet.
TUint32 OcBootHash(const TDesC& aText);

// Appends one line to boot.log beside the record.
void OcBootLog(const TDesC8& aLine);

#endif
