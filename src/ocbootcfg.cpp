// Reading and writing the autostart record. Shared by ocgui.exe and ocboot.exe;
// both run under SECUREID 0xE0001011, so both see the same private directory.
#include <e32std.h>
#include <f32file.h>
#include "ocboot.h"

_LIT(KBootFile, "C:\\private\\e0001011\\boot.dat");
_LIT(KBootBad,  "C:\\private\\e0001011\\boot.bad");

// The record lives on C: on purpose. E: disappears in mass storage mode, and a
// record that cannot be read is a record that cannot be switched off.

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
