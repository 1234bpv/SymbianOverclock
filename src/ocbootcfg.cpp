// Reading and writing the autostart record. Shared by ocgui.exe and ocboot.exe.
//
// A private directory would have been the natural home for it, but the two
// executables cannot share one: the installer refuses the whole package when two
// executables claim the same SID, and each SID gets its own private directory,
// which nothing without AllFiles may open. So the record sits in the public area
// instead, where both can reach it.
//
// That is a real loss and it is worth naming. Any process on the phone can now
// rewrite this file, where a private directory would have kept it to us. What
// stands between a forged record and a bad boot is the machine check, the CRC,
// and the two loop detectors in ocbootmain.cpp, which between them cost one boot
// at most. On a phone where the overclock itself needs an unlocked kernel driver
// to be present, a process able to write here has easier things to do.
//
// The visible file has one genuine advantage: any file manager can delete it,
// and deleting it switches autostart off without touching the application.
//
// On C: and not on E:, because E: disappears in mass storage mode, and a record
// that cannot be read is a record that cannot be switched off.
#include <e32std.h>
#include <f32file.h>
#include "ocboot.h"

_LIT(KBootFile, "C:\\Data\\Overclock\\boot.dat");
_LIT(KBootBad,  "C:\\Data\\Overclock\\boot.bad");
_LIT(KBootLog,  "C:\\Data\\Overclock\\boot.log");

void OcBootInit(TOcBootRecord& aRec)
    {
    Mem::FillZ(&aRec, sizeof(aRec));
    aRec.iMagic = KOcBootMagic;
    aRec.iVersion = KOcBootVersion;
    aRec.iState = EOcBootOff;
    aRec.iDelaySec = KOcBootDefaultDelaySec;
    aRec.iHoldSec = KOcBootDefaultHoldSec;
    }

TUint32 OcBootCrc(const TOcBootRecord& aRec)
    {
    // Plain CRC-32, reflected, polynomial 0xEDB88320, computed without a table:
    // the record is under a hundred bytes and this runs once per boot.
    const TUint8* p = (const TUint8*)&aRec;
    const TInt n = KOcBootRecordSize - 4;        // everything except iCrc
    TUint32 crc = 0xFFFFFFFFu;
    for (TInt i = 0; i < n; ++i)
        {
        crc ^= p[i];
        for (TInt b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (TUint32)(-(TInt32)(crc & 1u)));
        }
    return crc ^ 0xFFFFFFFFu;
    }

TUint32 OcBootHash(const TDesC& aText)
    {
    TUint32 h = 2166136261u;
    for (TInt i = 0; i < aText.Length(); ++i)
        {
        h ^= (TUint32)aText[i];
        h *= 16777619u;
        }
    return h;
    }

// One line per run, appended, next to the record. ocboot has no interface and
// runs before anyone is watching, so without this the only evidence of what it
// did is the record itself - which looks the same whether the clock was applied
// or the driver never opened.
void OcBootLog(const TDesC8& aLine)
    {
    RFs fs;
    if (fs.Connect() != KErrNone)
        return;
    fs.MkDirAll(KBootLog);
    RFile f;
    TInt r = f.Open(fs, KBootLog, EFileWrite);
    if (r == KErrNotFound)
        r = f.Create(fs, KBootLog, EFileWrite);
    if (r == KErrNone)
        {
        TInt pos = 0;
        if (f.Seek(ESeekEnd, pos) == KErrNone)
            {
            // Keep it small: this file is never rotated by anything else.
            if (pos > 8192)
                {
                f.SetSize(0);
                pos = 0;
                }
            TTime now;
            now.UniversalTime();
            TDateTime d = now.DateTime();
            TBuf8<64> stamp;
            stamp.Format(_L8("%04d-%02d-%02d %02d:%02d:%02d "),
                         d.Year(), d.Month() + 1, d.Day() + 1,
                         d.Hour(), d.Minute(), d.Second());
            f.Write(stamp);
            f.Write(aLine);
            f.Write(_L8("\r\n"));
            f.Flush();
            }
        f.Close();
        }
    fs.Close();
    }

TInt OcBootLoad(TOcBootRecord& aRec)
    {
    RFs fs;
    if (fs.Connect() != KErrNone)
        return KErrNotFound;
    RFile f;
    TInt r = f.Open(fs, KBootFile, EFileRead);
    if (r != KErrNone)
        {
        fs.Close();
        return KErrNotFound;
        }
    TPtr8 p((TUint8*)&aRec, 0, KOcBootRecordSize);
    r = f.Read(p);
    const TInt len = p.Length();
    f.Close();

    TBool bad = (r != KErrNone || len != KOcBootRecordSize ||
                 aRec.iMagic != KOcBootMagic ||
                 aRec.iVersion != KOcBootVersion ||
                 aRec.iCrc != OcBootCrc(aRec));
    if (bad)
        {
        // Keep the evidence, but make sure it can never be applied. A failure to
        // rename is not worth reporting: the checks above already refuse it.
        fs.Delete(KBootBad);
        fs.Rename(KBootFile, KBootBad);
        fs.Close();
        return KErrCorrupt;
        }
    fs.Close();
    return KErrNone;
    }

TInt OcBootSave(const TOcBootRecord& aRec)
    {
    TOcBootRecord rec = aRec;
    rec.iMagic = KOcBootMagic;
    rec.iVersion = KOcBootVersion;
    rec.iCrc = OcBootCrc(rec);

    RFs fs;
    TInt r = fs.Connect();
    if (r != KErrNone)
        return r;
    fs.MkDirAll(KBootFile);
    RFile f;
    r = f.Replace(fs, KBootFile, EFileWrite);
    if (r == KErrNone)
        {
        TPtrC8 p((const TUint8*)&rec, KOcBootRecordSize);
        r = f.Write(p);
        // Flush before the caller does anything with the clocks. Symbian buffers
        // writes, and a phone that dies before the data reaches flash comes back
        // to the old record - which is exactly the loop this record prevents.
        if (r == KErrNone)
            r = f.Flush();
        f.Close();
        }
    fs.Close();
    return r;
    }
